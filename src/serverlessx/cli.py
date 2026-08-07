"""Read-only host probing and a portable ServerlessX bootstrap workflow."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from .spd import contract


PROFILE_SCHEMA = "serverlessx.profile.v1"
PLAN_SCHEMA = "serverlessx.plan.v1"
DOCTOR_SCHEMA = "serverlessx.doctor.v1"
RESULT_SCHEMA = "serverlessx.run-result.v1"
VERIFICATION_SCHEMA = "serverlessx.verification.v1"
ERROR_SCHEMA = "serverlessx.error.v1"
RESULT_SCHEMA_VERSION = 1

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_UNAVAILABLE = 3
EXIT_RUN_FAILED = 4
EXIT_VERIFY_FAILED = 5
EXIT_CONFIGURATION = 6

PROFILE_KEYS = {
    "schema",
    "id",
    "title",
    "system",
    "tier",
    "maturity",
    "priority",
    "available",
    "unavailable_reason",
    "description",
    "requirements",
    "capabilities",
    "workflow",
    "claims",
    "privileged_operations",
}
REQUIREMENT_KEYS = {"python_min", "commands", "paths"}
RESULT_KEYS = {
    "schema",
    "schema_version",
    "run_id",
    "status",
    "system",
    "profile",
    "mode",
    "started_at",
    "finished_at",
    "metadata",
    "events",
    "outcome",
    "checks",
    "claims",
}
OUTCOME_KEYS = {"state", "closed", "owners"}
CHECK_KEYS = {"completed_ack_sequence", "replay_rejected"}
CLAIM_KEYS = {
    "contract_simulation",
    "kv_payload_transferred",
    "model_inference_performed",
    "gpu_execution_performed",
}
SAFE_NAME = re.compile(r"[a-z][a-z0-9_]{0,63}")
SAFE_RUN_ID = re.compile(r"[0-9]{8}T[0-9]{6}\.[0-9]{6}Z-[0-9a-f]{12}")
COMMAND_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}")
PROFILE_TIERS = {"portable", "lab"}
PROFILE_MATURITIES = {"executable", "source-preview", "planned", "reference-only"}

_configured_root = os.environ.get("SERVERLESSX_ROOT")
if _configured_root:
    REPO_ROOT = Path(_configured_root).resolve()
else:
    REPO_ROOT = Path(__file__).resolve().parents[2]


class CliError(Exception):
    """Expected command failure with a stable process exit code."""

    def __init__(self, message: str, exit_code: int) -> None:
        super().__init__(message)
        self.exit_code = exit_code


class ProfileError(CliError):
    def __init__(self, message: str) -> None:
        super().__init__(message, EXIT_CONFIGURATION)


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _require_string_list(value: Any, name: str) -> List[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ProfileError("{} must be a list of strings".format(name))
    return list(value)


def _load_json(path: Path, maximum_bytes: int = 4 << 20) -> Any:
    try:
        stat = path.lstat()
    except OSError as exc:
        raise CliError("cannot inspect {}: {}".format(path, exc), EXIT_VERIFY_FAILED)
    if path.is_symlink() or not path.is_file():
        raise CliError("refusing non-regular JSON file {}".format(path), EXIT_VERIFY_FAILED)
    if not 1 <= stat.st_size <= maximum_bytes:
        raise CliError(
            "JSON file {} must be between 1 and {} bytes".format(path, maximum_bytes),
            EXIT_VERIFY_FAILED,
        )

    def no_duplicates(pairs: List[Tuple[str, Any]]) -> Dict[str, Any]:
        result: Dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key {!r}".format(key))
            result[key] = value
        return result

    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(
                handle,
                object_pairs_hook=no_duplicates,
                parse_constant=lambda value: (_ for _ in ()).throw(
                    ValueError("non-finite JSON constant {!r}".format(value))
                ),
            )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise CliError("cannot parse {}: {}".format(path, exc), EXIT_VERIFY_FAILED)


def _validate_profile(value: Any, path: Path) -> Dict[str, Any]:
    if not isinstance(value, Mapping):
        raise ProfileError("{} must contain a JSON object".format(path))
    keys = set(value)
    if keys != PROFILE_KEYS:
        raise ProfileError(
            "{} profile keys differ: missing={}, unknown={}".format(
                path, sorted(PROFILE_KEYS - keys), sorted(keys - PROFILE_KEYS)
            )
        )
    if value["schema"] != PROFILE_SCHEMA:
        raise ProfileError("{} has unsupported profile schema".format(path))
    for name in ("id", "title", "system", "tier", "maturity", "description"):
        if not isinstance(value[name], str) or not value[name]:
            raise ProfileError("{}.{} must be a non-empty string".format(path, name))
    if SAFE_NAME.fullmatch(value["id"]) is None:
        raise ProfileError("{}.id is not a safe profile identifier".format(path))
    if SAFE_NAME.fullmatch(value["system"]) is None:
        raise ProfileError("{}.system is not a safe system identifier".format(path))
    if value["tier"] not in PROFILE_TIERS:
        raise ProfileError("{}.tier is not supported".format(path))
    if value["maturity"] not in PROFILE_MATURITIES:
        raise ProfileError("{}.maturity is not supported".format(path))
    if not _is_int(value["priority"]):
        raise ProfileError("{}.priority must be an integer".format(path))
    if not isinstance(value["available"], bool):
        raise ProfileError("{}.available must be boolean".format(path))
    reason = value["unavailable_reason"]
    if reason is not None and (not isinstance(reason, str) or not reason):
        raise ProfileError("{}.unavailable_reason must be null or non-empty text".format(path))
    if value["available"] and reason is not None:
        raise ProfileError("{} is available but has an unavailable reason".format(path))
    if not value["available"] and reason is None:
        raise ProfileError("{} is unavailable but has no reason".format(path))
    for name in ("capabilities", "workflow", "claims", "privileged_operations"):
        _require_string_list(value[name], "{}.{}".format(path, name))

    requirements = value["requirements"]
    if not isinstance(requirements, Mapping) or set(requirements) != REQUIREMENT_KEYS:
        raise ProfileError("{}.requirements must have exactly {}".format(path, sorted(REQUIREMENT_KEYS)))
    python_min = requirements["python_min"]
    if not isinstance(python_min, str) or re.fullmatch(r"[0-9]+\.[0-9]+", python_min) is None:
        raise ProfileError("{}.requirements.python_min must be MAJOR.MINOR".format(path))
    commands = _require_string_list(requirements["commands"], "requirements.commands")
    if any(COMMAND_NAME.fullmatch(command) is None for command in commands):
        raise ProfileError("{}.requirements.commands contains an unsafe name".format(path))
    paths = _require_string_list(requirements["paths"], "requirements.paths")
    if any(not Path(item).is_absolute() for item in paths):
        raise ProfileError("{}.requirements.paths must be absolute".format(path))
    return dict(value)


def load_profiles() -> Dict[str, Dict[str, Any]]:
    profiles_root = REPO_ROOT / "profiles"
    if not profiles_root.is_dir():
        raise ProfileError("profiles directory is missing: {}".format(profiles_root))
    profiles: Dict[str, Dict[str, Any]] = {}
    for path in sorted(profiles_root.glob("*/*/profile.json")):
        try:
            value = _load_json(path)
        except CliError as exc:
            raise ProfileError(str(exc))
        profile = _validate_profile(value, path)
        profile_id = profile["id"]
        if profile_id in profiles:
            raise ProfileError("duplicate profile id {!r}".format(profile_id))
        profiles[profile_id] = profile
    if not profiles:
        raise ProfileError("no profiles found under {}".format(profiles_root))
    return profiles


def _minimum_python(text: str) -> Tuple[int, int]:
    major, minor = text.split(".", 1)
    return int(major), int(minor)


def assess_profile(profile: Mapping[str, Any]) -> Dict[str, Any]:
    requirements = profile["requirements"]
    unmet: List[str] = []
    minimum = _minimum_python(requirements["python_min"])
    if sys.version_info[:2] < minimum:
        unmet.append("python>={}".format(requirements["python_min"]))
    for command in requirements["commands"]:
        if shutil.which(command) is None:
            unmet.append("command:{}".format(command))
    for required_path in requirements["paths"]:
        if not Path(required_path).exists():
            unmet.append("path:{}".format(required_path))
    compatible = not unmet
    return {
        "id": profile["id"],
        "available": profile["available"],
        "compatible": compatible,
        "runnable": profile["available"] and compatible,
        "unmet_requirements": unmet,
    }


def _nvidia_probe() -> Dict[str, Any]:
    executable = shutil.which("nvidia-smi")
    result: Dict[str, Any] = {
        "command_available": executable is not None,
        "gpus": [],
        "probe_error": None,
    }
    if executable is None:
        return result
    try:
        completed = subprocess.run(
            [
                executable,
                "--query-gpu=uuid,name,memory.total",
                "--format=csv,noheader,nounits",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        result["probe_error"] = str(exc)
        return result
    if completed.returncode != 0:
        message = completed.stderr.strip().splitlines()
        result["probe_error"] = message[0][:240] if message else "nvidia-smi failed"
        return result
    gpus: List[Dict[str, Any]] = []
    for line in completed.stdout.splitlines():
        parts = [part.strip() for part in line.split(",", 2)]
        if len(parts) != 3:
            continue
        try:
            memory_mib: Optional[int] = int(parts[2])
        except ValueError:
            memory_mib = None
        gpus.append({"uuid": parts[0], "name": parts[1], "memory_mib": memory_mib})
    result["gpus"] = gpus
    return result


def doctor_payload() -> Dict[str, Any]:
    infiniband_root = Path("/sys/class/infiniband")
    rdma_devices: List[str] = []
    if infiniband_root.is_dir():
        try:
            rdma_devices = sorted(
                child.name for child in infiniband_root.iterdir() if not child.name.startswith(".")
            )
        except OSError:
            rdma_devices = []
    profiles = load_profiles()
    assessments = [
        assess_profile(profile)
        for profile in sorted(profiles.values(), key=lambda item: item["id"])
    ]
    return {
        "schema": DOCTOR_SCHEMA,
        "status": "pass",
        "read_only": True,
        "host": {
            "hostname": socket.gethostname(),
            "platform": platform.system(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "nvidia": _nvidia_probe(),
        "rdma": {
            "sysfs_available": infiniband_root.is_dir(),
            "devices": rdma_devices,
        },
        "containers": {
            "docker": shutil.which("docker") is not None,
            "podman": shutil.which("podman") is not None,
        },
        "profiles": assessments,
    }


def profiles_payload() -> Dict[str, Any]:
    profiles = load_profiles()
    entries = []
    for profile in sorted(profiles.values(), key=lambda item: (-item["priority"], item["id"])):
        assessment = assess_profile(profile)
        entries.append(
            {
                "id": profile["id"],
                "title": profile["title"],
                "system": profile["system"],
                "tier": profile["tier"],
                "maturity": profile["maturity"],
                "priority": profile["priority"],
                "available": profile["available"],
                "compatible": assessment["compatible"],
                "runnable": assessment["runnable"],
                "unavailable_reason": profile["unavailable_reason"],
                "unmet_requirements": assessment["unmet_requirements"],
            }
        )
    return {"schema": "serverlessx.profiles.v1", "profiles": entries}


def build_plan(requested_profile: str, system: str = "spd") -> Dict[str, Any]:
    profiles = load_profiles()
    candidates = [profile for profile in profiles.values() if profile["system"] == system]
    selected: Optional[Dict[str, Any]] = None
    reason: Optional[str] = None
    if requested_profile == "auto":
        runnable = [profile for profile in candidates if assess_profile(profile)["runnable"]]
        if runnable:
            selected = sorted(runnable, key=lambda item: (-item["priority"], item["id"]))[0]
        else:
            reason = "no declared-available profile is compatible with this host"
    else:
        selected = profiles.get(requested_profile)
        if selected is None or selected["system"] != system:
            reason = "unknown {} profile {!r}".format(system, requested_profile)
            selected = None

    assessment: Optional[Dict[str, Any]] = None
    if selected is not None:
        assessment = assess_profile(selected)
        if not selected["available"]:
            reason = selected["unavailable_reason"]
        elif not assessment["compatible"]:
            reason = "unmet requirements: {}".format(
                ", ".join(assessment["unmet_requirements"])
            )

    ready = selected is not None and assessment is not None and assessment["runnable"]
    return {
        "schema": PLAN_SCHEMA,
        "status": "ready" if ready else "blocked",
        "system": system,
        "requested_profile": requested_profile,
        "selected_profile": selected["id"] if selected is not None else None,
        "reason": None if ready else reason,
        "requirements": dict(selected["requirements"]) if selected is not None else None,
        "capabilities": list(selected["capabilities"]) if selected is not None else [],
        "steps": list(selected["workflow"]) if selected is not None else [],
        "claims": list(selected["claims"]) if selected is not None else [],
        "privileged_operations": (
            list(selected["privileged_operations"]) if selected is not None else []
        ),
    }


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def _new_run_id() -> str:
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    return "{}-{}".format(timestamp, uuid.uuid4().hex[:12])


def _runs_root() -> Path:
    configured = os.environ.get("SERVERLESSX_RUNS_DIR")
    return Path(configured).resolve() if configured else (REPO_ROOT / "runs").resolve()


def _digest(label: str) -> str:
    return hashlib.sha256(label.encode("ascii")).hexdigest()


def _simulation_metadata(run_id: str) -> Dict[str, Any]:
    attempt_id = "attempt-{}".format(run_id)
    return {
        "schema": contract.SCHEMA,
        "schema_version": contract.SCHEMA_VERSION,
        "request_id": "serverlessx-cpu-demo",
        "epoch": 1,
        "attempt_id": attempt_id,
        "model_id": "TinyLlama/TinyLlama-1.1B-Chat-v1.0",
        "model_sha256": _digest("serverlessx-cpu-demo-model"),
        "tokenizer_sha256": _digest("serverlessx-cpu-demo-tokenizer"),
        "prompt_digest_sha256": _digest("serverlessx-cpu-demo-prompt-128"),
        "first_token_id": 2008,
        "kv_layout": contract.KV_LAYOUT,
        "dtype": contract.KV_DTYPE,
        "prompt_length": contract.EXPECTED_PROMPT_LENGTH,
        "output_tokens": contract.EXPECTED_OUTPUT_TOKENS,
        "tensors": contract.build_fixed_tensors(),
        "payload_bytes": contract.EXPECTED_KV_BYTES,
        "payload_sha256": _digest("serverlessx-cpu-demo-synthetic-kv-payload"),
        "source_owner_id": "source.simulated.1",
        "target_owner_id": "target.simulated.1",
        "buffer_id": "buffer-{}".format(run_id),
        "source_gpu_uuid": "GPU-11111111-1111-1111-1111-111111111111",
        "target_gpu_uuid": "GPU-22222222-2222-2222-2222-222222222222",
        "source_endpoint": "192.0.2.10:12855",
        "target_endpoint": "192.0.2.11:12856",
    }


def _relative_display(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path.resolve())


def run_cpu_simulation() -> Tuple[Dict[str, Any], Path]:
    started_at = _utc_now()
    run_id = _new_run_id()
    metadata = _simulation_metadata(run_id)
    ledger = contract.EpochLedger()
    session = contract.TransferSession(metadata, ledger=ledger)
    events: List[Dict[str, Any]] = []
    for kind in (
        contract.AckKind.TARGET_READY,
        contract.AckKind.TRANSFER_SUBMITTED,
        contract.AckKind.TRANSFER_COMPLETED,
        contract.AckKind.CACHE_VERIFIED,
    ):
        events.append(session.advance_kind(kind))
    token_digest = _digest("serverlessx-cpu-demo-eight-token-ids")
    events.append(
        session.advance_kind(
            contract.AckKind.DECODE_DONE,
            token_digest_sha256=token_digest,
        )
    )
    events.append(session.advance_kind(contract.AckKind.RELEASED))
    events.append(session.advance_kind(contract.AckKind.SOURCE_RELEASED))

    replay_rejected = False
    try:
        session.advance(events[-1])
    except contract.ProtocolError:
        replay_rejected = True
    if not session.closed or not replay_rejected:
        raise CliError("CPU contract simulation did not close safely", EXIT_RUN_FAILED)

    result: Dict[str, Any] = {
        "schema": RESULT_SCHEMA,
        "schema_version": RESULT_SCHEMA_VERSION,
        "run_id": run_id,
        "status": "pass",
        "system": "spd",
        "profile": "cpu",
        "mode": "contract-simulation",
        "started_at": started_at,
        "finished_at": _utc_now(),
        "metadata": metadata,
        "events": events,
        "outcome": {
            "state": session.state,
            "closed": session.closed,
            "owners": session.owners,
        },
        "checks": {
            "completed_ack_sequence": [kind.value for kind in contract.ACK_SEQUENCE],
            "replay_rejected": replay_rejected,
        },
        "claims": {
            "contract_simulation": True,
            "kv_payload_transferred": False,
            "model_inference_performed": False,
            "gpu_execution_performed": False,
        },
    }

    runs_root = _runs_root()
    try:
        runs_root.mkdir(parents=True, exist_ok=True)
        run_dir = runs_root / run_id
        run_dir.mkdir(mode=0o700)
        result_path = run_dir / "result.json"
        temporary = run_dir / ".result.json.tmp"
        with temporary.open("x", encoding="utf-8") as handle:
            json.dump(result, handle, ensure_ascii=True, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(str(temporary), str(result_path))
    except OSError as exc:
        raise CliError("cannot write run evidence: {}".format(exc), EXIT_RUN_FAILED)
    return result, result_path


def _exact_mapping(value: Any, keys: Iterable[str], name: str) -> Mapping[str, Any]:
    wanted = set(keys)
    if not isinstance(value, Mapping):
        raise CliError("{} must be an object".format(name), EXIT_VERIFY_FAILED)
    actual = set(value)
    if actual != wanted:
        raise CliError(
            "{} keys differ: missing={}, unknown={}".format(
                name, sorted(wanted - actual), sorted(actual - wanted)
            ),
            EXIT_VERIFY_FAILED,
        )
    return value


def _result_path(run_id: Optional[str]) -> Path:
    runs_root = _runs_root()
    if run_id is not None:
        if SAFE_RUN_ID.fullmatch(run_id) is None:
            raise CliError("run id has an invalid format", EXIT_VERIFY_FAILED)
        candidate = runs_root / run_id / "result.json"
        if candidate.parent.is_symlink():
            raise CliError("refusing a symlinked run directory", EXIT_VERIFY_FAILED)
        return candidate
    if not runs_root.is_dir():
        raise CliError("no runs directory found at {}".format(runs_root), EXIT_VERIFY_FAILED)
    candidates: List[Path] = []
    try:
        for entry in runs_root.iterdir():
            if entry.is_symlink() or not entry.is_dir() or SAFE_RUN_ID.fullmatch(entry.name) is None:
                continue
            candidate = entry / "result.json"
            if not candidate.is_symlink() and candidate.is_file():
                candidates.append(candidate)
    except OSError as exc:
        raise CliError("cannot inspect runs directory: {}".format(exc), EXIT_VERIFY_FAILED)
    if not candidates:
        raise CliError("no run result is available", EXIT_VERIFY_FAILED)
    return max(candidates, key=lambda path: (path.stat().st_mtime_ns, path.parent.name))


def _parse_time(value: Any, name: str) -> dt.datetime:
    if not isinstance(value, str):
        raise CliError("{} must be a UTC timestamp".format(name), EXIT_VERIFY_FAILED)
    try:
        parsed = dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%S.%fZ")
    except ValueError as exc:
        raise CliError("{} must be a UTC timestamp: {}".format(name, exc), EXIT_VERIFY_FAILED)
    return parsed.replace(tzinfo=dt.timezone.utc)


def verify_result(run_id: Optional[str] = None) -> Dict[str, Any]:
    path = _result_path(run_id)
    result = _exact_mapping(_load_json(path), RESULT_KEYS, "result")
    if (
        result["schema"] != RESULT_SCHEMA
        or not _is_int(result["schema_version"])
        or result["schema_version"] != RESULT_SCHEMA_VERSION
    ):
        raise CliError("result schema identity mismatch", EXIT_VERIFY_FAILED)
    if not isinstance(result["run_id"], str) or SAFE_RUN_ID.fullmatch(result["run_id"]) is None:
        raise CliError("result run_id is invalid", EXIT_VERIFY_FAILED)
    if result["run_id"] != path.parent.name:
        raise CliError("result run_id does not match its directory", EXIT_VERIFY_FAILED)
    if result["status"] != "pass":
        raise CliError("result does not report pass", EXIT_VERIFY_FAILED)
    if result["system"] != "spd" or result["profile"] != "cpu":
        raise CliError("result is not an SPD CPU profile run", EXIT_VERIFY_FAILED)
    if result["mode"] != "contract-simulation":
        raise CliError("result mode is not contract-simulation", EXIT_VERIFY_FAILED)
    started = _parse_time(result["started_at"], "started_at")
    finished = _parse_time(result["finished_at"], "finished_at")
    if finished < started:
        raise CliError("finished_at precedes started_at", EXIT_VERIFY_FAILED)

    try:
        metadata = contract.validate_metadata(result["metadata"])
        ledger = contract.EpochLedger()
        session = contract.TransferSession(metadata, ledger=ledger)
        events = result["events"]
        if not isinstance(events, list) or len(events) != len(contract.ACK_SEQUENCE):
            raise CliError("result must contain the complete ACK sequence", EXIT_VERIFY_FAILED)
        for event in events:
            session.advance(event)
    except contract.ContractError as exc:
        raise CliError("contract evidence failed replay: {}".format(exc), EXIT_VERIFY_FAILED)

    outcome = _exact_mapping(result["outcome"], OUTCOME_KEYS, "outcome")
    owners = outcome.get("owners")
    if (
        type(outcome.get("closed")) is not bool
        or not isinstance(owners, Mapping)
        or set(owners) != {"source", "target"}
        or type(owners.get("source")) is not bool
        or type(owners.get("target")) is not bool
    ):
        raise CliError("outcome ownership fields must be strict booleans", EXIT_VERIFY_FAILED)
    if outcome != {
        "state": contract.AckKind.SOURCE_RELEASED.value,
        "closed": True,
        "owners": {"source": False, "target": False},
    }:
        raise CliError("recorded outcome is not the closed ownership state", EXIT_VERIFY_FAILED)
    if not session.closed or session.state != outcome["state"] or session.owners != outcome["owners"]:
        raise CliError("replayed state differs from the recorded outcome", EXIT_VERIFY_FAILED)

    checks = _exact_mapping(result["checks"], CHECK_KEYS, "checks")
    expected_sequence = [kind.value for kind in contract.ACK_SEQUENCE]
    if checks["completed_ack_sequence"] != expected_sequence:
        raise CliError("recorded ACK sequence is incomplete", EXIT_VERIFY_FAILED)
    replay_rejected = False
    try:
        session.advance(events[-1])
    except contract.ProtocolError:
        replay_rejected = True
    if checks["replay_rejected"] is not True or not replay_rejected:
        raise CliError("replay rejection evidence is missing", EXIT_VERIFY_FAILED)

    claims = _exact_mapping(result["claims"], CLAIM_KEYS, "claims")
    if any(type(claims[name]) is not bool for name in CLAIM_KEYS):
        raise CliError("claim fields must be strict booleans", EXIT_VERIFY_FAILED)
    if claims != {
        "contract_simulation": True,
        "kv_payload_transferred": False,
        "model_inference_performed": False,
        "gpu_execution_performed": False,
    }:
        raise CliError("result makes unsupported CPU simulation claims", EXIT_VERIFY_FAILED)
    return {
        "schema": VERIFICATION_SCHEMA,
        "status": "pass",
        "run_id": result["run_id"],
        "result_path": _relative_display(path),
        "checks": [
            "schema",
            "timestamps",
            "metadata",
            "ack-replay",
            "ownership-closure",
            "claim-boundary",
        ],
    }


def _emit_json(payload: Mapping[str, Any], stream: Any = None) -> None:
    if stream is None:
        stream = sys.stdout
    json.dump(payload, stream, ensure_ascii=True, indent=2, sort_keys=True)
    stream.write("\n")


def _yes_no(value: bool) -> str:
    return "yes" if value else "no"


def _emit_profiles_text(payload: Mapping[str, Any]) -> None:
    print("PROFILE       AVAILABLE  COMPATIBLE  MATURITY       TITLE")
    for entry in payload["profiles"]:
        print(
            "{:<13} {:<10} {:<11} {:<14} {}".format(
                entry["id"],
                _yes_no(entry["available"]),
                _yes_no(entry["compatible"]),
                entry["maturity"],
                entry["title"],
            )
        )


def _emit_doctor_text(payload: Mapping[str, Any]) -> None:
    host = payload["host"]
    print("ServerlessX doctor (read-only)")
    print("Host:       {} ({} {})".format(host["hostname"], host["platform"], host["machine"]))
    print("Python:     {}".format(host["python"]))
    print("NVIDIA:     {} command, {} visible GPU(s)".format(
        "found" if payload["nvidia"]["command_available"] else "no",
        len(payload["nvidia"]["gpus"]),
    ))
    print("RDMA:       {} device(s)".format(len(payload["rdma"]["devices"])))
    containers = [name for name, found in payload["containers"].items() if found]
    print("Containers: {}".format(", ".join(containers) if containers else "none found"))
    print("Runnable:   {}".format(
        ", ".join(item["id"] for item in payload["profiles"] if item["runnable"]) or "none"
    ))


def _emit_plan_text(payload: Mapping[str, Any]) -> None:
    print("ServerlessX plan: {}".format(payload["status"]))
    print("System:  {}".format(payload["system"]))
    print("Profile: {}".format(payload["selected_profile"] or "none"))
    if payload["reason"]:
        print("Reason:  {}".format(payload["reason"]))
    if payload["steps"]:
        print("Steps:")
        for index, step in enumerate(payload["steps"], 1):
            print("  {}. {}".format(index, step))
    if payload["privileged_operations"]:
        print("Privileged operations (not executed):")
        for operation in payload["privileged_operations"]:
            print("  - {}".format(operation))
    else:
        print("Privileged operations: none")


def _format_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="output format (default: text)",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sx",
        description="Inspect and run the ServerlessX research artifact bootstrap.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    profiles_parser = subparsers.add_parser("profiles", help="list declared profiles")
    _format_arg(profiles_parser)

    doctor_parser = subparsers.add_parser("doctor", help="run read-only host checks")
    _format_arg(doctor_parser)

    plan_parser = subparsers.add_parser("plan", help="select and describe a profile")
    plan_parser.add_argument("--profile", default="auto", help="profile id or auto")
    _format_arg(plan_parser)

    run_parser = subparsers.add_parser("run", help="run one supported system")
    run_parser.add_argument("system", choices=("spd",))
    run_parser.add_argument("--profile", default="auto", help="profile id or auto")
    _format_arg(run_parser)

    verify_parser = subparsers.add_parser("verify", help="verify saved run evidence")
    selection = verify_parser.add_mutually_exclusive_group()
    selection.add_argument("--latest", action="store_true", help="verify the newest run")
    selection.add_argument("--run-id", help="verify one generated run id")
    _format_arg(verify_parser)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    output_format = args.format
    try:
        if args.command == "profiles":
            payload = profiles_payload()
            if output_format == "json":
                _emit_json(payload)
            else:
                _emit_profiles_text(payload)
            return EXIT_OK

        if args.command == "doctor":
            payload = doctor_payload()
            if output_format == "json":
                _emit_json(payload)
            else:
                _emit_doctor_text(payload)
            return EXIT_OK

        if args.command == "plan":
            payload = build_plan(args.profile)
            if output_format == "json":
                _emit_json(payload)
            else:
                _emit_plan_text(payload)
            return EXIT_OK if payload["status"] == "ready" else EXIT_UNAVAILABLE

        if args.command == "run":
            plan = build_plan(args.profile, system=args.system)
            if plan["status"] != "ready":
                if output_format == "json":
                    _emit_json(plan)
                else:
                    _emit_plan_text(plan)
                return EXIT_UNAVAILABLE
            if plan["selected_profile"] != "cpu":
                raise CliError("only the CPU profile is executable in this bootstrap", EXIT_UNAVAILABLE)
            result, result_path = run_cpu_simulation()
            payload = {
                "schema": "serverlessx.run.v1",
                "status": result["status"],
                "run_id": result["run_id"],
                "profile": result["profile"],
                "mode": result["mode"],
                "result_path": _relative_display(result_path),
            }
            if output_format == "json":
                _emit_json(payload)
            else:
                print("ServerlessX run: {}".format(payload["status"]))
                print("Run:     {}".format(payload["run_id"]))
                print("Profile: {} ({})".format(payload["profile"], payload["mode"]))
                print("Evidence: {}".format(payload["result_path"]))
            return EXIT_OK

        if args.command == "verify":
            payload = verify_result(run_id=args.run_id)
            if output_format == "json":
                _emit_json(payload)
            else:
                print("ServerlessX verification: {}".format(payload["status"]))
                print("Run:      {}".format(payload["run_id"]))
                print("Evidence: {}".format(payload["result_path"]))
                print("Checks:   {}".format(", ".join(payload["checks"])))
            return EXIT_OK
        parser.error("unknown command")
        return EXIT_USAGE
    except CliError as exc:
        if output_format == "json":
            _emit_json(
                {
                    "schema": ERROR_SCHEMA,
                    "status": "error",
                    "exit_code": exc.exit_code,
                    "message": str(exc),
                }
            )
        else:
            print("error: {}".format(exc), file=sys.stderr)
        return exc.exit_code
    except contract.ContractError as exc:
        wrapped = CliError("contract execution failed: {}".format(exc), EXIT_RUN_FAILED)
        if output_format == "json":
            _emit_json(
                {
                    "schema": ERROR_SCHEMA,
                    "status": "error",
                    "exit_code": wrapped.exit_code,
                    "message": str(wrapped),
                }
            )
        else:
            print("error: {}".format(wrapped), file=sys.stderr)
        return wrapped.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
