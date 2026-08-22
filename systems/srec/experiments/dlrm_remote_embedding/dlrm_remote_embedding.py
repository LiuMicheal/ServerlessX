#!/usr/bin/env python3
"""Run a small official-DLRM CPU inference with one remote embedding table.

The script deliberately keeps the experiment narrow:

* the model class is imported from an unmodified facebookresearch/dlrm clone;
* three small tables are used, with only one table backed by a DMerge mapping;
* no training, GPU, TorchRec, or dataloader is involved;
* the remote adapter is a transparent CPU ``EmbeddingBag``-like module.

The DMerge syscall ABI is kept in this file only to make the experiment easy
to audit.  It is not a replacement for the kernel data plane.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib
import json
import math
import os
import resource
import sys
import time
import types
from pathlib import Path
from typing import Iterable, Optional

import numpy as np
import torch
import torch.nn as nn


DMERGE_CONNECT_SESSION = 3
DMERGE_PULL = 1
DEFAULT_BASE = 0x4FFFF5A00000
PAGE_SIZE = 4096


class ConnectRequest(ctypes.Structure):
    _fields_ = [
        ("machine_id", ctypes.c_uint),
        ("nic_id", ctypes.c_uint),
        ("gid", ctypes.c_char_p),
    ]


class PullRequest(ctypes.Structure):
    _fields_ = [
        ("heap_hint", ctypes.c_uint),
        ("machine_id", ctypes.c_uint),
        ("eager_fetch", ctypes.c_bool),
    ]


def percentile(values: Iterable[float], pct: float) -> float:
    values = sorted(values)
    if not values:
        return float("nan")
    if len(values) == 1:
        return values[0]
    index = (len(values) - 1) * pct / 100.0
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return values[lower]
    return values[lower] + (values[upper] - values[lower]) * (index - lower)


def table_array(rows: int, dim: int, table_id: int) -> np.ndarray:
    """Match the deterministic values written by dmerge_embedding_producer.c."""

    ordinal = (table_id + 1) * 1_000_000 + np.arange(
        rows * dim, dtype=np.uint64
    )
    return (ordinal.astype(np.float64) * 1.0e-6).astype(np.float32).reshape(
        rows, dim
    )


def load_official_dlrm(official_root: Path):
    """Load DLRM_Net without installing the benchmark's optional dependencies."""

    official_root = official_root.resolve()
    if str(official_root) not in sys.path:
        sys.path.insert(0, str(official_root))

    # dlrm_s_pytorch imports its training dataloader and tensorboard at module
    # import time.  Neither is needed for this inference-only path.
    sys.modules.setdefault("dlrm_data_pytorch", types.ModuleType("dlrm_data_pytorch"))
    # ``sklearn.metrics`` is used only by the benchmark's evaluation helper;
    # inference does not need the heavyweight SciPy stack.
    if "sklearn.metrics" not in sys.modules:
        sklearn = types.ModuleType("sklearn")
        sklearn.metrics = types.ModuleType("sklearn.metrics")
        sys.modules["sklearn"] = sklearn
        sys.modules["sklearn.metrics"] = sklearn.metrics
    if "torch.utils.tensorboard" not in sys.modules:
        tensorboard = types.ModuleType("torch.utils.tensorboard")

        class _SummaryWriter:
            def __init__(self, *args, **kwargs):
                pass

            def close(self):
                pass

        tensorboard.SummaryWriter = _SummaryWriter
        sys.modules["torch.utils.tensorboard"] = tensorboard

    module = importlib.import_module("dlrm_s_pytorch")
    return module.DLRM_Net


class RemoteEmbeddingBag(nn.Module):
    """CPU EmbeddingBag-compatible view over a demand-paged float table."""

    def __init__(self, weight: torch.Tensor):
        super().__init__()
        if weight.dtype != torch.float32 or weight.dim() != 2:
            raise ValueError("remote table must be a two-dimensional float32 tensor")
        # A non-persistent buffer retains the zero-copy view without putting a
        # machine address in a state_dict or serialised artifact.
        self.register_buffer("weight", weight, persistent=False)

    def forward(
        self,
        indices: torch.Tensor,
        offsets: torch.Tensor,
        per_sample_weights: Optional[torch.Tensor] = None,
    ) -> torch.Tensor:
        if per_sample_weights is not None:
            raise ValueError("the minimal experiment uses unweighted pooling")
        indices = indices.to(dtype=torch.long, device="cpu").contiguous()
        offsets = offsets.to(dtype=torch.long, device="cpu").contiguous()
        if offsets.numel() == 0:
            return self.weight.new_empty((0, self.weight.shape[1]))

        # index_select is intentionally simple and makes each selected source
        # page visible to the normal DMerge VMA fault handler.
        selected = self.weight.index_select(0, indices)
        ends = torch.cat(
            [offsets[1:], torch.tensor([indices.numel()], dtype=torch.long)]
        )
        pooled = []
        for start, end in zip(offsets.tolist(), ends.tolist()):
            chunk = selected[start:end]
            pooled.append(chunk.sum(dim=0) if end > start else self.weight[0] * 0)
        return torch.stack(pooled, dim=0)


class RemoteMapping:
    """Own the DMerge fd, ioctl requests, and ctypes lifetime for the view."""

    def __init__(self, device: str, gid: str, machine_id: int, heap_hint: int):
        self.fd = os.open(device, os.O_RDWR | os.O_CLOEXEC)
        self.gid_bytes = gid.encode("ascii")
        self.machine_id = machine_id
        self.heap_hint = heap_hint
        self.libc = ctypes.CDLL(None, use_errno=True)

    def _ioctl(self, command: int, request: ctypes.Structure) -> int:
        rc = self.libc.ioctl(self.fd, command, ctypes.byref(request))
        if rc < 0:
            err = ctypes.get_errno()
            raise OSError(err, os.strerror(err))
        return int(rc)

    def connect_and_pull(self) -> tuple[float, float]:
        connect_request = ConnectRequest(
            machine_id=self.machine_id, nic_id=0, gid=self.gid_bytes
        )
        start = time.perf_counter_ns()
        self._ioctl(DMERGE_CONNECT_SESSION, connect_request)
        connect_us = (time.perf_counter_ns() - start) / 1_000.0

        pull_request = PullRequest(
            heap_hint=self.heap_hint, machine_id=self.machine_id, eager_fetch=False
        )
        start = time.perf_counter_ns()
        self._ioctl(DMERGE_PULL, pull_request)
        pull_us = (time.perf_counter_ns() - start) / 1_000.0
        return connect_us, pull_us

    def close(self) -> None:
        os.close(self.fd)


def make_model(DLRM_Net, rows: int, dim: int, table_id: int, seed: int):
    table_count = 3
    dense_dim = dim
    bottom_hidden = max(2 * dim, 8)
    features = 1 + table_count
    interaction_count = features * (features - 1) // 2
    top_input = dim + interaction_count

    torch.manual_seed(seed)
    np.random.seed(seed)
    model = DLRM_Net(
        m_spa=dim,
        ln_emb=np.array([rows] * table_count, dtype=np.int64),
        ln_bot=np.array([dense_dim, bottom_hidden, dim], dtype=np.int64),
        ln_top=np.array([top_input, max(dim, 8), 1], dtype=np.int64),
        arch_interaction_op="dot",
        arch_interaction_itself=False,
        ndevices=-1,
        loss_function="mse",
    )
    with torch.no_grad():
        for index, embedding in enumerate(model.emb_l):
            values = torch.from_numpy(table_array(rows, dim, index))
            embedding.weight.copy_(values)
    return model


def make_dense_input(batch_size: int, dim: int) -> torch.Tensor:
    return torch.linspace(0.05, 0.95, steps=batch_size * dim, dtype=torch.float32).reshape(
        batch_size, dim
    )


def measure_forward(model, dense, offsets, indices, warm_iters: int):
    before = resource.getrusage(resource.RUSAGE_SELF)
    start = time.perf_counter_ns()
    with torch.no_grad():
        cold_output = model(dense, offsets, indices)
    cold_us = (time.perf_counter_ns() - start) / 1_000.0

    warm = []
    with torch.no_grad():
        for _ in range(warm_iters):
            start = time.perf_counter_ns()
            output = model(dense, offsets, indices)
            warm.append((time.perf_counter_ns() - start) / 1_000.0)
    after = resource.getrusage(resource.RUSAGE_SELF)
    return {
        "cold_forward_us": cold_us,
        "warm_p50_us": percentile(warm, 50),
        "warm_p95_us": percentile(warm, 95),
        "warm_qps": 1_000_000.0 / percentile(warm, 50),
        "output": cold_output.detach().cpu(),
        "minor_faults_delta": after.ru_minflt - before.ru_minflt,
        "major_faults_delta": after.ru_majflt - before.ru_majflt,
    }


def run(args) -> dict:
    torch.set_num_threads(1)
    DLRM_Net = load_official_dlrm(Path(args.official_root))
    local_model = make_model(DLRM_Net, args.rows, args.dim, args.table_id, args.seed)
    remote_model = make_model(DLRM_Net, args.rows, args.dim, args.table_id, args.seed)
    remote_model.bot_l.load_state_dict(local_model.bot_l.state_dict())
    remote_model.top_l.load_state_dict(local_model.top_l.state_dict())

    dense = make_dense_input(args.batch_size, args.dim)
    offsets = [torch.arange(args.batch_size, dtype=torch.long) for _ in range(3)]
    index_values = np.linspace(0, args.rows - 1, args.batch_size, dtype=np.int64)
    indices = [torch.from_numpy(index_values.copy()) for _ in range(3)]

    local_stats = measure_forward(
        local_model, dense, offsets, indices, args.warm_iters
    )
    result = {
        "status": "PASS",
        "mode": args.mode,
        "official_root": str(Path(args.official_root).resolve()),
        "rows": args.rows,
        "dim": args.dim,
        "table_id": args.table_id,
        "table_bytes": args.rows * args.dim * 4,
        "batch_size": args.batch_size,
        "warm_iters": args.warm_iters,
        "seed": args.seed,
        "remote_pages_expected": len(
            {(int(row) * args.dim * 4) // PAGE_SIZE for row in index_values}
        ),
        "local": {
            key: value
            for key, value in local_stats.items()
            if key != "output"
        },
    }

    mapping = None
    if args.mode == "remote":
        mapping = RemoteMapping(
            args.device, args.gid, args.machine_id, args.heap_hint
        )
        try:
            connect_us, pull_us = mapping.connect_and_pull()
            count = args.rows * args.dim
            float_array = ctypes.c_float * count
            c_view = float_array.from_address(args.base)
            np_view = np.ctypeslib.as_array(c_view).reshape(args.rows, args.dim)
            remote_weight = torch.from_numpy(np_view)
            remote_model.emb_l[args.table_id] = RemoteEmbeddingBag(remote_weight)
            remote_stats = measure_forward(
                remote_model, dense, offsets, indices, args.warm_iters
            )
            difference = (remote_stats["output"] - local_stats["output"]).abs()
            result.update(
                {
                    "device": args.device,
                    "gid": args.gid,
                    "machine_id": args.machine_id,
                    "heap_hint": args.heap_hint,
                    "base": hex(args.base),
                    "connect_us": connect_us,
                    "pull_us": pull_us,
                    "remote": {
                        key: value
                        for key, value in remote_stats.items()
                        if key != "output"
                    },
                    "output_max_abs_error": float(difference.max().item()),
                    "output_mean_abs_error": float(difference.mean().item()),
                    "local_output_checksum": float(local_stats["output"].sum().item()),
                    "remote_output_checksum": float(remote_stats["output"].sum().item()),
                }
            )
            if result["output_max_abs_error"] > args.tolerance:
                result["status"] = "FAIL"
        finally:
            mapping.close()
    else:
        result["local_output_checksum"] = float(local_stats["output"].sum().item())

    return result


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("local", "remote"), default="local")
    parser.add_argument(
        "--official-root",
        default=os.environ.get("DLRM_OFFICIAL_ROOT", "/home/liumx/Code/dlrm-facebookresearch"),
    )
    parser.add_argument("--rows", type=int, default=16384)
    parser.add_argument("--dim", type=int, default=16)
    parser.add_argument("--table-id", type=int, default=0)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--warm-iters", type=int, default=20)
    parser.add_argument("--seed", type=int, default=20260822)
    parser.add_argument("--tolerance", type=float, default=1.0e-5)
    parser.add_argument("--device", default="/dev/mitosis-syscalls")
    parser.add_argument("--gid")
    parser.add_argument("--machine-id", type=int, default=1)
    parser.add_argument("--heap-hint", type=int, default=1)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=DEFAULT_BASE)
    args = parser.parse_args()
    if args.rows <= 0 or args.dim <= 0 or args.batch_size <= 0:
        parser.error("rows, dim, and batch-size must be positive")
    if args.rows * args.dim * 4 % PAGE_SIZE != 0:
        parser.error("rows * dim * 4 must be a multiple of 4096")
    if args.mode == "remote" and not args.gid:
        parser.error("--gid is required in remote mode")
    return args


if __name__ == "__main__":
    parsed = parse_args()
    result = run(parsed)
    print("RESULT_JSON=" + json.dumps(result, sort_keys=True))
    raise SystemExit(0 if result["status"] == "PASS" else 1)
