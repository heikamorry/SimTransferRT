"""Run the Release GUI sender and receiver on IPv4 loopback and retain their joint report."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

from run_diagnostics import cpu_seconds
from runtime_support import atomic_json, probe_udp_loopback


def compare_reports(baseline, current, baseline_visible, current_visible,
                    baseline_driver_sha256=None, current_driver_sha256=None):
    """Compare measured rows from paired GUI runs, retaining the original verdicts."""
    def summarize(report, visible):
        sender = report.get("sender", {})
        receiver = report.get("receiver", {})
        config = sender.get("config", {})
        seconds = report.get("test_duration_seconds")
        workload = {key: config.get(key) for key in (
            "duration_us", "frame_limit", "ndi_enabled", "ndi_hz", "ndi_tools", "ep_enabled",
            "ep_hz", "ep_bytes", "effective_link_mbps", "fault_injection", "status_interval_us",
            "mailbox_fast_path", "network_profile", "path_limit_mbps", "allow_oversubscribed")}
        workload.update(seconds=seconds, visible=visible, transport=sender.get("transport"),
                        send_api=sender.get("send_api"),
                        fragment_trace=sender.get("fragment_trace", {}).get("enabled"),
                        handoff_trace=sender.get("handoff_trace", {}).get("enabled"))
        reasons = []
        if (report.get("joint_scope", {}).get("eligible") is not True
                or config.get("target_ip") != "127.0.0.1"):
            reasons.append("missing paired 127.0.0.1 session")
        if (type(seconds) is not int or seconds <= 0 or workload["duration_us"] != seconds * 1000000
                or sender.get("finished") is not True or receiver.get("final") is not True):
            reasons.append("missing completed timed run")
        ui = {m["module_id"]: m for m in receiver.get("ui_observations", {}).get("modules", [])}
        distributions = {(0, "global_packet_gap_deviation"):
                         sender.get("global_packet_gap_deviation", {})}
        for module, name in ((1, "ndi"), (2, "ep")):
            tx = sender.get("modules", {}).get(str(module), {})
            rx = receiver.get("modules", {}).get(str(module), {})
            workload[name + "_source"] = tx.get("source_kind")
            hz = workload[name + "_hz"]
            if (workload[name + "_enabled"] is not True or type(hz) is not int or hz <= 0
                    or type(seconds) is not int or tx.get("built") != seconds * hz):
                reasons.append(name + ": incomplete or unknown frame population")
            if not all(type(rx.get(k)) is int and rx[k] > 0 for k in ("datagrams", "consumed")):
                reasons.append(name + ": no confirmed receive and consume")
            distributions[module, "packet_gap_jitter"] = tx.get("packet_gap_deviation", {})
            distributions[module, "sampling_jitter"] = tx.get("sampling_deviation", {})
            distributions[module, "initial_submission"] = tx.get("initial_duration", {})
        if None in workload.values():
            reasons.append("missing workload settings")
        metrics = {}
        for row in report.get("acceptance", []):
            module, metric = row["module_id"], row["id"]
            distribution = distributions.get((module, metric))
            measured = row.get("measured") if row.get("status") in ("pass", "fail") else None
            detail = {"status": row.get("status"), "measured": measured,
                      "threshold": row.get("threshold")}
            if distribution is not None or metric == "gui_widget_updates":
                if metric == "gui_widget_updates":
                    source = ui.get(module, {})
                    fields = ("max_complete_to_widget_update_us", "updates_after_one_module_period",
                              "widget_updated_frames")
                else:
                    source = distribution
                    fields = ("max_us", "violations", "count")
                count = source.get(fields[-1])
                observed = measured is not None and type(count) is int and count > 0
                detail.update({k: source.get(f) if observed else None for k, f in
                               zip(("max_us", "violations", "observations"), fields)})
            metrics[f"{module}.{metric}"] = detail
        return workload, reasons, metrics

    left = summarize(baseline, baseline_visible)
    right = summarize(current, current_visible)
    reasons = ["baseline: " + reason for reason in left[1]]
    reasons.extend("current: " + reason for reason in right[1])
    reasons.extend("changed workload: " + k for k in left[0] if left[0][k] != right[0][k])
    controls = {
        name: {side: report.get("sender", {}).get("config", {}).get(name)
               for side, report in (("baseline", baseline), ("current", current))}
        for name in ("ndi_handoff_guard_us", "asio_pooled_handler")
    }
    changed = [name for name, values in controls.items() if values["baseline"] != values["current"]]
    if len(changed) > 1:
        reasons.append("multiple sender controls changed: " + ", ".join(changed))
    if "asio_pooled_handler" in changed:
        if any(None in values.values() for values in controls.values()):
            reasons.append("handler pool comparison requires both sender controls in each report")
        if not baseline_driver_sha256 or baseline_driver_sha256 != current_driver_sha256:
            reasons.append("handler pool comparison requires the same GUI executable")
    for side, report in (("baseline", baseline), ("current", current)):
        sender = report.get("sender", {})
        requested = controls["asio_pooled_handler"][side]
        storage = sender.get("operation_storage", {})
        if requested is not None and storage.get("enabled") is not requested:
            reasons.append(side + ": configured and active handler pool settings do not match")
        if requested is True and (type(storage.get("hits")) is not int or storage["hits"] < 1):
            reasons.append(side + ": no handler pool allocations observed")
    rows = {}
    lost_passes = []
    higher_maxima = []
    for name in sorted(left[2].keys() | right[2].keys()):
        before, after = left[2].get(name), right[2].get(name)
        row = {"baseline": before, "current": after}
        if (not reasons and before and after and before["threshold"] == after["threshold"]
                and before.get("max_us") is not None and after.get("max_us") is not None):
            row["max_change_us"] = after["max_us"] - before["max_us"]
            if type(before["violations"]) is int and type(after["violations"]) is int:
                row["violations_change"] = after["violations"] - before["violations"]
            if before["status"] == "pass" and row["max_change_us"] > 0:
                higher_maxima.append(name)
        rows[name] = row
        if before and before["status"] == "pass" and (
                not after or after["status"] != "pass" or before["threshold"] != after["threshold"]):
            lost_passes.append(name)
    return {"comparable_workload": not reasons, "reasons": reasons,
            "previous_passes_no_longer_confirmed": None if reasons else lost_passes,
            "previous_passes_with_higher_max_us": None if reasons else higher_maxima,
            **controls, "changed_sender_controls": changed,
            "driver_sha256": {"baseline": baseline_driver_sha256, "current": current_driver_sha256},
            "operation_storage": {
                side: report.get("sender", {}).get("operation_storage")
                for side, report in (("baseline", baseline), ("current", current))},
            "baseline_workload": left[0], "current_workload": right[0], "rows": rows}


def run_diagnostic_first(args, build, output):
    common = [sys.executable, "-B", str(Path(__file__).resolve()), "--build", str(build)]
    if args.visible:
        common.append("--visible")
    if args.asio_pooled_handler:
        common.append("--asio-pooled-handler")
    if args.ndi_handoff_guard_us is not None:
        common.extend(["--ndi-handoff-guard-us", str(args.ndi_handoff_guard_us)])
    diagnostic = output / "diagnostic"
    command = common + ["--output", str(diagnostic), "--seconds", str(args.diagnostic_first),
                        "--fragment-trace"]
    if args.basic_tests:
        command.append("--basic-tests")
    print(f"Starting traced diagnostic. Results: {diagnostic}", flush=True)
    code = subprocess.run(command).returncode
    try:
        if args.basic_tests:
            basic = json.loads((diagnostic / "basic-tests.json").read_text(encoding="utf-8"))
            if basic.get("exit_code") != 0:
                print(f"Formal run skipped: basic tests failed. See {diagnostic / 'ctest.log'}")
                return code or 1
        run_path = diagnostic / "run.json"
        if run_path.is_file() and json.loads(run_path.read_text(encoding="utf-8")).get("timed_out"):
            print(f"Formal run skipped: the diagnostic timed out. See {diagnostic / 'driver.log'}")
            return 1
        if code not in (0, 2):
            print(f"Formal run skipped: diagnostic exited {code}. See {diagnostic}")
            return code
        data = json.loads((diagnostic / "joint.json").read_text(encoding="utf-8"))
        modules = data.get("receiver", {}).get("modules", {})
        if (data.get("joint_scope", {}).get("eligible") is not True
                or data.get("sender", {}).get("config", {}).get("target_ip") != "127.0.0.1"
                or not all(type(modules.get(m, {}).get("datagrams")) is int
                           and modules[m]["datagrams"] > 0 for m in ("1", "2"))):
            print(f"Formal run skipped: paired NDI and EP loopback reception was not confirmed. "
                  f"See {diagnostic / 'joint.json'}")
            return 3
    except (OSError, ValueError, AttributeError) as error:
        print(f"Formal run skipped: missing or unreadable diagnostic evidence: {error}")
        return 1
    formal = output / "formal"
    command = common + ["--output", str(formal), "--seconds", str(args.seconds)]
    if args.compare_to:
        command.extend(["--compare-to", str(args.compare_to.resolve())])
    print(f"Diagnostic reception confirmed. Starting untraced formal run. Results: {formal}",
          flush=True)
    return subprocess.run(command).returncode


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=root / "build/windows-release")
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--diagnostic-first", type=int, metavar="SECONDS",
                        help="run a traced diagnostic first, then the untraced formal interval; "
                             "--compare-to applies to the formal run")
    parser.add_argument("--ndi-handoff-guard-us", type=int, metavar="0..400",
                        help="override the NDI sender handoff lead for this measured run")
    parser.add_argument("--fragment-trace", action="store_true",
                        help="capture sender fragment timing for a diagnostic run")
    parser.add_argument("--asio-pooled-handler", action="store_true",
                        help="enable the experimental Asio sender handler storage pool")
    parser.add_argument("--output", type=Path, help="new directory for this run")
    parser.add_argument("--visible", action="store_true", help="show the test application window")
    parser.add_argument("--basic-tests", action="store_true",
                        help="run CTest before starting the measured loopback interval")
    parser.add_argument("--probe-only", action="store_true",
                        help="check IPv4 UDP in this process; exit 0 if bidirectional, 3 if unavailable")
    parser.add_argument("--compare-to", type=Path, metavar="JOINT_JSON",
                        help="compare the completed GUI run with a previous paired loopback report")
    args = parser.parse_args()
    if args.diagnostic_first is not None and (not 1 <= args.diagnostic_first <= 3600
                                              or args.fragment_trace or args.probe_only):
        parser.error("--diagnostic-first requires 1..3600 seconds and controls fragment tracing "
                     "for the paired GUI runs")
    if args.ndi_handoff_guard_us is not None and (
            args.probe_only or not 0 <= args.ndi_handoff_guard_us <= 400):
        parser.error("--ndi-handoff-guard-us requires a measured run and a value of 0..400")
    if os.name != "nt" or not 1 <= args.seconds <= 3600:
        parser.error("requires Windows and a duration of 1..3600 seconds")
    build = args.build.resolve()
    driver = build / "SimAppMemoryTests.exe"
    if args.probe_only and (args.basic_tests or args.compare_to or args.fragment_trace
                            or args.asio_pooled_handler):
        parser.error("--probe-only cannot use GUI run options")
    if args.compare_to:
        try:
            baseline = json.loads(args.compare_to.read_text(encoding="utf-8"))
            if not isinstance(baseline, dict) or not isinstance(baseline.get("acceptance"), list):
                parser.error("--compare-to requires a joint.json GUI report")
            baseline_manifest = args.compare_to.resolve().with_name("run.json")
            baseline_run = (json.loads(baseline_manifest.read_text(encoding="utf-8"))
                            if baseline_manifest.is_file() else {})
        except (OSError, ValueError) as exc:
            parser.error(f"unable to read baseline report: {exc}")
    if not args.probe_only and not driver.is_file():
        parser.error(f"build the Release target SimAppMemoryTests in {build}")
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        build.parent.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="loopback-", dir=build.parent))
    if args.diagnostic_first is not None:
        return run_diagnostic_first(args, build, output)
    if args.probe_only:
        probe = probe_udp_loopback()
        probe["utc"] = datetime.now(timezone.utc).isoformat()
        atomic_json(output / "udp-probe.json", probe)
        print(json.dumps(probe, indent=2))
        print(f"Probe report: {output / 'udp-probe.json'}")
        return 0 if probe["available"] else 3
    if args.basic_tests:
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        ctest = next(line.split("=", 1)[1] for line in cache.splitlines()
                     if line.startswith("CMAKE_CTEST_COMMAND:"))
        print(f"Running basic Release tests. Log: {output / 'ctest.log'}", flush=True)
        basic = subprocess.run([ctest, "--test-dir", str(build), "-C", "Release", "-j", "1",
                                "--no-tests=error", "--output-on-failure",
                                "--output-log", str(output / "ctest.log")], timeout=300)
        (output / "basic-tests.json").write_text(
            json.dumps({"exit_code": basic.returncode}, indent=2), encoding="utf-8")
        if basic.returncode:
            print("Basic tests failed. See ctest.log before running the long test.", flush=True)
            return basic.returncode
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    report = output / "joint.json"
    command = [str(driver), "soak", str(port), str(report), str(args.seconds), "1"]
    if args.ndi_handoff_guard_us is not None:
        command.extend(["--ndi-handoff-guard-us", str(args.ndi_handoff_guard_us)])
    if args.fragment_trace:
        command.append("--fragment-trace")
    if args.asio_pooled_handler:
        command.append("--asio-pooled-handler")
    artifacts = {}
    for name in ("SimTransfer.exe", "SimTransferCli.exe", driver.name):
        with (build / name).open("rb") as stream:
            artifacts[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = {
        "started_utc": datetime.now(timezone.utc).isoformat(), "seconds": args.seconds,
        "endpoint": "127.0.0.1", "port": port, "sha256": artifacts, "command": command,
        "visible": args.visible, "fragment_trace": args.fragment_trace,
        "asio_pooled_handler": args.asio_pooled_handler,
        "ndi_handoff_guard_us": args.ndi_handoff_guard_us,
        "measurement_mode": "fragment_trace_diagnostic" if args.fragment_trace else "untraced",
    }
    (output / "run.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"IPv4 UDP loopback for {args.seconds} seconds. Results: {output}", flush=True)
    startup = subprocess.STARTUPINFO()
    if not args.visible:
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
    started = time.monotonic()
    with (output / "driver.log").open("wb") as log:
        with subprocess.Popen(command, cwd=build, stdout=log, stderr=subprocess.STDOUT,
                              startupinfo=startup,
                              env={**os.environ, "QT_QPA_PLATFORM": "windows"}) as process:
            deadline = started + args.seconds + 60
            timed_out = False
            while process.poll() is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    timed_out = True
                    process.kill()
                    process.wait()
                    break
                try:
                    process.wait(timeout=min(30, remaining))
                except subprocess.TimeoutExpired:
                    elapsed = int(time.monotonic() - started)
                    print(f"Running {elapsed}s / {args.seconds}s; "
                          f"process {process.pid}; awaiting final report", flush=True)
            exit_code = process.returncode
            driver_cpu_seconds = cpu_seconds(process)
    manifest.update({
        "finished_utc": datetime.now(timezone.utc).isoformat(),
        "wall_seconds": round(time.monotonic() - started, 3),
        "exit_code": exit_code, "timed_out": timed_out,
        "driver_cpu_seconds": driver_cpu_seconds,
    })
    (output / "run.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if timed_out:
        print(f"Timed out after {args.seconds + 60}s. See {output / 'driver.log'}.")
        return 1
    if not report.is_file():
        print(f"No joint report. See {log.name}.")
        return exit_code or 1
    data = json.loads(report.read_text(encoding="utf-8"))
    sender = data.get("sender", {})
    actual_trace = sender.get("fragment_trace", {}).get("enabled")
    actual_pool = sender.get("operation_storage", {}).get("enabled")
    if (actual_trace is not args.fragment_trace or
            (args.asio_pooled_handler or actual_pool is not None) and actual_pool is not args.asio_pooled_handler):
        print(f"Requested sender options were not confirmed: fragment_trace={actual_trace}, "
              f"asio_pooled_handler={actual_pool}. Report: {report}")
        return 1
    if args.fragment_trace:
        print("Diagnostic run with fragment timing enabled.")
    if args.compare_to:
        comparison = compare_reports(baseline, data, baseline_run.get("visible"), args.visible,
                                     baseline_run.get("sha256", {}).get(driver.name), artifacts[driver.name])
        comparison.update(baseline_report=str(args.compare_to.resolve()), current_report=str(report))
        atomic_json(output / "comparison.json", comparison)
        print(f"Comparison: {output / 'comparison.json'}")
        for reason in comparison["reasons"]:
            print(f"Comparison unavailable: {reason}")
    for module, name in (("1", "NDI"), ("2", "EP")):
        tx = data["sender"]["modules"][module]
        rx = data["receiver"]["modules"][module]
        print(f"{name}: built={tx['built']} initial={tx['initial_complete']} "
              f"completed={rx['completed']} consumed={rx['consumed']}")
    for row in data["acceptance"]:
        if row["status"] == "fail":
            print(f"FAIL module={row['module_id']} {row['id']}: {row['measured']}")
    print(f"loopback_passed={data['loopback_passed']}")
    return exit_code or (0 if data["loopback_passed"] else 2)


if __name__ == "__main__":
    raise SystemExit(main())
