from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from serverlessx import cli


class CliTests(unittest.TestCase):
    def test_auto_plan_selects_only_declared_available_profile(self) -> None:
        plan = cli.build_plan("auto")
        self.assertEqual("ready", plan["status"])
        self.assertEqual("cpu", plan["selected_profile"])
        self.assertEqual([], plan["privileged_operations"])

    def test_plan_keeps_unavailable_profile_blocked(self) -> None:
        plan = cli.build_plan("phos_lab")
        self.assertEqual("blocked", plan["status"])
        self.assertEqual("phos_lab", plan["selected_profile"])
        self.assertIn("site-specific", plan["reason"])

    def test_cpu_run_and_verify_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.dict(os.environ, {"SERVERLESSX_RUNS_DIR": temporary}):
                result, result_path = cli.run_cpu_simulation()
                self.assertEqual("pass", result["status"])
                self.assertTrue(result_path.is_file())
                checked = cli.verify_result(result["run_id"])
                self.assertEqual("pass", checked["status"])
                self.assertEqual(result["run_id"], checked["run_id"])

    def test_verify_rejects_tampering_and_numeric_booleans(self) -> None:
        mutations = (
            lambda value: value["claims"].__setitem__("gpu_execution_performed", True),
            lambda value: value.__setitem__("schema_version", True),
            lambda value: value["outcome"].__setitem__("closed", 1),
            lambda value: value["outcome"]["owners"].__setitem__("source", 0),
        )
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index), tempfile.TemporaryDirectory() as temporary:
                with mock.patch.dict(os.environ, {"SERVERLESSX_RUNS_DIR": temporary}):
                    result, result_path = cli.run_cpu_simulation()
                    value = json.loads(result_path.read_text(encoding="utf-8"))
                    mutate(value)
                    result_path.write_text(json.dumps(value), encoding="utf-8")
                    with self.assertRaises(cli.CliError):
                        cli.verify_result(result["run_id"])

    def test_json_command_output_is_machine_readable(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = cli.main(["plan", "--profile", "auto", "--format", "json"])
        self.assertEqual(0, status)
        self.assertEqual("serverlessx.plan.v1", json.loads(output.getvalue())["schema"])


if __name__ == "__main__":
    unittest.main()
