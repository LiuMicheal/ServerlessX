"""Strict metadata and ACK contract for the integrated P/transfer/D artifact.

This module intentionally owns no sockets, CUDA allocations, or transport
objects.  A runner validates untrusted JSON here before it touches those
resources, then advances :class:`TransferSession` as role-specific operations
complete.  The fixed dimensions match the TinyLlama CCF-A experiment.
"""

from __future__ import annotations

import copy
import hashlib
import ipaddress
import json
import math
import re
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Mapping, Optional, Union


SCHEMA = "serverlesspd.real-pd-gdr-contract.v1"
SCHEMA_VERSION = 1
EVENT = "ack"
STATUS = "pass"
KV_LAYOUT = "legacy-pkv-l0k-l0v-fp16-le-v1"
KV_DTYPE = "float16"

EXPECTED_LAYER_COUNT = 22
EXPECTED_KV_HEAD_COUNT = 4
EXPECTED_HEAD_DIM = 64
EXPECTED_PROMPT_LENGTH = 128
EXPECTED_OUTPUT_TOKENS = 8
EXPECTED_VOCAB_SIZE = 32_000
EXPECTED_TENSOR_SHAPE = (1, EXPECTED_KV_HEAD_COUNT, EXPECTED_PROMPT_LENGTH, EXPECTED_HEAD_DIM)
EXPECTED_TENSOR_BYTES = math.prod(EXPECTED_TENSOR_SHAPE) * 2
EXPECTED_TENSOR_COUNT = EXPECTED_LAYER_COUNT * 2
EXPECTED_KV_BYTES = EXPECTED_TENSOR_BYTES * EXPECTED_TENSOR_COUNT
MAX_JSON_BYTES = 1 << 20

SHA256_RE = re.compile(r"[0-9a-f]{64}")
SAFE_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}")
GPU_UUID_RE = re.compile(r"GPU-[A-Za-z0-9][A-Za-z0-9-]{7,79}")

TENSOR_KEYS = {
    "name",
    "layer",
    "kind",
    "shape",
    "offset_bytes",
    "nbytes",
}
METADATA_KEYS = {
    "schema",
    "schema_version",
    "request_id",
    "epoch",
    "attempt_id",
    "model_id",
    "model_sha256",
    "tokenizer_sha256",
    "prompt_digest_sha256",
    "first_token_id",
    "kv_layout",
    "dtype",
    "prompt_length",
    "output_tokens",
    "tensors",
    "payload_bytes",
    "payload_sha256",
    "source_owner_id",
    "target_owner_id",
    "buffer_id",
    "source_gpu_uuid",
    "target_gpu_uuid",
    "source_endpoint",
    "target_endpoint",
}
ACK_KEYS = {
    "schema",
    "schema_version",
    "event",
    "status",
    "ack",
    "request_id",
    "epoch",
    "attempt_id",
    "buffer_id",
    "owner_id",
    "payload_sha256",
    "token_digest_sha256",
}


class ContractError(ValueError):
    """Base class for a fail-closed contract violation."""


class MetadataError(ContractError):
    """The immutable request metadata is malformed or mismatched."""


class ProtocolError(ContractError):
    """An ACK is invalid, replayed, stale, or out of order."""


class OwnershipError(ContractError):
    """A role attempted to use or release another role's allocation."""


class AckKind(str, Enum):
    TARGET_READY = "TARGET_READY"
    TRANSFER_SUBMITTED = "TRANSFER_SUBMITTED"
    TRANSFER_COMPLETED = "TRANSFER_COMPLETED"
    CACHE_VERIFIED = "CACHE_VERIFIED"
    DECODE_DONE = "DECODE_DONE"
    RELEASED = "RELEASED"
    SOURCE_RELEASED = "SOURCE_RELEASED"


ACK_SEQUENCE = tuple(AckKind)
ACK_OWNER_FIELD = {
    AckKind.TARGET_READY: "target_owner_id",
    AckKind.TRANSFER_SUBMITTED: "source_owner_id",
    AckKind.TRANSFER_COMPLETED: "source_owner_id",
    AckKind.CACHE_VERIFIED: "target_owner_id",
    AckKind.DECODE_DONE: "target_owner_id",
    AckKind.RELEASED: "target_owner_id",
    AckKind.SOURCE_RELEASED: "source_owner_id",
}


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _exact_object(
    value: Any,
    keys: set[str],
    field_name: str,
    error: type[ContractError],
) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise error(f"{field_name} must be an object")
    actual = set(value)
    if actual != keys:
        raise error(
            f"{field_name} keys differ: missing={sorted(keys - actual)}, "
            f"unknown={sorted(actual - keys)}"
        )
    return value


def _require_int(value: Any, name: str, minimum: int = 0) -> int:
    if not _is_int(value) or value < minimum:
        raise MetadataError(f"{name} must be an integer >= {minimum}")
    return value


def _require_sha256(value: Any, name: str, error: type[ContractError] = MetadataError) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise error(f"{name} must be a lowercase SHA-256")
    return value


def _require_safe_id(value: Any, name: str, error: type[ContractError] = MetadataError) -> str:
    if not isinstance(value, str) or SAFE_ID_RE.fullmatch(value) is None:
        raise error(f"{name} must be a safe non-empty ASCII identifier")
    return value


def _require_model_id(value: Any) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 256
        or not value.isascii()
        or any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in value)
    ):
        raise MetadataError("model_id must be non-empty printable ASCII")
    return value


def _require_gpu_uuid(value: Any, name: str) -> str:
    if not isinstance(value, str) or GPU_UUID_RE.fullmatch(value) is None:
        raise MetadataError(f"{name} must be a CUDA GPU UUID")
    return value


def _require_endpoint(value: Any, name: str) -> str:
    if not isinstance(value, str) or value.count(":") != 1:
        raise MetadataError(f"{name} must be an IPv4 address and port")
    host, port_text = value.rsplit(":", 1)
    try:
        address = ipaddress.ip_address(host)
        port = int(port_text, 10)
    except ValueError as exc:
        raise MetadataError(f"{name} must be an IPv4 address and port") from exc
    if address.version != 4 or str(address) != host or not 1 <= port <= 65_535:
        raise MetadataError(f"{name} must be a canonical IPv4 address and valid port")
    if port_text != str(port):
        raise MetadataError(f"{name} port must use canonical decimal notation")
    return value


def canonical_json_bytes(value: Any) -> bytes:
    """Return deterministic ASCII JSON and reject non-JSON/non-finite values."""

    try:
        return json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("ascii")
    except (TypeError, ValueError, UnicodeEncodeError) as exc:
        raise ContractError(f"cannot canonicalize JSON: {exc}") from exc


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def _no_duplicate_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise MetadataError(f"JSON contains duplicate key {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise MetadataError(f"JSON contains non-finite constant {value!r}")


def parse_metadata_json(
    payload: Union[bytes, str],
    *,
    expected: Optional[Mapping[str, Any]] = None,
) -> dict[str, Any]:
    """Parse one bounded metadata object with duplicate-key rejection."""

    if not isinstance(payload, (bytes, str)):
        raise MetadataError("metadata JSON must be bytes or text")
    size = len(payload.encode("utf-8")) if isinstance(payload, str) else len(payload)
    if not 1 <= size <= MAX_JSON_BYTES:
        raise MetadataError(f"metadata JSON size must be in [1, {MAX_JSON_BYTES}]")
    try:
        value = json.loads(
            payload,
            object_pairs_hook=_no_duplicate_object,
            parse_constant=_reject_constant,
        )
    except MetadataError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise MetadataError(f"metadata is not valid JSON: {exc}") from exc
    return validate_metadata(value, expected=expected)


def _validate_tensor(value: Any, index: int) -> dict[str, Any]:
    tensor = _exact_object(value, TENSOR_KEYS, f"tensors[{index}]", MetadataError)
    layer = index // 2
    kind = "K" if index % 2 == 0 else "V"
    wanted_name = f"L{layer}{kind}"
    if tensor["name"] != wanted_name:
        raise MetadataError(f"tensors[{index}].name must equal {wanted_name}")
    if not _is_int(tensor["layer"]) or tensor["layer"] != layer:
        raise MetadataError(f"tensors[{index}].layer must equal {layer}")
    if tensor["kind"] != kind:
        raise MetadataError(f"tensors[{index}].kind must equal {kind}")
    shape = tensor["shape"]
    if (
        not isinstance(shape, list)
        or any(not _is_int(item) for item in shape)
        or shape != list(EXPECTED_TENSOR_SHAPE)
    ):
        raise MetadataError(
            f"tensors[{index}].shape must equal {list(EXPECTED_TENSOR_SHAPE)}"
        )
    expected_offset = index * EXPECTED_TENSOR_BYTES
    if not _is_int(tensor["offset_bytes"]) or tensor["offset_bytes"] != expected_offset:
        raise MetadataError(
            f"tensors[{index}].offset_bytes must equal {expected_offset}"
        )
    if not _is_int(tensor["nbytes"]) or tensor["nbytes"] != EXPECTED_TENSOR_BYTES:
        raise MetadataError(
            f"tensors[{index}].nbytes must equal {EXPECTED_TENSOR_BYTES}"
        )
    return copy.deepcopy(dict(tensor))


def validate_metadata(
    value: Any, *, expected: Optional[Mapping[str, Any]] = None
) -> dict[str, Any]:
    """Validate and return a detached copy of the fixed experiment metadata."""

    metadata = _exact_object(value, METADATA_KEYS, "metadata", MetadataError)
    if metadata["schema"] != SCHEMA:
        raise MetadataError(f"schema must equal {SCHEMA}")
    if not _is_int(metadata["schema_version"]) or metadata["schema_version"] != SCHEMA_VERSION:
        raise MetadataError(f"schema_version must equal {SCHEMA_VERSION}")

    _require_safe_id(metadata["request_id"], "request_id")
    _require_int(metadata["epoch"], "epoch")
    _require_safe_id(metadata["attempt_id"], "attempt_id")
    _require_model_id(metadata["model_id"])
    for name in (
        "model_sha256",
        "tokenizer_sha256",
        "prompt_digest_sha256",
        "payload_sha256",
    ):
        _require_sha256(metadata[name], name)
    first_token_id = _require_int(metadata["first_token_id"], "first_token_id")
    if first_token_id >= EXPECTED_VOCAB_SIZE:
        raise MetadataError(f"first_token_id must be < {EXPECTED_VOCAB_SIZE}")
    if metadata["kv_layout"] != KV_LAYOUT:
        raise MetadataError(f"kv_layout must equal {KV_LAYOUT}")
    if metadata["dtype"] != KV_DTYPE:
        raise MetadataError(f"dtype must equal {KV_DTYPE}")
    if metadata["prompt_length"] != EXPECTED_PROMPT_LENGTH or not _is_int(
        metadata["prompt_length"]
    ):
        raise MetadataError(f"prompt_length must equal {EXPECTED_PROMPT_LENGTH}")
    if metadata["output_tokens"] != EXPECTED_OUTPUT_TOKENS or not _is_int(
        metadata["output_tokens"]
    ):
        raise MetadataError(f"output_tokens must equal {EXPECTED_OUTPUT_TOKENS}")

    tensors = metadata["tensors"]
    if not isinstance(tensors, list) or len(tensors) != EXPECTED_TENSOR_COUNT:
        raise MetadataError(f"tensors must contain exactly {EXPECTED_TENSOR_COUNT} entries")
    normalized_tensors = [_validate_tensor(tensor, index) for index, tensor in enumerate(tensors)]
    if metadata["payload_bytes"] != EXPECTED_KV_BYTES or not _is_int(metadata["payload_bytes"]):
        raise MetadataError(f"payload_bytes must equal {EXPECTED_KV_BYTES}")

    for name in ("source_owner_id", "target_owner_id", "buffer_id"):
        _require_safe_id(metadata[name], name)
    if metadata["source_owner_id"] == metadata["target_owner_id"]:
        raise MetadataError("source_owner_id and target_owner_id must differ")
    _require_gpu_uuid(metadata["source_gpu_uuid"], "source_gpu_uuid")
    _require_gpu_uuid(metadata["target_gpu_uuid"], "target_gpu_uuid")
    if metadata["source_gpu_uuid"] == metadata["target_gpu_uuid"]:
        raise MetadataError("source_gpu_uuid and target_gpu_uuid must differ")
    _require_endpoint(metadata["source_endpoint"], "source_endpoint")
    _require_endpoint(metadata["target_endpoint"], "target_endpoint")
    if metadata["source_endpoint"] == metadata["target_endpoint"]:
        raise MetadataError("source_endpoint and target_endpoint must differ")

    if expected is not None:
        if not isinstance(expected, Mapping):
            raise MetadataError("expected metadata identity must be an object")
        unknown = set(expected) - METADATA_KEYS
        if unknown:
            raise MetadataError(f"expected metadata contains unknown fields: {sorted(unknown)}")
        for name, wanted in expected.items():
            if metadata[name] != wanted or type(metadata[name]) is not type(wanted):
                raise MetadataError(
                    f"metadata {name} mismatch: expected {wanted!r}, got {metadata[name]!r}"
                )

    normalized = copy.deepcopy(dict(metadata))
    normalized["tensors"] = normalized_tensors
    return normalized


@dataclass
class EpochLedger:
    """Reject stale epochs and replayed attempts for each logical request."""

    _current: dict[str, tuple[int, str]] = field(default_factory=dict, init=False)
    _seen_attempts: set[str] = field(default_factory=set, init=False)
    _seen_buffers: set[str] = field(default_factory=set, init=False)

    def reserve(self, metadata: Mapping[str, Any]) -> None:
        value = validate_metadata(metadata)
        request_id = value["request_id"]
        proposed = (value["epoch"], value["attempt_id"])
        current = self._current.get(request_id)
        if current is not None and proposed[0] <= current[0]:
            relation = "replayed" if proposed == current else "stale or duplicate"
            raise ProtocolError(
                f"{relation} epoch for {request_id}: {proposed[0]} <= {current[0]}"
            )
        if value["attempt_id"] in self._seen_attempts:
            raise ProtocolError("attempt_id has already been used")
        if value["buffer_id"] in self._seen_buffers:
            raise ProtocolError("buffer_id has already been used")
        self._current[request_id] = proposed
        self._seen_attempts.add(value["attempt_id"])
        self._seen_buffers.add(value["buffer_id"])

    def assert_current(self, metadata: Mapping[str, Any]) -> None:
        value = validate_metadata(metadata)
        wanted = (value["epoch"], value["attempt_id"])
        if self._current.get(value["request_id"]) != wanted:
            raise ProtocolError("metadata does not identify the current epoch/attempt")

    def current(self, request_id: str) -> Optional[tuple[int, str]]:
        _require_safe_id(request_id, "request_id", ProtocolError)
        return self._current.get(request_id)


def _coerce_ack_kind(value: Any) -> AckKind:
    try:
        return AckKind(value)
    except (TypeError, ValueError) as exc:
        raise ProtocolError(f"unknown ACK kind {value!r}") from exc


def make_ack(
    kind: Union[AckKind, str],
    metadata: Mapping[str, Any],
    *,
    owner_id: Optional[str] = None,
    payload_sha256: Optional[str] = None,
    token_digest_sha256: Optional[str] = None,
) -> dict[str, Any]:
    """Build one canonical ACK; validation remains mandatory at receipt."""

    value = validate_metadata(metadata)
    ack_kind = _coerce_ack_kind(kind)
    expected_owner = value[ACK_OWNER_FIELD[ack_kind]]
    if owner_id is None:
        owner_id = expected_owner
    if payload_sha256 is None and ack_kind in {
        AckKind.TRANSFER_COMPLETED,
        AckKind.CACHE_VERIFIED,
        AckKind.DECODE_DONE,
        AckKind.RELEASED,
        AckKind.SOURCE_RELEASED,
    }:
        payload_sha256 = value["payload_sha256"]
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "event": EVENT,
        "status": STATUS,
        "ack": ack_kind.value,
        "request_id": value["request_id"],
        "epoch": value["epoch"],
        "attempt_id": value["attempt_id"],
        "buffer_id": value["buffer_id"],
        "owner_id": owner_id,
        "payload_sha256": payload_sha256,
        "token_digest_sha256": token_digest_sha256,
    }


def validate_ack(
    value: Any,
    metadata: Mapping[str, Any],
    *,
    expected_kind: Optional[Union[AckKind, str]] = None,
) -> dict[str, Any]:
    """Validate one ACK against immutable metadata and role ownership."""

    ack = _exact_object(value, ACK_KEYS, "ACK", ProtocolError)
    bound = validate_metadata(metadata)
    if (
        ack["schema"] != SCHEMA
        or not _is_int(ack["schema_version"])
        or ack["schema_version"] != SCHEMA_VERSION
    ):
        raise ProtocolError("ACK schema identity mismatch")
    if ack["event"] != EVENT or ack["status"] != STATUS:
        raise ProtocolError("ACK must be a passing ack event")
    kind = _coerce_ack_kind(ack["ack"])
    if expected_kind is not None and kind != _coerce_ack_kind(expected_kind):
        raise ProtocolError(
            f"expected ACK {_coerce_ack_kind(expected_kind).value}, got {kind.value}"
        )
    for name in ("request_id", "epoch", "attempt_id", "buffer_id"):
        if ack[name] != bound[name] or type(ack[name]) is not type(bound[name]):
            raise ProtocolError(f"ACK {name} does not match immutable metadata")
    expected_owner = bound[ACK_OWNER_FIELD[kind]]
    if ack["owner_id"] != expected_owner:
        raise OwnershipError(
            f"ACK {kind.value} owner must equal {ACK_OWNER_FIELD[kind]}"
        )

    hash_kinds = {
        AckKind.TRANSFER_COMPLETED,
        AckKind.CACHE_VERIFIED,
        AckKind.DECODE_DONE,
        AckKind.RELEASED,
        AckKind.SOURCE_RELEASED,
    }
    if kind in hash_kinds:
        _require_sha256(ack["payload_sha256"], "ACK payload_sha256", ProtocolError)
        if ack["payload_sha256"] != bound["payload_sha256"]:
            raise ProtocolError("ACK payload SHA-256 differs from metadata")
    elif ack["payload_sha256"] is not None:
        raise ProtocolError(f"ACK {kind.value} must not claim payload verification")

    token_kinds = {AckKind.DECODE_DONE, AckKind.RELEASED, AckKind.SOURCE_RELEASED}
    if kind in token_kinds:
        _require_sha256(
            ack["token_digest_sha256"], "ACK token_digest_sha256", ProtocolError
        )
    elif ack["token_digest_sha256"] is not None:
        raise ProtocolError(f"ACK {kind.value} must not claim decode completion")
    return copy.deepcopy(dict(ack))


@dataclass
class TransferSession:
    """In-memory fail-closed state machine for one immutable transfer attempt."""

    metadata: Mapping[str, Any]
    ledger: Optional[EpochLedger] = None
    _value: dict[str, Any] = field(init=False, repr=False)
    _index: int = field(default=-1, init=False, repr=False)
    _aborted_reason: Optional[str] = field(default=None, init=False, repr=False)
    # Metadata is created only after P has packed its live source slab.
    _source_owned: bool = field(default=True, init=False, repr=False)
    _target_owned: bool = field(default=False, init=False, repr=False)
    _token_digest: Optional[str] = field(default=None, init=False, repr=False)

    def __post_init__(self) -> None:
        self._value = validate_metadata(self.metadata)
        if self.ledger is not None:
            self.ledger.reserve(self._value)

    @property
    def state(self) -> str:
        if self._aborted_reason is not None:
            return "ABORTED"
        if self._index < 0:
            return "INIT"
        return ACK_SEQUENCE[self._index].value

    @property
    def aborted_reason(self) -> Optional[str]:
        return self._aborted_reason

    @property
    def owners(self) -> dict[str, bool]:
        return {"source": self._source_owned, "target": self._target_owned}

    @property
    def closed(self) -> bool:
        if self._aborted_reason is not None:
            return not self._source_owned and not self._target_owned
        return self._index == len(ACK_SEQUENCE) - 1 and not any(self.owners.values())

    def next_kind(self) -> Optional[AckKind]:
        if self._aborted_reason is not None or self._index + 1 >= len(ACK_SEQUENCE):
            return None
        return ACK_SEQUENCE[self._index + 1]

    def advance(self, ack: Mapping[str, Any]) -> dict[str, Any]:
        if self._aborted_reason is not None:
            raise ProtocolError("aborted session cannot accept ACKs")
        expected = self.next_kind()
        if expected is None:
            raise ProtocolError("completed session cannot accept ACKs")
        checked = validate_ack(ack, self._value, expected_kind=expected)

        # Mutate ownership only after every ACK assertion has passed.
        if expected is AckKind.TARGET_READY:
            self._target_owned = True
        elif expected is AckKind.DECODE_DONE:
            self._token_digest = checked["token_digest_sha256"]
        elif expected is AckKind.RELEASED:
            if checked["token_digest_sha256"] != self._token_digest:
                raise ProtocolError("target release token digest differs from DECODE_DONE")
            self._target_owned = False
        elif expected is AckKind.SOURCE_RELEASED:
            if checked["token_digest_sha256"] != self._token_digest:
                raise ProtocolError("source release token digest differs from DECODE_DONE")
            self._source_owned = False
        self._index += 1
        return checked

    def advance_kind(
        self,
        kind: Union[AckKind, str],
        *,
        owner_id: Optional[str] = None,
        payload_sha256: Optional[str] = None,
        token_digest_sha256: Optional[str] = None,
    ) -> dict[str, Any]:
        ack_kind = _coerce_ack_kind(kind)
        if token_digest_sha256 is None and ack_kind in {
            AckKind.RELEASED,
            AckKind.SOURCE_RELEASED,
        }:
            token_digest_sha256 = self._token_digest
        ack = make_ack(
            ack_kind,
            self._value,
            owner_id=owner_id,
            payload_sha256=payload_sha256,
            token_digest_sha256=token_digest_sha256,
        )
        return self.advance(ack)

    def abort(self, reason: str) -> None:
        if self.closed:
            raise ProtocolError("closed session cannot be aborted")
        if self._aborted_reason is not None:
            raise ProtocolError("session is already aborted")
        if not isinstance(reason, str) or not reason.strip() or len(reason) > 512:
            raise ProtocolError("abort reason must be non-empty and bounded")
        self._aborted_reason = reason

    def cleanup_owner(self, owner_id: str) -> None:
        """Record exact post-abort resource cleanup without broad process matching."""

        if self._aborted_reason is None:
            raise ProtocolError("owner cleanup shortcut is allowed only after abort")
        _require_safe_id(owner_id, "owner_id", OwnershipError)
        if owner_id == self._value["source_owner_id"] and self._source_owned:
            self._source_owned = False
            return
        if owner_id == self._value["target_owner_id"] and self._target_owned:
            self._target_owned = False
            return
        raise OwnershipError("owner does not hold a live resource in this session")


def build_fixed_tensors() -> list[dict[str, Any]]:
    """Return the canonical TinyLlama tensor table for builders and tests."""

    result: list[dict[str, Any]] = []
    for index in range(EXPECTED_TENSOR_COUNT):
        layer = index // 2
        kind = "K" if index % 2 == 0 else "V"
        result.append(
            {
                "name": f"L{layer}{kind}",
                "layer": layer,
                "kind": kind,
                "shape": list(EXPECTED_TENSOR_SHAPE),
                "offset_bytes": index * EXPECTED_TENSOR_BYTES,
                "nbytes": EXPECTED_TENSOR_BYTES,
            }
        )
    return result
