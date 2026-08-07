from __future__ import annotations

import argparse
import contextlib
import copy
import io
import json
import os
import socket
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Optional
from unittest import mock

from serverlessx.spd import tinyllama_pd as workload

PROMPT_PATH = Path(workload.__file__).with_name("data") / "prompt-128.json"


MODEL_SHA = "a" * 64
TOKENIZER_SHA = "2d8e988e5eade2beb623d561b2bbb1fd412eae20b856f281cdce5814617f5862"


class FakeTensor:
    dtype = "torch.float16"

    def __init__(self, shape: tuple[int, ...], raw: bytes):
        self.shape = shape
        self.raw = raw


class FakeRuntime:
    info = workload.ModelInfo(
        model_type="llama",
        model_class="LlamaForCausalLM",
        dtype="float16",
        layer_count=22,
        kv_head_count=4,
        head_dim=64,
        vocab_size=32000,
    )
    model_load_ms = 12.5

    def __init__(self, packed: workload.PackedKv):
        self.packed = packed
        self.unpacked_payload: Optional[bytes] = None

    def prefill(self, prompt_token_ids: list[int]) -> workload.PrefillOutput:
        return workload.PrefillOutput(101, "gpu-cache", 3.5)

    def pack(self, cache: object, prompt_length: int) -> workload.PackedKv:
        if cache != "gpu-cache" or prompt_length != 128:
            raise AssertionError("unexpected mock prefill cache")
        return self.packed

    def unpack(
        self, payload: bytes, tensors: list[dict]
    ) -> tuple[object, float]:
        self.unpacked_payload = payload
        if tensors != self.packed.tensors:
            raise AssertionError("unexpected tensor layout")
        return "received-cache", 1.25

    def decode(
        self, cache: object, first_token_id: int, output_tokens: int
    ) -> workload.DecodeOutput:
        if cache not in {"gpu-cache", "received-cache"}:
            raise AssertionError("unexpected mock decode cache")
        if first_token_id != 101 or output_tokens != 8:
            raise AssertionError("unexpected mock decode parameters")
        tokens = [101, 102, 103, 104, 105, 106, 107, 108]
        return workload.DecodeOutput(tokens, 7.0, [1.0] * 7)


class FakeSender:
    def __init__(self) -> None:
        self.header: Optional[dict] = None
        self.payload: Optional[bytes] = None

    def send(self, header: dict, payload: bytes) -> float:
        self.header = copy.deepcopy(header)
        self.payload = payload
        return 2.0


class DummyConnection:
    def __init__(self) -> None:
        self.closed = False

    def close(self) -> None:
        self.closed = True


class FakeReceiver:
    listen_ip = "127.0.0.1"
    port = 12690

    def __init__(self, header: dict, payload: bytes):
        self.header = header
        self.payload = payload
        self.connection = DummyConnection()
        self.closed = False

    def receive_header(self) -> tuple[DummyConnection, dict, int]:
        return self.connection, copy.deepcopy(self.header), len(self.payload)

    def receive_payload(
        self, connection: DummyConnection, expected_bytes: int
    ) -> tuple[bytes, float]:
        if connection is not self.connection or expected_bytes != len(self.payload):
            raise AssertionError("unexpected mock receive")
        return self.payload, 2.25

    def close(self) -> None:
        self.closed = True


class FakeCuda:
    def __init__(self) -> None:
        self.synchronize_count = 0

    def synchronize(self) -> None:
        self.synchronize_count += 1

    def Event(self, *, enable_timing: bool) -> None:
        raise AssertionError("host_sync must not create CUDA timing events")


def args(role: str, temporary: Optional[Path] = None) -> argparse.Namespace:
    return argparse.Namespace(
        role=role,
        model_path=Path("/models/TinyLlama"),
        model_id="TinyLlama/TinyLlama-1.1B-Chat-v1.0",
        model_sha256=MODEL_SHA,
        tokenizer_sha256=TOKENIZER_SHA,
        prompt_token_ids_file=PROMPT_PATH,
        request_id="real-pd-test",
        epoch=7,
        output_tokens=8,
        listen_ip="127.0.0.1" if role == "decode" else None,
        peer_ip="127.0.0.1" if role == "prefill" else None,
        port=12690 if role in {"prefill", "decode"} else None,
        timeout_sec=5.0,
        export_kv=(temporary / "kv.bin") if temporary is not None else None,
        export_kv_header=(temporary / "kv.header.json")
        if temporary is not None
        else None,
    )


def expected(prompt_length: int, info: workload.ModelInfo) -> workload.ExpectedKv:
    prompt_ids = list(range(prompt_length))
    return workload.ExpectedKv(
        model_id="TinyLlama/TinyLlama-1.1B-Chat-v1.0",
        model_sha256=MODEL_SHA,
        tokenizer_sha256=TOKENIZER_SHA,
        request_id="real-pd-test",
        epoch=7,
        prompt_length=prompt_length,
        prompt_digest_sha256=workload.token_digest(prompt_ids),
        output_tokens=8,
        model_info=info,
    )


def packed_fixture(prompt_length: int = 128) -> workload.PackedKv:
    info = FakeRuntime.info
    shape = [1, info.kv_head_count, prompt_length, info.head_dim]
    tensor_bytes = 2
    for dimension in shape:
        tensor_bytes *= dimension
    tensors = []
    chunks = []
    offset = 0
    for layer in range(info.layer_count):
        for kind in ("K", "V"):
            index = len(chunks)
            chunk = bytes([index % 251]) * tensor_bytes
            tensors.append(
                {
                    "name": f"L{layer}{kind}",
                    "layer": layer,
                    "kind": kind,
                    "shape": shape,
                    "offset_bytes": offset,
                    "nbytes": tensor_bytes,
                }
            )
            chunks.append(chunk)
            offset += tensor_bytes
    return workload.PackedKv(b"".join(chunks), tensors, 1.0, 1.5)


class RealPdMvpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.prompt_ids = workload.load_prompt_token_ids(
            PROMPT_PATH,
            128,
            TOKENIZER_SHA,
        )
        cls.packed = packed_fixture()
        cls.expected = workload.ExpectedKv(
            model_id="TinyLlama/TinyLlama-1.1B-Chat-v1.0",
            model_sha256=MODEL_SHA,
            tokenizer_sha256=TOKENIZER_SHA,
            request_id="real-pd-test",
            epoch=7,
            prompt_length=128,
            prompt_digest_sha256=workload.token_digest(cls.prompt_ids),
            output_tokens=8,
            model_info=FakeRuntime.info,
        )
        cls.header = workload.build_kv_header(cls.expected, cls.packed, 101)

    def test_committed_prompt_is_pinned_to_128_tokens_and_tokenizer(self) -> None:
        document = json.loads(
            PROMPT_PATH.read_text()
        )
        self.assertEqual(workload.PROMPT_SCHEMA, document["schema"])
        self.assertEqual(128, len(document["token_ids"]))
        self.assertEqual(TOKENIZER_SHA, document["tokenizer_sha256"])
        self.assertEqual(document["token_ids"], self.prompt_ids)

    def test_prompt_rejects_tokenizer_identity_mismatch(self) -> None:
        with self.assertRaisesRegex(workload.WorkloadError, "different tokenizer"):
            workload.load_prompt_token_ids(
                PROMPT_PATH,
                128,
                "f" * 64,
            )

    def test_prompt_rejects_unbound_bare_token_array(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "prompt.json"
            path.write_text(json.dumps(list(range(128))))
            with self.assertRaisesRegex(workload.WorkloadError, "JSON object"):
                workload.load_prompt_token_ids(path, 128, TOKENIZER_SHA)

    def test_canonical_pack_orders_l0k_l0v_then_l1(self) -> None:
        info = workload.ModelInfo("llama", "LlamaForCausalLM", "float16", 2, 1, 2, 8)
        shape = (1, 1, 1, 2)
        raw_chunks = [b"\x01\x00" * 2, b"\x02\x00" * 2, b"\x03\x00" * 2, b"\x04\x00" * 2]
        tensors = [FakeTensor(shape, raw) for raw in raw_chunks]
        cache = ((tensors[0], tensors[1]), (tensors[2], tensors[3]))
        ticks = iter((0, 1_000_000, 2_000_000, 4_000_000))
        packed = workload.pack_legacy_pkv(
            cache,
            1,
            info,
            to_host=lambda tensor: tensor,
            to_le_fp16_bytes=lambda tensor: tensor.raw,
            clock_ns=lambda: next(ticks),
        )
        self.assertEqual(b"".join(raw_chunks), packed.payload)
        self.assertEqual(["L0K", "L0V", "L1K", "L1V"], [x["name"] for x in packed.tensors])
        self.assertEqual([0, 4, 8, 12], [x["offset_bytes"] for x in packed.tensors])
        self.assertEqual(1.0, packed.d2h_ms)
        self.assertEqual(2.0, packed.pack_ms)

    def test_header_rejects_epoch_and_offset_changes(self) -> None:
        changed = copy.deepcopy(self.header)
        changed["epoch"] = 8
        with self.assertRaisesRegex(workload.WorkloadError, "epoch mismatch"):
            workload.validate_kv_header(changed, self.expected)
        changed = copy.deepcopy(self.header)
        changed["tensors"][2]["offset_bytes"] += 2
        with self.assertRaisesRegex(workload.WorkloadError, "offset_bytes mismatch"):
            workload.validate_kv_header(changed, self.expected)

    def test_payload_hash_mismatch_fails_closed(self) -> None:
        changed = bytearray(self.packed.payload)
        changed[-1] ^= 1
        with self.assertRaisesRegex(workload.WorkloadError, "SHA-256 mismatch"):
            workload.validate_payload(self.header, bytes(changed))

    def test_length_prefixed_stream_round_trip(self) -> None:
        left, right = socket.socketpair()
        try:
            payload = b"real-kv"
            header = {"schema": "fixture", "payload_bytes": len(payload)}
            workload.send_framed(left, header, payload)
            received_header, payload_length = workload.receive_framed_header(right)
            received_payload = workload.recv_exact(right, payload_length)
        finally:
            left.close()
            right.close()
        self.assertEqual(header, received_header)
        self.assertEqual(payload, received_payload)

    def test_truncated_stream_is_rejected(self) -> None:
        left, right = socket.socketpair()
        try:
            left.sendall(workload.LENGTH_PREFIX.pack(10) + b"short")
            left.close()
            with self.assertRaisesRegex(workload.WorkloadError, "ended after"):
                workload.receive_framed_header(right)
        finally:
            right.close()

    def test_mock_reference_uses_prefill_t0_for_same_semantic_decode(self) -> None:
        runtime = FakeRuntime(self.packed)
        result = workload.run_reference(args("reference"), self.prompt_ids, runtime)
        self.assertEqual("reference", result["role"])
        self.assertEqual([101, 102, 103, 104, 105, 106, 107, 108], result["token_ids"])
        self.assertEqual(7, len(result["decode_itl_ms"]))
        self.assertIsNone(result["tcp_ms"])

    def test_host_sync_timing_avoids_cuda_events(self) -> None:
        runtime = object.__new__(workload.HfLlamaRuntime)
        cuda = FakeCuda()
        runtime.torch = SimpleNamespace(cuda=cuda)
        with mock.patch.dict(
            os.environ, {"SERVERLESSPD_CUDA_TIMING_MODE": "host_sync"}
        ):
            result, elapsed_ms = runtime._gpu_call(lambda: "done")
        self.assertEqual("done", result)
        self.assertEqual(2, cuda.synchronize_count)
        self.assertGreaterEqual(elapsed_ms, 0.0)

    def test_mock_prefill_exports_exact_bytes_and_sends_header(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_args = args("prefill", Path(directory))
            sender = FakeSender()
            result = workload.run_prefill(
                run_args, self.prompt_ids, FakeRuntime(self.packed), sender
            )
            self.assertEqual(self.packed.payload, sender.payload)
            self.assertEqual(self.header["payload_sha256"], result["kv_payload_sha256"])
            self.assertEqual(self.packed.payload, run_args.export_kv.read_bytes())
            exported_header = json.loads(run_args.export_kv_header.read_text())
            self.assertEqual(sender.header, exported_header)
            self.assertEqual([101], result["token_ids"])

    def test_mock_decode_validates_then_reconstructs_and_generates(self) -> None:
        runtime = FakeRuntime(self.packed)
        receiver = FakeReceiver(self.header, self.packed.payload)
        result = workload.run_decode(
            args("decode"), self.prompt_ids, runtime, receiver
        )
        self.assertTrue(receiver.connection.closed)
        self.assertEqual(self.packed.payload, runtime.unpacked_payload)
        self.assertEqual([101, 102, 103, 104, 105, 106, 107, 108], result["token_ids"])
        self.assertEqual(1.25, result["h2d_ms"])
        self.assertEqual(2.25, result["tcp_ms"])

    def test_execute_decode_emits_ready_before_returning_result(self) -> None:
        runtime = FakeRuntime(self.packed)
        receiver = FakeReceiver(self.header, self.packed.payload)
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            events = workload.execute(
                args("decode"),
                runtime_factory=lambda _path: runtime,
                receiver_factory=lambda _ip, _port, _timeout: receiver,
            )
        ready_events = [json.loads(line) for line in stdout.getvalue().splitlines()]
        self.assertEqual(["model_ready"], [event["event"] for event in ready_events])
        self.assertEqual("decode", ready_events[0]["role"])
        self.assertEqual(12690, ready_events[0]["port"])
        self.assertEqual("result", events[0]["event"])
        self.assertTrue(receiver.closed)

    def test_decode_rejects_bad_hash_before_runtime_unpack(self) -> None:
        runtime = FakeRuntime(self.packed)
        changed = bytearray(self.packed.payload)
        changed[0] ^= 1
        receiver = FakeReceiver(self.header, bytes(changed))
        with self.assertRaisesRegex(workload.WorkloadError, "SHA-256 mismatch"):
            workload.run_decode(args("decode"), self.prompt_ids, runtime, receiver)
        self.assertTrue(receiver.connection.closed)
        self.assertIsNone(runtime.unpacked_payload)

    def test_directional_arguments_are_fail_closed(self) -> None:
        invalid = args("decode")
        invalid.listen_ip = None
        with self.assertRaisesRegex(workload.WorkloadError, "requires --listen-ip"):
            workload.validate_args(invalid)
        invalid = args("prefill")
        invalid.peer_ip = None
        with self.assertRaisesRegex(workload.WorkloadError, "requires --peer-ip"):
            workload.validate_args(invalid)


if __name__ == "__main__":
    unittest.main()
