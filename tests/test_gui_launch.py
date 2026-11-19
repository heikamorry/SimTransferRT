"""Exercise model launch, smoke and screenshot options through the actual application."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


exe = Path(sys.argv.pop(1)).resolve()
fixture = Path(sys.argv.pop(1)).resolve()
png_signature = b"\x89PNG\r\n\x1a\n"


class GuiLaunchTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.directory = Path(work.name)

    def launch(self, *args):
        return subprocess.run([exe, *args], cwd=self.directory, capture_output=True,
                              timeout=15, check=False).returncode

    def test_smoke_finishes_screenshot_before_exiting(self):
        image = self.directory / "window.png"
        self.assertEqual(self.launch("--v1", "--smoke-test", "--screenshot", image), 0)
        self.assertTrue(image.read_bytes().startswith(png_signature))

    def test_missing_model_argument_returns_usage_error(self):
        self.assertEqual(self.launch("--smoke-test", "--model"), 2)

    def test_v1_cannot_claim_model_preview_succeeded(self):
        image = self.directory / "unused.png"
        self.assertEqual(self.launch("--v1", "--model", fixture, "--screenshot", image), 2)
        self.assertFalse(image.exists())

    def test_capture_cannot_replace_model_with_its_image(self):
        # Content detection allows a PLY to have any filename.
        source = self.directory / "原模型.png"
        contents = fixture.read_bytes()
        source.write_bytes(contents)
        self.assertNotEqual(self.launch("--model", source, "--screenshot", source), 0)
        self.assertEqual(source.read_bytes(), contents)

    def test_failed_load_preserves_last_good_capture(self):
        image = self.directory / "last-good.png"
        self.assertEqual(self.launch("--model", fixture, "--screenshot", image), 0)
        contents = image.read_bytes()
        self.assertTrue(contents.startswith(png_signature))
        self.assertNotEqual(self.launch("--model", "missing.ply", "--screenshot", image), 0)
        self.assertEqual(image.read_bytes(), contents)

    def test_smoke_reports_model_failure_without_screenshot(self):
        self.assertNotEqual(self.launch("--smoke-test", "--model", "missing.ply"), 0)


if __name__ == "__main__":
    unittest.main()
