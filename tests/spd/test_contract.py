from __future__ import annotations

import copy
import hashlib
import json
import unittest

from serverlessx.spd import contract


TOKEN_DIGEST = hashlib.sha256(b"eight deterministic token ids").hexdigest()


def make_metadata(
    *,
    request_id: str = "spd-request-01",
    epoch: int = 7,
    attempt_id: str = "attempt-07",
) -> dict:
    return {
        "schema": contract.SCHEMA,
        "schema_version": contract.SCHEMA_VERSION,
        "request_id": request_id,
        "epoch": epoch,
        "attempt_id": attempt_id,
        "model_id": "TinyLlama/TinyLlama-1.1B-Chat-v1.0",
        "model_sha256": "1" * 64,
        "tokenizer_sha256": "2" * 64,
        "prompt_digest_sha256": "3" * 64,
        "first_token_id": 2008,
        "kv_layout": contract.KV_LAYOUT,
        "dtype": "float16",
        "prompt_length": 128,
        "output_tokens": 8,
        "tensors": contract.build_fixed_tensors(),
        "payload_bytes": contract.EXPECTED_KV_BYTES,
        "payload_sha256": "4" * 64,
        "source_owner_id": "source-p.123.456",
        "target_owner_id": "target-d.789.012",
        "buffer_id": f"buffer-{attempt_id}",
        "source_gpu_uuid": "GPU-11111111-1111-1111-1111-111111111111",
        "target_gpu_uuid": "GPU-22222222-2222-2222-2222-222222222222",
        "source_endpoint": "192.0.2.12:12855",
        "target_endpoint": "192.0.2.13:12856",
    }


def advance_to_decode(session: contract.TransferSession) -> None:
    for kind in (
        contract.AckKind.TARGET_READY,
        contract.AckKind.TRANSFER_SUBMITTED,
        contract.AckKind.TRANSFER_COMPLETED,
        contract.AckKind.CACHE_VERIFIED,
    ):
        session.advance_kind(kind)


class MetadataContractTests(unittest.TestCase):
    def test_valid_metadata_round_trip_is_canonical_and_detached(self) -> None:
        source = make_metadata()
        checked = contract.validate_metadata(source)
        encoded = contract.canonical_json_bytes(checked)
        parsed = contract.parse_metadata_json(encoded)
        self.assertEqual(source, parsed)
        self.assertEqual(encoded, contract.canonical_json_bytes(parsed))
        self.assertEqual(64, len(contract.canonical_sha256(parsed)))

        source["tensors"][0]["shape"][0] = 9
        self.assertEqual([1, 4, 128, 64], checked["tensors"][0]["shape"])

    def test_duplicate_unknown_and_missing_keys_are_rejected(self) -> None:
        with self.assertRaisesRegex(contract.MetadataError, "duplicate key"):
            contract.parse_metadata_json(
                '{"schema":"a","schema":"b"}'
            )

        for mutation in ("missing", "unknown"):
            value = make_metadata()
            if mutation == "missing":
                value.pop("buffer_id")
            else:
                value["surprise"] = True
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                contract.MetadataError, "keys differ"
            ):
                contract.validate_metadata(value)

    def test_identity_and_fixed_shape_mismatches_are_rejected(self) -> None:
        mutations = {
            "epoch": True,
            "prompt_length": 127,
            "payload_bytes": contract.EXPECTED_KV_BYTES - 2,
            "payload_sha256": "A" * 64,
            "source_endpoint": "192.0.2.012:12855",
            "target_gpu_uuid": make_metadata()["source_gpu_uuid"],
            "target_owner_id": make_metadata()["source_owner_id"],
        }
        for field, replacement in mutations.items():
            value = make_metadata()
            value[field] = replacement
            with self.subTest(field=field), self.assertRaises(contract.MetadataError):
                contract.validate_metadata(value)

        value = make_metadata()
        value["tensors"][3]["offset_bytes"] += 2
        with self.assertRaisesRegex(contract.MetadataError, "offset_bytes"):
            contract.validate_metadata(value)

        value = make_metadata()
        value["tensors"][0]["shape"][0] = True
        with self.assertRaisesRegex(contract.MetadataError, "shape"):
            contract.validate_metadata(value)

    def test_expected_identity_is_exact_and_type_strict(self) -> None:
        value = make_metadata()
        checked = contract.validate_metadata(
            value,
            expected={
                "request_id": "spd-request-01",
                "epoch": 7,
                "model_sha256": "1" * 64,
            },
        )
        self.assertEqual(7, checked["epoch"])
        with self.assertRaisesRegex(contract.MetadataError, "epoch mismatch"):
            contract.validate_metadata(value, expected={"epoch": 8})
        with self.assertRaisesRegex(contract.MetadataError, "unknown fields"):
            contract.validate_metadata(value, expected={"not_a_field": 1})

    def test_non_finite_json_and_oversize_input_are_rejected(self) -> None:
        with self.assertRaisesRegex(contract.MetadataError, "non-finite"):
            contract.parse_metadata_json('{"x":NaN}')
        with self.assertRaisesRegex(contract.MetadataError, "size"):
            contract.parse_metadata_json("x" * (contract.MAX_JSON_BYTES + 1))


class EpochLedgerTests(unittest.TestCase):
    def test_ledger_rejects_duplicate_and_stale_epoch_but_accepts_fresh(self) -> None:
        ledger = contract.EpochLedger()
        first = make_metadata(epoch=7, attempt_id="a7")
        ledger.reserve(first)
        self.assertEqual((7, "a7"), ledger.current(first["request_id"]))

        for epoch, attempt in ((7, "a7"), (7, "different"), (6, "a6")):
            with self.subTest(epoch=epoch, attempt=attempt), self.assertRaisesRegex(
                contract.ProtocolError, "epoch"
            ):
                ledger.reserve(make_metadata(epoch=epoch, attempt_id=attempt))

        fresh = make_metadata(epoch=8, attempt_id="a8")
        ledger.reserve(fresh)
        ledger.assert_current(fresh)
        with self.assertRaisesRegex(contract.ProtocolError, "current"):
            ledger.assert_current(first)

        reused_attempt = make_metadata(epoch=9, attempt_id="a8")
        reused_attempt["buffer_id"] = "different-buffer"
        with self.assertRaisesRegex(contract.ProtocolError, "attempt_id"):
            ledger.reserve(reused_attempt)

        reused_buffer = make_metadata(epoch=9, attempt_id="a9")
        reused_buffer["buffer_id"] = fresh["buffer_id"]
        with self.assertRaisesRegex(contract.ProtocolError, "buffer_id"):
            ledger.reserve(reused_buffer)

    def test_epochs_are_scoped_per_request(self) -> None:
        ledger = contract.EpochLedger()
        ledger.reserve(make_metadata(request_id="request-a", epoch=10, attempt_id="a10"))
        ledger.reserve(make_metadata(request_id="request-b", epoch=0, attempt_id="b0"))
        self.assertEqual((0, "b0"), ledger.current("request-b"))


class AckContractTests(unittest.TestCase):
    def test_happy_path_tracks_owner_lifetime_and_closes(self) -> None:
        metadata = make_metadata()
        session = contract.TransferSession(metadata)
        self.assertEqual("INIT", session.state)
        self.assertEqual({"source": True, "target": False}, session.owners)

        session.advance_kind(contract.AckKind.TARGET_READY)
        self.assertEqual({"source": True, "target": True}, session.owners)
        session.advance_kind(contract.AckKind.TRANSFER_SUBMITTED)
        self.assertEqual({"source": True, "target": True}, session.owners)
        session.advance_kind(contract.AckKind.TRANSFER_COMPLETED)
        session.advance_kind(contract.AckKind.CACHE_VERIFIED)
        session.advance_kind(
            contract.AckKind.DECODE_DONE,
            token_digest_sha256=TOKEN_DIGEST,
        )
        session.advance_kind(contract.AckKind.RELEASED)
        self.assertEqual({"source": True, "target": False}, session.owners)
        session.advance_kind(contract.AckKind.SOURCE_RELEASED)
        self.assertEqual("SOURCE_RELEASED", session.state)
        self.assertTrue(session.closed)
        self.assertEqual({"source": False, "target": False}, session.owners)

    def test_ack_round_trip_and_exact_key_contract(self) -> None:
        metadata = make_metadata()
        ack = contract.make_ack(contract.AckKind.TARGET_READY, metadata)
        checked = contract.validate_ack(
            json.loads(contract.canonical_json_bytes(ack)),
            metadata,
            expected_kind=contract.AckKind.TARGET_READY,
        )
        self.assertEqual(ack, checked)

        malformed = copy.deepcopy(ack)
        malformed["extra"] = 1
        with self.assertRaisesRegex(contract.ProtocolError, "keys differ"):
            contract.validate_ack(malformed, metadata)

        wrong_version_type = copy.deepcopy(ack)
        wrong_version_type["schema_version"] = True
        with self.assertRaisesRegex(contract.ProtocolError, "schema identity"):
            contract.validate_ack(wrong_version_type, metadata)

    def test_wrong_order_replay_and_owner_fail_without_state_change(self) -> None:
        metadata = make_metadata()
        session = contract.TransferSession(metadata)

        skipped = contract.make_ack(contract.AckKind.TRANSFER_SUBMITTED, metadata)
        with self.assertRaisesRegex(contract.ProtocolError, "expected ACK"):
            session.advance(skipped)
        self.assertEqual("INIT", session.state)
        self.assertEqual({"source": True, "target": False}, session.owners)

        first = session.advance_kind(contract.AckKind.TARGET_READY)
        with self.assertRaisesRegex(contract.ProtocolError, "expected ACK"):
            session.advance(first)
        self.assertEqual("TARGET_READY", session.state)

        wrong_owner = contract.make_ack(
            contract.AckKind.TRANSFER_SUBMITTED,
            metadata,
            owner_id=metadata["target_owner_id"],
        )
        with self.assertRaises(contract.OwnershipError):
            session.advance(wrong_owner)
        self.assertEqual({"source": True, "target": True}, session.owners)

    def test_wrong_epoch_buffer_and_hash_fail_before_state_change(self) -> None:
        metadata = make_metadata()
        session = contract.TransferSession(metadata)
        session.advance_kind(contract.AckKind.TARGET_READY)
        session.advance_kind(contract.AckKind.TRANSFER_SUBMITTED)

        for field, replacement in (
            ("epoch", 8),
            ("buffer_id", "other-buffer"),
            ("payload_sha256", "5" * 64),
        ):
            ack = contract.make_ack(contract.AckKind.TRANSFER_COMPLETED, metadata)
            ack[field] = replacement
            with self.subTest(field=field), self.assertRaises(contract.ContractError):
                session.advance(ack)
            self.assertEqual("TRANSFER_SUBMITTED", session.state)

    def test_verified_and_decode_acks_require_their_proofs(self) -> None:
        metadata = make_metadata()
        session = contract.TransferSession(metadata)
        session.advance_kind(contract.AckKind.TARGET_READY)
        session.advance_kind(contract.AckKind.TRANSFER_SUBMITTED)
        session.advance_kind(contract.AckKind.TRANSFER_COMPLETED)

        verified = contract.make_ack(contract.AckKind.CACHE_VERIFIED, metadata)
        verified["payload_sha256"] = None
        with self.assertRaisesRegex(contract.ProtocolError, "payload_sha256"):
            session.advance(verified)
        session.advance_kind(contract.AckKind.CACHE_VERIFIED)

        without_tokens = contract.make_ack(contract.AckKind.DECODE_DONE, metadata)
        with self.assertRaisesRegex(contract.ProtocolError, "token_digest_sha256"):
            session.advance(without_tokens)
        session.advance_kind(
            contract.AckKind.DECODE_DONE,
            token_digest_sha256=TOKEN_DIGEST,
        )

        wrong_release = contract.make_ack(
            contract.AckKind.RELEASED,
            metadata,
            token_digest_sha256="6" * 64,
        )
        with self.assertRaisesRegex(contract.ProtocolError, "differs"):
            session.advance(wrong_release)
        self.assertEqual("DECODE_DONE", session.state)

    def test_abort_blocks_success_and_requires_exact_owner_cleanup(self) -> None:
        metadata = make_metadata()
        session = contract.TransferSession(metadata)
        session.advance_kind(contract.AckKind.TARGET_READY)
        session.advance_kind(contract.AckKind.TRANSFER_SUBMITTED)
        session.abort("target hash mismatch")
        self.assertEqual("ABORTED", session.state)
        self.assertFalse(session.closed)

        with self.assertRaisesRegex(contract.ProtocolError, "aborted"):
            session.advance_kind(contract.AckKind.TRANSFER_COMPLETED)
        with self.assertRaises(contract.OwnershipError):
            session.cleanup_owner("unrelated-owner")
        session.cleanup_owner(metadata["target_owner_id"])
        session.cleanup_owner(metadata["source_owner_id"])
        self.assertTrue(session.closed)
        with self.assertRaises(contract.OwnershipError):
            session.cleanup_owner(metadata["source_owner_id"])

    def test_ledger_prevents_old_attempt_from_opening_new_session(self) -> None:
        ledger = contract.EpochLedger()
        old = make_metadata(epoch=7, attempt_id="a7")
        first = contract.TransferSession(old, ledger=ledger)
        first.abort("controlled failure before target ready")
        self.assertFalse(first.closed)
        first.cleanup_owner(old["source_owner_id"])
        self.assertTrue(first.closed)

        fresh = make_metadata(epoch=8, attempt_id="a8")
        contract.TransferSession(fresh, ledger=ledger)
        with self.assertRaisesRegex(contract.ProtocolError, "epoch"):
            contract.TransferSession(old, ledger=ledger)


if __name__ == "__main__":
    unittest.main()
