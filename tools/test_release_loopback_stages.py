import argparse
import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from run_release_loopback import run_diagnostic_first


class DiagnosticFirst(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.output = Path(temp.name).resolve()
        self.diagnostic = self.output / "diagnostic"
        self.diagnostic.mkdir()
        self.args = argparse.Namespace(visible=True, asio_pooled_handler=True,
                                       ndi_handoff_guard_us=0, basic_tests=True, seconds=600,
                                       diagnostic_first=15, compare_to=self.output / "baseline.json")
        self.data = {"joint_scope": {"eligible": True},
                     "sender": {"config": {"target_ip": "127.0.0.1"}},
                     "receiver": {"modules": {"1": {"datagrams": 10}, "2": {"datagrams": 12}}}}
        self.write("joint.json", self.data)
        self.write("basic-tests.json", {"exit_code": 0})
        self.write("run.json", {"timed_out": False})

    def write(self, name, report):
        (self.diagnostic / name).write_text(json.dumps(report), encoding="utf-8")

    def simulate(self, *codes):
        results = [subprocess.CompletedProcess([], code) for code in codes]
        with patch("run_release_loopback.subprocess.run", side_effect=results) as launch:
            with redirect_stdout(io.StringIO()) as output:
                result = run_diagnostic_first(self.args, self.output / "build", self.output)
            return result, [call.args[0] for call in launch.call_args_list], output.getvalue()

    def test_formal_after_measured_diagnostic(self):
        for diagnostic_code, formal_code in ((2, 0), (0, 2), (2, 1)):
            with self.subTest(diagnostic_code=diagnostic_code, formal_code=formal_code):
                result, (short, long), _ = self.simulate(diagnostic_code, formal_code)
                self.assertEqual(result, formal_code)
                for command in (short, long):
                    self.assertEqual(command[command.index("--build") + 1], str(self.output / "build"))
                    self.assertIn("--visible", command)
                    self.assertIn("--asio-pooled-handler", command)
                    self.assertEqual(command[command.index("--ndi-handoff-guard-us") + 1], "0")
                for flag in ("--basic-tests", "--fragment-trace"):
                    self.assertIn(flag, short)
                    self.assertNotIn(flag, long)
                self.assertNotIn("--compare-to", short)
                self.assertEqual(long[long.index("--compare-to") + 1], str(self.args.compare_to))
                self.assertEqual(short[short.index("--output") + 1], str(self.diagnostic))
                self.assertEqual(long[long.index("--output") + 1], str(self.output / "formal"))
                self.assertEqual(short[short.index("--seconds") + 1], "15")
                self.assertEqual(long[long.index("--seconds") + 1], "600")

    def assert_stopped(self, diagnostic_code, expected_code, reason):
        result, commands, message = self.simulate(diagnostic_code)
        self.assertEqual(result, expected_code)
        self.assertEqual(len(commands), 1)
        self.assertIn(reason, message)

    def test_basic_failure(self):
        self.write("basic-tests.json", {"exit_code": 2})
        self.assert_stopped(2, 2, "basic tests failed")

    def test_timeout(self):
        self.write("run.json", {"timed_out": True})
        self.assert_stopped(1, 1, "timed out")

    def test_missing_report(self):
        (self.diagnostic / "joint.json").unlink()
        self.assert_stopped(2, 1, "missing or unreadable")

    def test_no_ep_reception(self):
        self.data["receiver"]["modules"]["2"]["datagrams"] = 0
        self.write("joint.json", self.data)
        self.assert_stopped(2, 3, "reception was not confirmed")

    def test_unpaired_receivers(self):
        self.data["joint_scope"]["eligible"] = False
        self.write("joint.json", self.data)
        self.assert_stopped(2, 3, "reception was not confirmed")


if __name__ == "__main__":
    unittest.main()
