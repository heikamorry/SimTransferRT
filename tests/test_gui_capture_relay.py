"""Exercise WPR handoff file ownership without starting ETW or requesting elevation."""
import uuid
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "tools/handoff_etw/capture-gui.py"
spec = importlib.util.spec_from_file_location("capture_gui", SOURCE)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


@unittest.skipUnless(os.name == "nt", "Windows collector test")
class GuiCaptureRelayTest(unittest.TestCase):
    def run_fake_collector(self, output, start_code=0, stop_code=0):
        instance = "SimTransferGui-" + uuid.uuid4().hex
        capture.prepare_relay(output, "wpr-elevated")
        etl = output / "scheduler.etl"
        etl.touch(exist_ok=False)
        (output / "stop.request").touch()
        owned = {f: f.stat().st_ino for f in output.iterdir()}
        self.assertNotIn(0, owned.values())
        self.assertEqual((output / "wpr-elevated.start-ready").stat().st_size, 0)
        script = capture.collector_script(output, output / "mock.wprp", instance, "wpr-elevated", 30)
        stub = """
function Test-Wpr {
    if ($args[0] -eq '-start') {
        $global:LASTEXITCODE=START_CODE
        'start logged'
    } elseif ($args[0] -eq '-stop') {
        [IO.File]::WriteAllBytes($args[1], [byte[]](0,1,127,254,255))
        $global:LASTEXITCODE=STOP_CODE
        'stop logged'
    } elseif ($args[0] -eq '-cancel') {
        'only private instance cancelled'
        $global:LASTEXITCODE=0
    }
}
$wpr='Test-Wpr'
""".replace("START_CODE", str(start_code)).replace("STOP_CODE", str(stop_code))
        script = script.replace("$wpr=Join-Path ([Environment]::SystemDirectory) 'wpr.exe'", stub)
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand",
                                 capture.encoded(script)], capture_output=True, timeout=15)
        self.assertEqual({f: f.stat().st_ino for f in output.iterdir()}, owned,
                         "parent-owned output must not be replaced")
        self.assertEqual((output / "wpr-elevated.start-ready").read_bytes(), b"1")
        self.assertEqual((output / "wpr-elevated.start-code").read_text(), str(start_code))
        self.assertIn("start logged", (output / "wpr-elevated.start.log").read_text())
        self.assertFalse((Path(tempfile.gettempdir()) / (instance + ".etl")).exists())
        return result.returncode

    def test_success_and_failure_do_not_replace_user_files(self):
        for start, stop, expected in [(0, 0, 0), (5, 0, 1), (0, 2, 2)]:
            with self.subTest(start=start, stop=stop), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp)
                self.assertEqual(self.run_fake_collector(output, start, stop), expected)
                etl_bytes = (output / "scheduler.etl").read_bytes()
                self.assertEqual(etl_bytes, bytes([0, 1, 127, 254, 255]) if expected == 0 else b"")
                cancel = (output / "wpr-elevated.cancel.log").read_text()
                self.assertEqual(bool(cancel), bool(stop))


if __name__ == "__main__":
    unittest.main()
