"""Run the existing V2 loopback acceptance case with and without a separate preview GUI."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_v2_validation import run_case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("gui", "sender", "model", "output"):
        parser.add_argument("--" + argument, required=True, type=Path)
    parser.add_argument("--seconds", type=int, default=5)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sender = args.sender.resolve()
    runs = []
    for label in ("baseline", "concurrent-preview"):
        gui = None
        capture = output / "preview.png"
        started = time.monotonic()
        try:
            if label == "concurrent-preview":
                # The ordinary GUI exits after its requested first-frame screenshot.
                # Log the overlap so a still-loading model cannot be described as idle.
                gui = subprocess.Popen([args.gui.resolve(), "--model", args.model.resolve(),
                                        "--screenshot", capture], cwd=args.gui.resolve().parent)
            result = run_case(sender, output, label, "1g", seconds=args.seconds)
            elapsed = time.monotonic() - started
            alive = gui is not None and gui.poll() is None
            if gui is not None and (gui.wait(timeout=180) or not capture.exists()):
                raise RuntimeError("Preview process did not produce the requested frame")
            dest = output / label
            with (dest / "sender.json").open(encoding="utf-8") as stream:
                tx = json.load(stream)["modules"]
            with (dest / "receiver.json").open(encoding="utf-8") as stream:
                rx = json.load(stream)["modules"]
            runs.append(dict(case=label, functional=result["functional_pass"],
                             acceptance=result["strict_acceptance"], runId=result["run_id"],
                             caseWallSeconds=elapsed, guiStillRunningAtCaseEnd=alive,
                             modules={m: dict(built=tx[m]["built"], received=rx[m]["completed"],
                                              consumed=rx[m]["consumed"], skipped=tx[m]["skipped_slots"],
                                              missed=tx[m]["deadline_misses"],
                                              samplingP999us=tx[m]["sampling_deviation"]["p999_us"],
                                              maxPacketGapDeviationUs=tx[m]["packet_gap_deviation"]["max_us"])
                                      for m in ("1","2")}))
        finally:
            if gui is not None and gui.poll() is None:
                gui.terminate()
                gui.wait(timeout=20)
    summary = dict(platform=sys.platform, durationSeconds=args.seconds, gui=str(args.gui.resolve()),
                   sender=str(sender), model=args.model.name, sameHostSeparateProcesses=True, runs=runs)
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return 0 if all(r["functional"] for r in runs) else 1


if __name__ == "__main__":
    raise SystemExit(main())
