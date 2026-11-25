"""Measure the native Windows Release GUI at rest and under loopback load."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_diagnostics import free_ports, memory_bytes, write_json


def stop_child(process):
    if process.poll() is None:
        process.kill()
        process.wait()


def idle_launch(gui, options):
    start = time.monotonic()
    with subprocess.Popen([gui], **options) as child:
        try:
            samples = []
            while (elapsed := time.monotonic() - start) < 25:
                if child.poll() is not None:
                    raise RuntimeError(f"GUI exited early with code {child.returncode}")
                sample = memory_bytes(child)
                if not sample["available"]:
                    raise RuntimeError(sample["error"])
                samples.append((elapsed, sample))
                time.sleep(0.05)
            stable = [s for t, s in samples if t >= 5]

            def span(key):
                values = [s[key] for s in stable]
                return {"min": min(values), "median": statistics.median(values), "max": max(values)}

            # Send the normal window-close request only to this launch.
            command = f"(Get-Process -Id {child.pid}).CloseMainWindow()"
            close = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                                    command], capture_output=True, text=True, check=True, timeout=10)
            if close.stdout.strip().lower() != "true" or child.wait(timeout=15) != 0:
                raise RuntimeError("GUI failed to close normally")
            return {"pid": child.pid, "samples": len(samples), "settledSamples": len(stable),
                    "idleWorkingSetBytes": span("working_set_bytes"),
                    "idlePrivateBytes": span("private_bytes"),
                    "sampledPeakWorkingSetBytes": max(s["working_set_bytes"] for _, s in samples),
                    "sampledPeakPrivateBytes": max(s["private_bytes"] for _, s in samples),
                    "osPeakWorkingSetBytes": samples[-1][1]["os_peak_working_set_bytes"],
                    "exitCode": child.returncode}
        finally:
            stop_child(child)


def model_first_frame(gui, model, options):
    digest = hashlib.sha256()
    with model.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    capture = Path(options["cwd"]) / "model.png"
    start = time.monotonic()
    peak_ws = 0
    peak_private = 0
    samples = 0
    with subprocess.Popen([gui, "--model", model, "--screenshot", capture], **options) as child:
        try:
            while child.poll() is None:
                if time.monotonic() - start > 120:
                    raise TimeoutError("model did not render within 120 seconds")
                sample = memory_bytes(child)
                if not sample["available"]:
                    if child.poll() is not None:
                        break
                    raise RuntimeError(sample["error"])
                peak_ws = max(peak_ws, sample["working_set_bytes"])
                peak_private = max(peak_private, sample["private_bytes"])
                samples += 1
                time.sleep(0.02)
            elapsed = time.monotonic() - start
            if child.returncode != 0 or not samples:
                raise RuntimeError("model launch failed")
            with capture.open("rb") as image:
                if image.read(8) != b"\x89PNG\r\n\x1a\n":
                    raise RuntimeError("model capture missing")
            return {"executable": gui.name, "pid": child.pid, "file": model.name,
                    "sourceBytes": model.stat().st_size, "sha256": digest.hexdigest(),
                    "targetSampleIntervalMs": 20, "samples": samples, "elapsedSeconds": elapsed,
                    "sampledPeakWorkingSetBytes": peak_ws, "sampledPeakPrivateBytes": peak_private,
                    "screenshotBytes": capture.stat().st_size, "exitCode": child.returncode}
        finally:
            stop_child(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin", type=Path, help="Release directory containing both executables")
    parser.add_argument("output", type=Path, help="new directory for this recording")
    parser.add_argument("--case", choices=("all", "startup", "paired", "joint", "model"), default="all")
    parser.add_argument("--model", type=Path, help="also measure opening this model in the shipped GUI")
    parser.add_argument("--joint-blocks", type=int, default=12, help="number of 10-second windows")
    parser.add_argument("--cycles", type=int, default=12, help="4-second runs after the long run")
    args = parser.parse_args()
    if os.name != "nt" or not 1 <= args.joint_blocks <= 360 or not 1 <= args.cycles <= 100:
        parser.error("requires Windows, 1–360 blocks and 1–100 cycles")
    if (args.case == "model" and not args.model) or (args.model and not args.model.is_file()):
        parser.error("specify an existing --model file")
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    binaries = args.bin.resolve()
    gui = binaries / "SimTransfer.exe"
    with tempfile.TemporaryDirectory() as work:
        options = {"cwd": work, "env": {**os.environ, "QT_QPA_PLATFORM": "windows"}}

        def driver(role, port):
            return [binaries / "SimAppMemoryTests.exe", role, str(port), dest / f"{role}.json",
                    str(args.joint_blocks), str(args.cycles)]

        if args.case in ("all", "startup"):
            launches = [idle_launch(gui, options) for _ in range(3)]
            write_json(dest / "startup.json", {"executable": gui.name, "launches": launches,
                       "timeToSampleSeconds": 25, "settledFromSeconds": 5, "targetSampleIntervalMs": 50})
        if args.case in ("all", "paired"):
            port, _ = free_ports()
            with ThreadPoolExecutor(1) as pool, subprocess.Popen(
                    driver("receiver", port), stdout=subprocess.PIPE, text=True, **options) as rx:
                try:
                    ready = pool.submit(rx.stdout.readline).result(timeout=30)
                    if ready.strip() != "receiver-ready":
                        raise RuntimeError("receiver did not publish readiness")
                    subprocess.run(driver("sender", port), **options, check=True, timeout=55)
                    if rx.wait(timeout=25) != 0:
                        raise RuntimeError("receiver test failed")
                finally:
                    stop_child(rx)
        if args.case in ("all", "joint"):
            port, _ = free_ports()
            timeout = 90 + 12 * args.joint_blocks + 15 * args.cycles
            subprocess.run(driver("joint", port), **options, check=True, timeout=timeout)
        if args.case in ("all", "model") and args.model:
            write_json(dest / "model.json", model_first_frame(gui, args.model.resolve(), options))


if __name__ == "__main__":
    main()
