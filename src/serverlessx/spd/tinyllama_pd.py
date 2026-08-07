#!/usr/bin/env python3
"""Minimal real-model 1P1D workload for TinyLlama artifact experiments.

The decode role listens first.  The prefill role transfers a canonical legacy
KV cache over a length-prefixed TCP stream.  This file intentionally contains
no cluster orchestration so the evaluator-facing runner can own process and
evidence lifecycle.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import os
import re
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Any, Callable, NamedTuple, Optional, Protocol, Sequence


EVENT_SCHEMA = "serverlesspd.real-pd-event.v1"
KV_SCHEMA = "serverlesspd.real-kv.v1"
KV_LAYOUT = "legacy-pkv-l0k-l0v-fp16-le-v1"
TRANSPORT = "tcp_host_staging"
EXPECTED_LAYERS = 22
EXPECTED_KV_HEADS = 4
EXPECTED_HEAD_DIM = 64
EXPECTED_PROMPT_TOKENS = 128
EXPECTED_OUTPUT_TOKENS = 8
MAX_HEADER_BYTES = 1 << 20
MAX_PAYLOAD_BYTES = 1 << 30
LENGTH_PREFIX = struct.Struct("!Q")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
REQUEST_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}")


class WorkloadError(RuntimeError):
    """A contract, transport, or model-runtime failure."""


class ModelInfo(NamedTuple):
    model_type: str
    model_class: str
    dtype: str
    layer_count: int
    kv_head_count: int
    head_dim: int
    vocab_size: int


class PrefillOutput(NamedTuple):
    first_token_id: int
    cache: Any
    gpu_ms: float


class PackedKv(NamedTuple):
    payload: bytes
    tensors: list[dict[str, Any]]
    d2h_ms: float
    pack_ms: float


class DecodeOutput(NamedTuple):
    token_ids: list[int]
    gpu_ms: float
    itl_ms: list[float]


class ExpectedKv(NamedTuple):
    model_id: str
    model_sha256: str
    tokenizer_sha256: str
    request_id: str
    epoch: int
    prompt_length: int
    prompt_digest_sha256: str
    output_tokens: int
    model_info: ModelInfo


class ModelRuntime(Protocol):
    info: ModelInfo
    model_load_ms: float

    def prefill(self, prompt_token_ids: Sequence[int]) -> PrefillOutput: ...

    def pack(self, cache: Any, prompt_length: int) -> PackedKv: ...

    def unpack(
        self, payload: bytes, tensors: Sequence[dict[str, Any]]
    ) -> tuple[Any, float]: ...

    def decode(
        self, cache: Any, first_token_id: int, output_tokens: int
    ) -> DecodeOutput: ...


class Sender(Protocol):
    def send(self, header: dict[str, Any], payload: bytes) -> float: ...


class Receiver(Protocol):
    @property
    def listen_ip(self) -> str: ...

    @property
    def port(self) -> int: ...

    def receive_header(self) -> tuple[socket.socket, dict[str, Any], int]: ...

    def receive_payload(
        self, connection: socket.socket, expected_bytes: int
    ) -> tuple[bytes, float]: ...

    def close(self) -> None: ...


def _now_ns() -> int:
    return time.perf_counter_ns()


def _elapsed_ms(start_ns: int, end_ns: int) -> float:
    return (end_ns - start_ns) / 1_000_000.0


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _require_int(value: Any, field: str, minimum: int = 0) -> int:
    if not _is_int(value) or value < minimum:
        raise WorkloadError(f"{field} must be an integer >= {minimum}")
    return value


def _require_float(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise WorkloadError(f"{field} must be a finite number")
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise WorkloadError(f"{field} must be a finite number >= 0")
    return result


def _require_sha256(value: Any, field: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise WorkloadError(f"{field} must be a lowercase SHA-256")
    return value


def _require_exact_keys(value: dict[str, Any], expected: set[str], field: str) -> None:
    actual = set(value)
    if actual != expected:
        raise WorkloadError(
            f"{field} keys differ from contract: expected {sorted(expected)}, "
            f"got {sorted(actual)}"
        )


def canonical_json_bytes(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("ascii")
    except (TypeError, ValueError) as exc:
        raise WorkloadError(f"value is not canonical JSON: {exc}") from exc


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def token_digest(token_ids: Sequence[int]) -> str:
    return sha256_bytes(canonical_json_bytes(list(token_ids)))


PROMPT_SCHEMA = "serverlesspd.prompt-token-ids.v1"
PROMPT_KEYS = {"schema", "text", "token_ids", "tokenizer_sha256"}


def load_prompt_token_ids(
    path: Path,
    expected_length: int,
    expected_tokenizer_sha256: Optional[str] = None,
) -> list[int]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise WorkloadError(f"cannot read prompt token IDs: {exc}") from exc
    if len(raw) > MAX_HEADER_BYTES:
        raise WorkloadError("prompt token ID file is unreasonably large")
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise WorkloadError(f"prompt token ID file is not valid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise WorkloadError("prompt token ID document must be a JSON object")
    _require_exact_keys(value, PROMPT_KEYS, "prompt token ID document")
    if value["schema"] != PROMPT_SCHEMA:
        raise WorkloadError("prompt token ID document schema mismatch")
    if not isinstance(value["text"], str) or not value["text"]:
        raise WorkloadError("prompt token ID document text must be non-empty")
    tokenizer_sha256 = _require_sha256(
        value["tokenizer_sha256"], "prompt tokenizer_sha256"
    )
    if (
        expected_tokenizer_sha256 is not None
        and tokenizer_sha256 != expected_tokenizer_sha256
    ):
        raise WorkloadError(
            "prompt token IDs were produced by a different tokenizer artifact"
        )
    value = value["token_ids"]
    if not isinstance(value, list) or len(value) != expected_length:
        raise WorkloadError(
            f"prompt token ID file must contain exactly {expected_length} IDs"
        )
    if any(not _is_int(token) or token < 0 for token in value):
        raise WorkloadError("prompt token IDs must be non-negative integers")
    return value


def validate_tinyllama_info(info: ModelInfo) -> None:
    if info.model_type != "llama" or info.model_class != "LlamaForCausalLM":
        raise WorkloadError("model must be a Hugging Face LlamaForCausalLM")
    if info.dtype != "float16":
        raise WorkloadError("model runtime must use FP16 weights")
    expected = (EXPECTED_LAYERS, EXPECTED_KV_HEADS, EXPECTED_HEAD_DIM)
    actual = (info.layer_count, info.kv_head_count, info.head_dim)
    if actual != expected:
        raise WorkloadError(
            "model is not the pinned TinyLlama architecture: "
            f"expected layers/KV-heads/head-dim {expected}, got {actual}"
        )
    if info.vocab_size <= 0:
        raise WorkloadError("model vocabulary size must be positive")


def _legacy_cache(cache: Any) -> tuple[Any, ...]:
    if hasattr(cache, "to_legacy_cache"):
        cache = cache.to_legacy_cache()
    if not isinstance(cache, (tuple, list)):
        raise WorkloadError("model did not return a legacy-compatible KV cache")
    result = tuple(cache)
    if not result:
        raise WorkloadError("model returned an empty KV cache")
    return result


def pack_legacy_pkv(
    cache: Any,
    prompt_length: int,
    info: ModelInfo,
    to_host: Callable[[Any], Any],
    to_le_fp16_bytes: Callable[[Any], bytes],
    clock_ns: Callable[[], int] = _now_ns,
) -> PackedKv:
    """Pack L0K,L0V,... in a deterministic, gap-free FP16 byte layout."""
    legacy = _legacy_cache(cache)
    if len(legacy) != info.layer_count:
        raise WorkloadError(
            f"KV cache has {len(legacy)} layers; expected {info.layer_count}"
        )
    expected_shape = (1, info.kv_head_count, prompt_length, info.head_dim)
    ordered: list[tuple[str, int, str, Any]] = []
    for layer_index, layer in enumerate(legacy):
        if not isinstance(layer, (tuple, list)) or len(layer) != 2:
            raise WorkloadError(f"KV layer {layer_index} must contain exactly K and V")
        for kind, tensor in zip(("K", "V"), layer):
            try:
                shape = tuple(int(dimension) for dimension in tensor.shape)
            except (AttributeError, TypeError, ValueError) as exc:
                raise WorkloadError(
                    f"KV tensor L{layer_index}{kind} has no valid shape"
                ) from exc
            if shape != expected_shape:
                raise WorkloadError(
                    f"KV tensor L{layer_index}{kind} shape {shape} != {expected_shape}"
                )
            dtype = str(getattr(tensor, "dtype", ""))
            if dtype not in {"torch.float16", "float16"}:
                raise WorkloadError(
                    f"KV tensor L{layer_index}{kind} must have FP16 dtype, got {dtype}"
                )
            ordered.append((f"L{layer_index}{kind}", layer_index, kind, tensor))

    d2h_start = clock_ns()
    host_tensors = [to_host(tensor) for _, _, _, tensor in ordered]
    d2h_end = clock_ns()

    pack_start = clock_ns()
    chunks: list[bytes] = []
    tensors: list[dict[str, Any]] = []
    offset = 0
    expected_tensor_bytes = math.prod(expected_shape) * 2
    for (name, layer_index, kind, _), host_tensor in zip(ordered, host_tensors):
        chunk = to_le_fp16_bytes(host_tensor)
        if not isinstance(chunk, bytes) or len(chunk) != expected_tensor_bytes:
            encoded_length = len(chunk) if isinstance(chunk, bytes) else "non-bytes"
            raise WorkloadError(
                f"KV tensor {name} encoded to {encoded_length} bytes; "
                f"expected {expected_tensor_bytes}"
            )
        tensors.append(
            {
                "name": name,
                "layer": layer_index,
                "kind": kind,
                "shape": list(expected_shape),
                "offset_bytes": offset,
                "nbytes": len(chunk),
            }
        )
        chunks.append(chunk)
        offset += len(chunk)
    payload = b"".join(chunks)
    pack_end = clock_ns()
    return PackedKv(
        payload=payload,
        tensors=tensors,
        d2h_ms=_elapsed_ms(d2h_start, d2h_end),
        pack_ms=_elapsed_ms(pack_start, pack_end),
    )


HEADER_KEYS = {
    "schema",
    "schema_version",
    "layout",
    "dtype",
    "model_id",
    "model_sha256",
    "tokenizer_sha256",
    "request_id",
    "epoch",
    "prompt_length",
    "prompt_digest_sha256",
    "output_tokens",
    "layer_count",
    "kv_head_count",
    "head_dim",
    "tensors",
    "payload_bytes",
    "payload_sha256",
    "first_token_id",
}
TENSOR_KEYS = {"name", "layer", "kind", "shape", "offset_bytes", "nbytes"}


def build_kv_header(
    expected: ExpectedKv,
    packed: PackedKv,
    first_token_id: int,
) -> dict[str, Any]:
    if not _is_int(first_token_id) or not 0 <= first_token_id < expected.model_info.vocab_size:
        raise WorkloadError("first token ID is outside the model vocabulary")
    header = {
        "schema": KV_SCHEMA,
        "schema_version": 1,
        "layout": KV_LAYOUT,
        "dtype": "float16",
        "model_id": expected.model_id,
        "model_sha256": expected.model_sha256,
        "tokenizer_sha256": expected.tokenizer_sha256,
        "request_id": expected.request_id,
        "epoch": expected.epoch,
        "prompt_length": expected.prompt_length,
        "prompt_digest_sha256": expected.prompt_digest_sha256,
        "output_tokens": expected.output_tokens,
        "layer_count": expected.model_info.layer_count,
        "kv_head_count": expected.model_info.kv_head_count,
        "head_dim": expected.model_info.head_dim,
        "tensors": packed.tensors,
        "payload_bytes": len(packed.payload),
        "payload_sha256": sha256_bytes(packed.payload),
        "first_token_id": first_token_id,
    }
    validate_kv_header(header, expected)
    return header


def validate_kv_header(header: Any, expected: ExpectedKv) -> dict[str, Any]:
    if not isinstance(header, dict):
        raise WorkloadError("KV header must be a JSON object")
    _require_exact_keys(header, HEADER_KEYS, "KV header")
    exact_values = {
        "schema": KV_SCHEMA,
        "schema_version": 1,
        "layout": KV_LAYOUT,
        "dtype": "float16",
        "model_id": expected.model_id,
        "model_sha256": expected.model_sha256,
        "tokenizer_sha256": expected.tokenizer_sha256,
        "request_id": expected.request_id,
        "epoch": expected.epoch,
        "prompt_length": expected.prompt_length,
        "prompt_digest_sha256": expected.prompt_digest_sha256,
        "output_tokens": expected.output_tokens,
        "layer_count": expected.model_info.layer_count,
        "kv_head_count": expected.model_info.kv_head_count,
        "head_dim": expected.model_info.head_dim,
    }
    for key, wanted in exact_values.items():
        if header[key] != wanted or (
            isinstance(wanted, int) and not _is_int(header[key])
        ):
            raise WorkloadError(
                f"KV header {key} mismatch: expected {wanted!r}, got {header[key]!r}"
            )
    _require_sha256(header["model_sha256"], "KV header model_sha256")
    _require_sha256(header["tokenizer_sha256"], "KV header tokenizer_sha256")
    _require_sha256(header["prompt_digest_sha256"], "KV header prompt digest")
    _require_sha256(header["payload_sha256"], "KV header payload digest")
    first_token_id = _require_int(header["first_token_id"], "first_token_id")
    if first_token_id >= expected.model_info.vocab_size:
        raise WorkloadError("KV header first token ID is outside the vocabulary")

    tensors = header["tensors"]
    expected_count = expected.model_info.layer_count * 2
    if not isinstance(tensors, list) or len(tensors) != expected_count:
        raise WorkloadError(f"KV header must describe exactly {expected_count} tensors")
    expected_shape = [
        1,
        expected.model_info.kv_head_count,
        expected.prompt_length,
        expected.model_info.head_dim,
    ]
    expected_tensor_bytes = math.prod(expected_shape) * 2
    offset = 0
    for tensor_index, tensor in enumerate(tensors):
        field = f"KV header tensors[{tensor_index}]"
        if not isinstance(tensor, dict):
            raise WorkloadError(f"{field} must be an object")
        _require_exact_keys(tensor, TENSOR_KEYS, field)
        layer = tensor_index // 2
        kind = "K" if tensor_index % 2 == 0 else "V"
        wanted = {
            "name": f"L{layer}{kind}",
            "layer": layer,
            "kind": kind,
            "shape": expected_shape,
            "offset_bytes": offset,
            "nbytes": expected_tensor_bytes,
        }
        for key, value in wanted.items():
            if tensor[key] != value or (
                isinstance(value, int) and not _is_int(tensor[key])
            ):
                raise WorkloadError(
                    f"{field}.{key} mismatch: expected {value!r}, got {tensor[key]!r}"
                )
        offset += expected_tensor_bytes
    payload_bytes = _require_int(header["payload_bytes"], "payload_bytes", 1)
    if payload_bytes != offset:
        raise WorkloadError(
            f"KV payload size {payload_bytes} does not match tensor layout {offset}"
        )
    if payload_bytes > MAX_PAYLOAD_BYTES:
        raise WorkloadError("KV payload exceeds the receiver safety limit")
    return header


def validate_payload(header: dict[str, Any], payload: bytes) -> None:
    if len(payload) != header["payload_bytes"]:
        raise WorkloadError(
            f"KV payload length mismatch: expected {header['payload_bytes']}, "
            f"got {len(payload)}"
        )
    actual = sha256_bytes(payload)
    if actual != header["payload_sha256"]:
        raise WorkloadError(
            f"KV payload SHA-256 mismatch: expected {header['payload_sha256']}, "
            f"got {actual}"
        )


def _json_object_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise WorkloadError(f"KV header contains duplicate key {key!r}")
        result[key] = value
    return result


def decode_header(raw: bytes) -> dict[str, Any]:
    try:
        value = json.loads(raw, object_pairs_hook=_json_object_no_duplicates)
    except WorkloadError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise WorkloadError(f"KV header is not valid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise WorkloadError("KV header must decode to an object")
    return value


def recv_exact(connection: socket.socket, length: int) -> bytes:
    _require_int(length, "receive length")
    chunks: list[bytes] = []
    remaining = length
    while remaining:
        chunk = connection.recv(min(remaining, 1 << 20))
        if not chunk:
            received = length - remaining
            raise WorkloadError(
                f"TCP stream ended after {received} of {length} expected bytes"
            )
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_framed(connection: socket.socket, header: dict[str, Any], payload: bytes) -> None:
    raw_header = canonical_json_bytes(header)
    if not raw_header or len(raw_header) > MAX_HEADER_BYTES:
        raise WorkloadError("KV header length is outside the safety limit")
    if not payload or len(payload) > MAX_PAYLOAD_BYTES:
        raise WorkloadError("KV payload length is outside the safety limit")
    connection.sendall(LENGTH_PREFIX.pack(len(raw_header)))
    connection.sendall(raw_header)
    connection.sendall(LENGTH_PREFIX.pack(len(payload)))
    connection.sendall(payload)


def receive_framed_header(connection: socket.socket) -> tuple[dict[str, Any], int]:
    raw_length = recv_exact(connection, LENGTH_PREFIX.size)
    (header_length,) = LENGTH_PREFIX.unpack(raw_length)
    if header_length == 0 or header_length > MAX_HEADER_BYTES:
        raise WorkloadError(f"invalid KV header length {header_length}")
    header = decode_header(recv_exact(connection, header_length))
    raw_payload_length = recv_exact(connection, LENGTH_PREFIX.size)
    (payload_length,) = LENGTH_PREFIX.unpack(raw_payload_length)
    if payload_length == 0 or payload_length > MAX_PAYLOAD_BYTES:
        raise WorkloadError(f"invalid KV payload length {payload_length}")
    return header, payload_length


class TcpSender:
    def __init__(self, peer_ip: str, port: int, timeout_sec: float):
        self.peer_ip = peer_ip
        self.port = port
        self.timeout_sec = timeout_sec

    def send(self, header: dict[str, Any], payload: bytes) -> float:
        start = _now_ns()
        try:
            with socket.create_connection(
                (self.peer_ip, self.port), timeout=self.timeout_sec
            ) as connection:
                connection.settimeout(self.timeout_sec)
                send_framed(connection, header, payload)
        except (OSError, TimeoutError) as exc:
            raise WorkloadError(f"TCP KV send failed: {exc}") from exc
        return _elapsed_ms(start, _now_ns())


class TcpReceiver:
    def __init__(self, listen_ip: str, port: int, timeout_sec: float):
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self._socket.settimeout(timeout_sec)
            self._socket.bind((listen_ip, port))
            self._socket.listen(1)
        except OSError:
            self._socket.close()
            raise
        bound_ip, bound_port = self._socket.getsockname()
        self._listen_ip = str(bound_ip)
        self._port = int(bound_port)
        self._timeout_sec = timeout_sec
        self._receive_start_ns: Optional[int] = None

    @property
    def listen_ip(self) -> str:
        return self._listen_ip

    @property
    def port(self) -> int:
        return self._port

    def receive_header(self) -> tuple[socket.socket, dict[str, Any], int]:
        connection: Optional[socket.socket] = None
        try:
            connection, _ = self._socket.accept()
            # Exclude time waiting for P to finish prefill.  tcp_ms starts once
            # the connection exists and therefore measures only the handoff.
            self._receive_start_ns = _now_ns()
            connection.settimeout(self._timeout_sec)
            header, payload_length = receive_framed_header(connection)
            return connection, header, payload_length
        except (OSError, TimeoutError) as exc:
            if connection is not None:
                connection.close()
            raise WorkloadError(f"TCP KV receive failed: {exc}") from exc
        except Exception:
            if connection is not None:
                connection.close()
            raise

    def receive_payload(
        self, connection: socket.socket, expected_bytes: int
    ) -> tuple[bytes, float]:
        if self._receive_start_ns is None:
            raise WorkloadError("receive_payload called before receive_header")
        try:
            payload = recv_exact(connection, expected_bytes)
        except (OSError, TimeoutError) as exc:
            raise WorkloadError(f"TCP KV receive failed: {exc}") from exc
        return payload, _elapsed_ms(self._receive_start_ns, _now_ns())

    def close(self) -> None:
        self._socket.close()


class HfLlamaRuntime:
    """Thin torch/transformers boundary; tests replace this entire class."""

    def __init__(self, model_path: Path):
        load_start = _now_ns()
        try:
            import numpy as np
            import torch
            from transformers import LlamaForCausalLM
        except ImportError as exc:
            raise WorkloadError(f"real P/D runtime dependency is missing: {exc}") from exc
        if not torch.cuda.is_available():
            raise WorkloadError("CUDA is required for the real TinyLlama workload")
        if not model_path.is_dir():
            raise WorkloadError(f"model path is not a local directory: {model_path}")
        try:
            model = LlamaForCausalLM.from_pretrained(
                str(model_path),
                local_files_only=True,
                torch_dtype=torch.float16,
            )
            model = model.to(device="cuda", dtype=torch.float16)
            model.eval()
            torch.cuda.synchronize()
        except Exception as exc:
            raise WorkloadError(f"failed to load local LlamaForCausalLM: {exc}") from exc
        config = model.config
        attention_heads = int(config.num_attention_heads)
        hidden_size = int(config.hidden_size)
        if attention_heads <= 0 or hidden_size % attention_heads != 0:
            raise WorkloadError("model has an invalid attention-head configuration")
        first_parameter = next(model.parameters(), None)
        info = ModelInfo(
            model_type=str(config.model_type),
            model_class=type(model).__name__,
            dtype=(
                "float16"
                if first_parameter is not None and first_parameter.dtype == torch.float16
                else str(getattr(first_parameter, "dtype", "unknown"))
            ),
            layer_count=int(config.num_hidden_layers),
            kv_head_count=int(config.num_key_value_heads),
            head_dim=hidden_size // attention_heads,
            vocab_size=int(config.vocab_size),
        )
        validate_tinyllama_info(info)
        self.model = model
        self.torch = torch
        self.np = np
        self.info = info
        self.model_load_ms = _elapsed_ms(load_start, _now_ns())

    def _gpu_call(self, function: Callable[[], Any]) -> tuple[Any, float]:
        timing_mode = os.environ.get("SERVERLESSPD_CUDA_TIMING_MODE", "event")
        if timing_mode == "host_sync":
            self.torch.cuda.synchronize()
            started = _now_ns()
            result = function()
            self.torch.cuda.synchronize()
            return result, _elapsed_ms(started, _now_ns())
        if timing_mode != "event":
            raise WorkloadError(
                "SERVERLESSPD_CUDA_TIMING_MODE must be event or host_sync"
            )
        start = self.torch.cuda.Event(enable_timing=True)
        end = self.torch.cuda.Event(enable_timing=True)
        self.torch.cuda.synchronize()
        start.record()
        result = function()
        end.record()
        end.synchronize()
        return result, float(start.elapsed_time(end))

    def prefill(self, prompt_token_ids: Sequence[int]) -> PrefillOutput:
        if any(token >= self.info.vocab_size for token in prompt_token_ids):
            raise WorkloadError("prompt contains a token outside the model vocabulary")
        input_ids = self.torch.tensor(
            [list(prompt_token_ids)], dtype=self.torch.long, device="cuda"
        )

        def call() -> Any:
            with self.torch.inference_mode():
                return self.model(input_ids=input_ids, use_cache=True, return_dict=True)

        output, gpu_ms = self._gpu_call(call)
        if output.past_key_values is None:
            raise WorkloadError("model prefill did not return a KV cache")
        last_logits = output.logits[:, -1, :].detach().to(
            device="cpu", non_blocking=False
        ).numpy()
        first_token = int(self.np.argmax(last_logits, axis=-1)[0])
        return PrefillOutput(first_token, output.past_key_values, gpu_ms)

    def pack(self, cache: Any, prompt_length: int) -> PackedKv:
        def to_host(tensor: Any) -> Any:
            return tensor.detach().contiguous().to(device="cpu", non_blocking=False)

        def to_bytes(tensor: Any) -> bytes:
            array = tensor.numpy()
            if array.dtype != self.np.dtype(self.np.float16):
                raise WorkloadError(f"host KV tensor dtype is not FP16: {array.dtype}")
            return array.astype(self.np.dtype("<f2"), copy=False).tobytes(order="C")

        return pack_legacy_pkv(
            cache, prompt_length, self.info, to_host=to_host, to_le_fp16_bytes=to_bytes
        )

    def unpack(
        self, payload: bytes, tensors: Sequence[dict[str, Any]]
    ) -> tuple[Any, float]:
        h2d_start = _now_ns()
        layers: list[tuple[Any, Any]] = []
        for layer_index in range(self.info.layer_count):
            pair: list[Any] = []
            for tensor in tensors[layer_index * 2 : layer_index * 2 + 2]:
                offset = int(tensor["offset_bytes"])
                nbytes = int(tensor["nbytes"])
                array = self.np.frombuffer(
                    payload, dtype=self.np.dtype("<f2"), count=nbytes // 2, offset=offset
                ).copy()
                array = array.reshape(tuple(tensor["shape"]))
                pair.append(
                    self.torch.from_numpy(array)
                    .to(device="cuda", dtype=self.torch.float16, non_blocking=False)
                    .contiguous()
                )
            layers.append((pair[0], pair[1]))
        self.torch.cuda.synchronize()
        return tuple(layers), _elapsed_ms(h2d_start, _now_ns())

    def decode(
        self, cache: Any, first_token_id: int, output_tokens: int
    ) -> DecodeOutput:
        if output_tokens < 1:
            raise WorkloadError("output token count must be positive")
        token_ids = [first_token_id]
        itl_ms: list[float] = []
        past = cache
        current = first_token_id
        for _ in range(output_tokens - 1):
            input_ids = self.torch.tensor([[current]], dtype=self.torch.long, device="cuda")

            def call() -> Any:
                with self.torch.inference_mode():
                    return self.model(
                        input_ids=input_ids,
                        past_key_values=past,
                        use_cache=True,
                        return_dict=True,
                    )

            output, gpu_ms = self._gpu_call(call)
            if output.past_key_values is None:
                raise WorkloadError("model decode did not return an updated KV cache")
            current = int(self.torch.argmax(output.logits[:, -1, :], dim=-1).item())
            token_ids.append(current)
            itl_ms.append(gpu_ms)
            past = output.past_key_values
        return DecodeOutput(token_ids, sum(itl_ms), itl_ms)


def _base_result(
    args: argparse.Namespace,
    prompt_ids: Sequence[int],
    runtime: ModelRuntime,
    role: str,
) -> dict[str, Any]:
    return {
        "schema": EVENT_SCHEMA,
        "schema_version": 1,
        "event": "result",
        "status": "pass",
        "role": role,
        "request_id": args.request_id,
        "epoch": args.epoch,
        "model_id": args.model_id,
        "model_sha256": args.model_sha256,
        "tokenizer_sha256": args.tokenizer_sha256,
        "model_class": runtime.info.model_class,
        "model_dtype": runtime.info.dtype,
        "prompt_length": len(prompt_ids),
        "prompt_digest_sha256": token_digest(prompt_ids),
        "output_tokens": args.output_tokens,
        "transport": TRANSPORT if role in {"prefill", "decode"} else None,
        "kv_layout": KV_LAYOUT if role in {"prefill", "decode"} else None,
        "model_load_ms": _require_float(runtime.model_load_ms, "model_load_ms"),
        "prefill_gpu_ms": None,
        "pack_ms": None,
        "d2h_ms": None,
        "tcp_ms": None,
        "h2d_ms": None,
        "decode_gpu_ms": None,
        "decode_itl_ms": [],
        "first_token_id": None,
        "token_ids": [],
        "token_digest_sha256": None,
        "kv_header_sha256": None,
        "kv_payload_bytes": None,
        "kv_payload_sha256": None,
    }


def _expected(args: argparse.Namespace, prompt_ids: Sequence[int], info: ModelInfo) -> ExpectedKv:
    return ExpectedKv(
        model_id=args.model_id,
        model_sha256=args.model_sha256,
        tokenizer_sha256=args.tokenizer_sha256,
        request_id=args.request_id,
        epoch=args.epoch,
        prompt_length=len(prompt_ids),
        prompt_digest_sha256=token_digest(prompt_ids),
        output_tokens=args.output_tokens,
        model_info=info,
    )


def run_reference(
    args: argparse.Namespace, prompt_ids: Sequence[int], runtime: ModelRuntime
) -> dict[str, Any]:
    prefill = runtime.prefill(prompt_ids)
    decode = runtime.decode(prefill.cache, prefill.first_token_id, args.output_tokens)
    result = _base_result(args, prompt_ids, runtime, "reference")
    result.update(
        {
            "prefill_gpu_ms": _require_float(prefill.gpu_ms, "prefill_gpu_ms"),
            "decode_gpu_ms": _require_float(decode.gpu_ms, "decode_gpu_ms"),
            "decode_itl_ms": [
                _require_float(value, "decode_itl_ms") for value in decode.itl_ms
            ],
            "first_token_id": prefill.first_token_id,
            "token_ids": decode.token_ids,
            "token_digest_sha256": token_digest(decode.token_ids),
        }
    )
    validate_result_event(result, args.output_tokens)
    return result


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    try:
        with temporary.open("xb") as output:
            os.chmod(temporary, 0o600)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary.unlink()


def export_kv(
    payload_path: Path,
    header_path: Path,
    header: dict[str, Any],
    payload: bytes,
) -> None:
    if payload_path.resolve() == header_path.resolve():
        raise WorkloadError("KV payload and header export paths must differ")
    validate_payload(header, payload)
    _atomic_write(payload_path, payload)
    try:
        _atomic_write(header_path, canonical_json_bytes(header) + b"\n")
    except Exception:
        with contextlib.suppress(FileNotFoundError):
            payload_path.unlink()
        raise


def run_prefill(
    args: argparse.Namespace,
    prompt_ids: Sequence[int],
    runtime: ModelRuntime,
    sender: Sender,
) -> dict[str, Any]:
    prefill = runtime.prefill(prompt_ids)
    packed = runtime.pack(prefill.cache, len(prompt_ids))
    expected = _expected(args, prompt_ids, runtime.info)
    header = build_kv_header(expected, packed, prefill.first_token_id)
    if args.export_kv is not None:
        header_path = args.export_kv_header or Path(str(args.export_kv) + ".json")
        export_kv(args.export_kv, header_path, header, packed.payload)
    tcp_ms = sender.send(header, packed.payload)
    result = _base_result(args, prompt_ids, runtime, "prefill")
    token_ids = [prefill.first_token_id]
    result.update(
        {
            "prefill_gpu_ms": _require_float(prefill.gpu_ms, "prefill_gpu_ms"),
            "pack_ms": _require_float(packed.pack_ms, "pack_ms"),
            "d2h_ms": _require_float(packed.d2h_ms, "d2h_ms"),
            "tcp_ms": _require_float(tcp_ms, "tcp_ms"),
            "first_token_id": prefill.first_token_id,
            "token_ids": token_ids,
            "token_digest_sha256": token_digest(token_ids),
            "kv_header_sha256": sha256_bytes(canonical_json_bytes(header)),
            "kv_payload_bytes": len(packed.payload),
            "kv_payload_sha256": header["payload_sha256"],
        }
    )
    validate_result_event(result, args.output_tokens)
    return result


def decode_ready_event(
    args: argparse.Namespace, runtime: ModelRuntime, receiver: Receiver
) -> dict[str, Any]:
    return {
        "schema": EVENT_SCHEMA,
        "schema_version": 1,
        "event": "model_ready",
        "status": "pass",
        "role": "decode",
        "request_id": args.request_id,
        "epoch": args.epoch,
        "model_id": args.model_id,
        "model_sha256": args.model_sha256,
        "tokenizer_sha256": args.tokenizer_sha256,
        "model_class": runtime.info.model_class,
        "model_dtype": runtime.info.dtype,
        "model_load_ms": _require_float(runtime.model_load_ms, "model_load_ms"),
        "listen_ip": receiver.listen_ip,
        "port": receiver.port,
        "pid": os.getpid(),
    }


def run_decode(
    args: argparse.Namespace,
    prompt_ids: Sequence[int],
    runtime: ModelRuntime,
    receiver: Receiver,
) -> dict[str, Any]:
    connection: Optional[socket.socket] = None
    try:
        connection, header, framed_payload_bytes = receiver.receive_header()
        expected = _expected(args, prompt_ids, runtime.info)
        validate_kv_header(header, expected)
        if framed_payload_bytes != header["payload_bytes"]:
            raise WorkloadError(
                "framed payload length does not match the validated KV header"
            )
        payload, tcp_ms = receiver.receive_payload(connection, framed_payload_bytes)
        validate_payload(header, payload)
    finally:
        if connection is not None:
            connection.close()
    cache, h2d_ms = runtime.unpack(payload, header["tensors"])
    decode = runtime.decode(cache, header["first_token_id"], args.output_tokens)
    result = _base_result(args, prompt_ids, runtime, "decode")
    result.update(
        {
            "tcp_ms": _require_float(tcp_ms, "tcp_ms"),
            "h2d_ms": _require_float(h2d_ms, "h2d_ms"),
            "decode_gpu_ms": _require_float(decode.gpu_ms, "decode_gpu_ms"),
            "decode_itl_ms": [
                _require_float(value, "decode_itl_ms") for value in decode.itl_ms
            ],
            "first_token_id": header["first_token_id"],
            "token_ids": decode.token_ids,
            "token_digest_sha256": token_digest(decode.token_ids),
            "kv_header_sha256": sha256_bytes(canonical_json_bytes(header)),
            "kv_payload_bytes": len(payload),
            "kv_payload_sha256": header["payload_sha256"],
        }
    )
    validate_result_event(result, args.output_tokens)
    return result


RESULT_KEYS = {
    "schema",
    "schema_version",
    "event",
    "status",
    "role",
    "request_id",
    "epoch",
    "model_id",
    "model_sha256",
    "tokenizer_sha256",
    "model_class",
    "model_dtype",
    "prompt_length",
    "prompt_digest_sha256",
    "output_tokens",
    "transport",
    "kv_layout",
    "model_load_ms",
    "prefill_gpu_ms",
    "pack_ms",
    "d2h_ms",
    "tcp_ms",
    "h2d_ms",
    "decode_gpu_ms",
    "decode_itl_ms",
    "first_token_id",
    "token_ids",
    "token_digest_sha256",
    "kv_header_sha256",
    "kv_payload_bytes",
    "kv_payload_sha256",
}


def validate_result_event(event: dict[str, Any], output_tokens: int) -> None:
    _require_exact_keys(event, RESULT_KEYS, "result event")
    if event["schema"] != EVENT_SCHEMA or event["schema_version"] != 1:
        raise WorkloadError("result event schema mismatch")
    if event["event"] != "result" or event["status"] != "pass":
        raise WorkloadError("result event status mismatch")
    _require_sha256(event["model_sha256"], "result model_sha256")
    _require_sha256(event["tokenizer_sha256"], "result tokenizer_sha256")
    _require_sha256(event["prompt_digest_sha256"], "result prompt digest")
    role = event["role"]
    if role not in {"reference", "prefill", "decode"}:
        raise WorkloadError(f"invalid result role {role!r}")
    expected_token_count = 1 if role == "prefill" else output_tokens
    tokens = event["token_ids"]
    if (
        not isinstance(tokens, list)
        or len(tokens) != expected_token_count
        or any(not _is_int(token) or token < 0 for token in tokens)
    ):
        raise WorkloadError(
            f"{role} result must contain exactly {expected_token_count} token IDs"
        )
    if event["first_token_id"] != tokens[0]:
        raise WorkloadError("result first_token_id does not match token_ids[0]")
    if event["token_digest_sha256"] != token_digest(tokens):
        raise WorkloadError("result token digest does not match token IDs")
    itl = event["decode_itl_ms"]
    expected_itl = 0 if role == "prefill" else output_tokens - 1
    if not isinstance(itl, list) or len(itl) != expected_itl:
        raise WorkloadError(
            f"{role} result must contain exactly {expected_itl} decode ITLs"
        )
    for value in itl:
        _require_float(value, "decode_itl_ms")
    for key in (
        "model_load_ms",
        "prefill_gpu_ms",
        "pack_ms",
        "d2h_ms",
        "tcp_ms",
        "h2d_ms",
        "decode_gpu_ms",
    ):
        if event[key] is not None:
            _require_float(event[key], key)
    required_timings = {
        "reference": {"model_load_ms", "prefill_gpu_ms", "decode_gpu_ms"},
        "prefill": {
            "model_load_ms",
            "prefill_gpu_ms",
            "pack_ms",
            "d2h_ms",
            "tcp_ms",
        },
        "decode": {"model_load_ms", "tcp_ms", "h2d_ms", "decode_gpu_ms"},
    }
    optional_timings = {
        "model_load_ms",
        "prefill_gpu_ms",
        "pack_ms",
        "d2h_ms",
        "tcp_ms",
        "h2d_ms",
        "decode_gpu_ms",
    }
    for key in required_timings[role]:
        if event[key] is None:
            raise WorkloadError(f"{role} result requires {key}")
    for key in optional_timings - required_timings[role]:
        if event[key] is not None:
            raise WorkloadError(f"{role} result forbids {key}")
    if role in {"prefill", "decode"}:
        _require_sha256(event["kv_header_sha256"], "kv_header_sha256")
        _require_sha256(event["kv_payload_sha256"], "kv_payload_sha256")
        _require_int(event["kv_payload_bytes"], "kv_payload_bytes", 1)
        if event["transport"] != TRANSPORT or event["kv_layout"] != KV_LAYOUT:
            raise WorkloadError(f"{role} result transport/layout mismatch")
    else:
        for key in (
            "transport",
            "kv_layout",
            "kv_header_sha256",
            "kv_payload_bytes",
            "kv_payload_sha256",
        ):
            if event[key] is not None:
                raise WorkloadError(f"reference result forbids {key}")


def emit_event(event: dict[str, Any]) -> None:
    sys.stdout.write(canonical_json_bytes(event).decode("ascii") + "\n")
    sys.stdout.flush()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", required=True, choices=("reference", "prefill", "decode"))
    parser.add_argument("--model-path", type=Path, required=True)
    parser.add_argument("--model-id", required=True)
    parser.add_argument("--model-sha256", required=True)
    parser.add_argument("--tokenizer-sha256", required=True)
    parser.add_argument("--prompt-token-ids-file", type=Path, required=True)
    parser.add_argument("--request-id", required=True)
    parser.add_argument("--epoch", type=int, default=0)
    parser.add_argument("--output-tokens", type=int, default=EXPECTED_OUTPUT_TOKENS)
    parser.add_argument("--listen-ip")
    parser.add_argument("--peer-ip")
    parser.add_argument("--port", type=int)
    parser.add_argument("--timeout-sec", type=float, default=120.0)
    parser.add_argument("--export-kv", type=Path)
    parser.add_argument("--export-kv-header", type=Path)
    return parser


def validate_args(args: argparse.Namespace) -> None:
    if not isinstance(args.model_id, str) or not args.model_id.strip():
        raise WorkloadError("model_id must be non-empty")
    _require_sha256(args.model_sha256, "model_sha256")
    _require_sha256(args.tokenizer_sha256, "tokenizer_sha256")
    if REQUEST_ID_RE.fullmatch(args.request_id) is None:
        raise WorkloadError("request_id contains unsupported characters")
    _require_int(args.epoch, "epoch")
    if args.output_tokens != EXPECTED_OUTPUT_TOKENS:
        raise WorkloadError(
            f"this MVP requires exactly {EXPECTED_OUTPUT_TOKENS} output tokens"
        )
    if not math.isfinite(args.timeout_sec) or args.timeout_sec <= 0:
        raise WorkloadError("timeout_sec must be finite and positive")
    if args.role == "decode":
        if not args.listen_ip or args.peer_ip is not None:
            raise WorkloadError("decode requires --listen-ip and forbids --peer-ip")
    elif args.role == "prefill":
        if not args.peer_ip or args.listen_ip is not None:
            raise WorkloadError("prefill requires --peer-ip and forbids --listen-ip")
    elif args.listen_ip is not None or args.peer_ip is not None:
        raise WorkloadError("reference forbids network address arguments")
    if args.role in {"prefill", "decode"}:
        if not _is_int(args.port) or not 1 <= args.port <= 65535:
            raise WorkloadError("prefill/decode require a valid --port")
    elif args.port is not None:
        raise WorkloadError("reference forbids --port")
    if args.export_kv_header is not None and args.export_kv is None:
        raise WorkloadError("--export-kv-header requires --export-kv")
    if args.role != "prefill" and args.export_kv is not None:
        raise WorkloadError("only the prefill role can export the raw KV cache")


def _failure_event(args: argparse.Namespace, stage: str, exc: BaseException) -> dict[str, Any]:
    return {
        "schema": EVENT_SCHEMA,
        "schema_version": 1,
        "event": "failure",
        "status": "fail",
        "role": getattr(args, "role", "unknown"),
        "request_id": getattr(args, "request_id", None),
        "epoch": getattr(args, "epoch", None),
        "stage": stage,
        "error_type": type(exc).__name__,
        "error": str(exc),
    }


def execute(
    args: argparse.Namespace,
    runtime_factory: Callable[[Path], ModelRuntime] = HfLlamaRuntime,
    sender_factory: Callable[[str, int, float], Sender] = TcpSender,
    receiver_factory: Callable[[str, int, float], Receiver] = TcpReceiver,
) -> list[dict[str, Any]]:
    validate_args(args)
    prompt_ids = load_prompt_token_ids(
        args.prompt_token_ids_file,
        EXPECTED_PROMPT_TOKENS,
        expected_tokenizer_sha256=args.tokenizer_sha256,
    )
    with contextlib.redirect_stdout(sys.stderr):
        runtime = runtime_factory(args.model_path)
    validate_tinyllama_info(runtime.info)
    if any(token >= runtime.info.vocab_size for token in prompt_ids):
        raise WorkloadError("prompt contains a token outside the model vocabulary")
    if args.role == "reference":
        with contextlib.redirect_stdout(sys.stderr):
            return [run_reference(args, prompt_ids, runtime)]
    if args.role == "prefill":
        sender = sender_factory(args.peer_ip, args.port, args.timeout_sec)
        with contextlib.redirect_stdout(sys.stderr):
            return [run_prefill(args, prompt_ids, runtime, sender)]

    receiver: Optional[Receiver] = None
    try:
        receiver = receiver_factory(args.listen_ip, args.port, args.timeout_sec)
        ready = decode_ready_event(args, runtime, receiver)
        emit_event(ready)
        with contextlib.redirect_stdout(sys.stderr):
            result = run_decode(args, prompt_ids, runtime, receiver)
        return [result]
    finally:
        if receiver is not None:
            receiver.close()


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    stage = "validation"
    try:
        stage = "execution"
        events = execute(args)
        for event in events:
            emit_event(event)
        return 0
    except Exception as exc:
        emit_event(_failure_event(args, stage, exc))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
