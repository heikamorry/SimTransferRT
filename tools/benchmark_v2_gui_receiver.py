"""Compare isolated Windows Release CLI + GUI receiver runs at fixed loopback pacing."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess

from run_diagnostics import free_ports, write_json
from runtime_support import probe_udp_loopback


def verify_loopback():
    result = probe_udp_loopback()
    if not result["available"]:
        raise RuntimeError(f"IPv4 UDP loopback unavailable for benchmark: {json.dumps(result)}")


def run_rate(binaries, sender_cli, output, rate, startup, legacy_driver):
    run_dir = output / f"{rate}Mbps"
    run_dir.mkdir()
    port, command_port = free_ports()
    receiver_file = run_dir / "receiver.json"
    sender_file = run_dir / "sender.json"
    env = {**os.environ, "QT_QPA_PLATFORM": "windows"}
    receiver = [binaries / "SimAppMemoryTests.exe", "receiver", str(port), receiver_file, "1", "1"]
    if not legacy_driver:
        receiver.append("--include-receiver-report")
    sender = [sender_cli, "--transport", "asio-async", "--bind", "127.0.0.1",
              "--target", "127.0.0.1", "--port", str(port), "--command-port", str(command_port),
              "--allowed-nack-port", str(port), "--network-profile", "1g", "--path-limit-mbps",
              str(rate), "--ndi-tools", "4", "--ndi-hz", "60", "--ep-bytes", "204800",
              "--ep-hz", "50", "--duration", "24", "--report", sender_file]
    with (run_dir / "receiver-stderr.log").open("w") as err, ThreadPoolExecutor(1) as pool:
        with subprocess.Popen(receiver, cwd=run_dir, env=env, stdout=subprocess.PIPE,
                              stderr=err, text=True, startupinfo=startup) as child:
            try:
                ready = pool.submit(child.stdout.readline).result(timeout=35)
                if ready.strip() != "receiver-ready":
                    raise RuntimeError(f"GUI receiver failed before readiness; see {err.name}")
                with (run_dir / "sender-stderr.log").open("w") as tx_err:
                    subprocess.run(sender, check=True, timeout=55, cwd=run_dir, env=env,
                                   stdout=subprocess.DEVNULL, stderr=tx_err, startupinfo=startup)
                if child.wait(timeout=25) != 0:
                    raise RuntimeError(f"GUI receiver failed; see {err.name}")
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
    tx = json.loads(sender_file.read_text(encoding="utf-8"))
    rx = json.loads(receiver_file.read_text(encoding="utf-8"))
    counts = rx["firstStop"]["received"]
    modules = {}
    for module_id, name in (("1", "ndi"), ("2", "ep")):
        m = tx["modules"][module_id]
        modules[name] = {field: m[field] for field in
                         ("built", "initial_complete", "datagrams", "retransmissions", "nacks",
                          "skipped_slots", "deadline_misses", "send_errors")}
        modules[name]["consumed"] = counts[name]
        if not m["initial_complete"] or not counts[name]:
            raise RuntimeError(f"No paired {name} frames in {run_dir}")
        modules[name]["initial_max_us"] = m["initial_duration"]["max_us"]
        modules[name]["initial_p99_us"] = m["initial_duration"]["p99_us"]
        if not legacy_driver:
            rx_module = rx["receiver"]["modules"][module_id]
            if not rx["receiver"]["final"] or rx_module["consumed"] != counts[name]:
                raise RuntimeError(f"Inconsistent final receiver snapshot in {receiver_file}")
            modules[name]["receiver"] = {key: rx_module[key] for key in
                                         ("completed", "consumed", "expired", "late", "gapTriggers",
                                          "tailTriggers", "requestedFragments", "retries")}
    if tx["config"]["effective_link_mbps"] != rate or not tx["finished"] or tx["error"]:
        raise RuntimeError(f"Unexpected sender config or run status in {sender_file}")
    return {"path_limit_mbps": rate, "elapsed_us": tx["elapsed_us"], "modules": modules}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin", type=Path, help="Release directory with SimAppMemoryTests and SimTransferCli")
    parser.add_argument("output", type=Path, help="new output directory for per-run reports")
    parser.add_argument("--sender-cli", type=Path, help="use a specific installed SimTransferCli.exe")
    parser.add_argument("--rates", type=int, nargs="+", default=[200, 300])
    parser.add_argument("--legacy-driver", action="store_true",
                        help="0.3.0 receiver driver exports only consumed counts")
    args = parser.parse_args()
    if os.name != "nt" or any(rate < 100 or rate > 1000 for rate in args.rates):
        parser.error("requires Windows and path limits between 100 and 1000 Mbps")
    if len(set(args.rates)) != len(args.rates):
        parser.error("rate list must be unique")
    binaries = args.bin.resolve()
    sender_cli = args.sender_cli.resolve() if args.sender_cli else binaries / "SimTransferCli.exe"
    output = args.output.resolve()
    if output.exists():
        parser.error(f"output already exists: {output}")
    hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (binaries / "SimAppMemoryTests.exe", sender_cli)}
    verify_loopback()
    output.mkdir(parents=True, exist_ok=False)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    result = {"duration_seconds": 24, "endpoint": "127.0.0.1", "transport": "asio-async",
              "binary_sha256": hashes, "runs": []}
    for rate in args.rates:
        run = run_rate(binaries, sender_cli, output, rate, startup, args.legacy_driver)
        result["runs"].append(run)
        write_json(output / "comparison.json", result)
        print(json.dumps(run), flush=True)


if __name__ == "__main__":
    main()
