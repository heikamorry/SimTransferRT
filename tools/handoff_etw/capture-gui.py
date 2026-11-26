"""Capture a short visible SimTransferRT loopback and its GUI-thread scheduler trace on Windows."""
import argparse
import base64
import ctypes
from ctypes import wintypes as wt
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def json_file(path, obj):
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False), encoding="utf-8")


def ps_literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def encoded(script):
    return base64.b64encode(script.encode('utf-16le')).decode('ascii')


def make_profile(output, tag):
    xml = ET.parse(HERE / "SchedulerMemory.wprp")
    collector = xml.find("./Profiles/SystemCollector")
    collector.set("Name", "SimTransferGui-" + tag)
    target = output / "SchedulerMemory.wprp"
    xml.write(target, encoding="utf-8", xml_declaration=True)
    return target


def collector_script(output, profile, instance, attempt, duration):
    # Only start, stop or cancel the known, private instance. The flag file is a signal only.
    values = {
        "dir": output, "profile": str(profile) + "!SimTransferHandoff.Light",
        "instance": instance, "attempt": attempt,
    }
    binding = "\n".join("$" + k + "=" + ps_literal(v) for k, v in values.items())
    return binding + "\n$limit=" + str(duration + 18) + """
$ErrorActionPreference='Stop'
$wpr=Join-Path ([Environment]::SystemDirectory) 'wpr.exe'
$utf8=New-Object System.Text.UTF8Encoding($false)
$prefix=Join-Path $dir $attempt
$etl=Join-Path $dir 'scheduler.etl'
$staging=Join-Path ([IO.Path]::GetTempPath()) ($instance+'.etl')
function Open-Relay($path) {
    # Truncate an existing caller-owned file; neither create it nor replace its security descriptor.
    return [IO.File]::Open($path, [IO.FileMode]::Truncate, [IO.FileAccess]::Write, [IO.FileShare]::Read)
}
function Write-Relay($path, $value) {
    $bytes=$utf8.GetBytes([string]$value)
    $stream=Open-Relay $path
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}
$started=$false
$saved=$false
try {
    # Native stderr may contain diagnostics on an otherwise successful command.
    $ErrorActionPreference='Continue'
    $startOutput=(& $wpr -start $profile -instancename $instance 2>&1 | Out-String)
    $code=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    $started=($code -eq 0)
    Write-Relay ($prefix+'.start.log') $startOutput
    Write-Relay ($prefix+'.start-code') $code
    Write-Relay ($prefix+'.start-ready') '1'
    if (-not $started) {exit 1}
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $limit -and -not [IO.File]::Exists((Join-Path $dir 'stop.request'))) {
        Start-Sleep -Milliseconds 150
    }
    $ErrorActionPreference='Continue'
    $stopOutput=(& $wpr -stop $staging -skipPdbGen -instancename $instance 2>&1 | Out-String)
    $code=$LASTEXITCODE
    if ($code -eq 0) { $started=$false }
    $ErrorActionPreference='Stop'
    Write-Relay ($prefix+'.stop.log') $stopOutput
    if ($code -ne 0) {exit 2}
    $source=[IO.File]::OpenRead($staging)
    try {
        if ($source.Length -eq 0) {throw 'WPR produced an empty ETL'}
        $destination=Open-Relay $etl
        try { $source.CopyTo($destination) } finally { $destination.Dispose() }
    } finally { $source.Dispose() }
    $saved=$true
} catch {
    Write-Relay ($prefix+'.error.log') $_.Exception.Message
    exit 3
} finally {
    if ($started -and -not $saved) {
        $ErrorActionPreference='Continue'
        $cancelOutput=(& $wpr -cancel -instancename $instance 2>&1 | Out-String)
        Write-Relay ($prefix+'.cancel.log') $cancelOutput
    }
    if ([IO.File]::Exists($staging)) { [IO.File]::Delete($staging) }
}
exit 0
"""


def prepare_relay(output, attempt):
    for suffix in (".start.log", ".start-code", ".start-ready", ".stop.log", ".cancel.log", ".error.log"):
        (output / (attempt + suffix)).touch(exist_ok=False)


def start_collector(output, profile, instance, seconds, elevated):
    attempt = "wpr-elevated" if elevated else "wpr-user"
    prepare_relay(output, attempt)
    powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    payload = encoded(collector_script(output, profile, instance, attempt, seconds))
    if elevated:
        arguments = "-NoLogo -NoProfile -NonInteractive -EncodedCommand " + payload
        # The GUI and this coordinator stay in the ordinary caller's session.
        bootstrap = (
            "$ErrorActionPreference='Stop'\n"
            f"$p=Start-Process -FilePath {ps_literal(powershell)} -ArgumentList {ps_literal(arguments)} "
            "-Verb RunAs -WindowStyle Hidden -PassThru -Wait\nexit $p.ExitCode"
        )
        payload = encoded(bootstrap)
    with (output / (attempt + ".log")).open("wb") as log:
        process = subprocess.Popen(
            [str(powershell), "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand", payload],
            stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    code_file = output / (attempt + ".start-code")
    ready = output / (attempt + ".start-ready")
    deadline = time.monotonic() + (180 if elevated else 30)
    while ready.stat().st_size == 0:
        if process.poll() is not None:
            if ready.stat().st_size:
                break
            raise RuntimeError(f"WPR helper exited before starting. See {attempt}.error.log and {attempt}.log in {output}")
        if time.monotonic() > deadline:
            raise RuntimeError(f"Timed out waiting for WPR startup. See {attempt}.log in {output}")
        time.sleep(.15)
    code = int(code_file.read_text(encoding="utf-8"))
    if code:
        process.wait(timeout=10)
    return process, code, attempt


def privilege_error(code, message):
    common = ("0x80070005", "0x80070522", "0xc0000022", "0xc0000061", "access denied",
              "access is denied", "privilege", "administrator", "elevation", "拒绝访问",
              "访问被拒绝", "特权", "权限", "管理员")
    return code & 0xffffffff in (5, 1314, 0x80070005, 0x80070522, 0xc0000022, 0xc0000061) or any(
        term in message.lower() for term in common)


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD), ("szExeFile", wt.WCHAR * 260),
    ]


def windows_api():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    user = ctypes.WinDLL("user32", use_last_error=True)
    enum_proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    signatures = (
        (kernel.CreateToolhelp32Snapshot, [wt.DWORD, wt.DWORD], wt.HANDLE),
        (kernel.Process32FirstW, [wt.HANDLE, ctypes.POINTER(ProcessEntry)], wt.BOOL),
        (kernel.Process32NextW, [wt.HANDLE, ctypes.POINTER(ProcessEntry)], wt.BOOL),
        (kernel.CloseHandle, [wt.HANDLE], wt.BOOL),
        (kernel.OpenProcess, [wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
        (kernel.TerminateProcess, [wt.HANDLE, wt.UINT], wt.BOOL),
        (user.EnumWindows, [enum_proc, wt.LPARAM], wt.BOOL),
        (user.GetWindowThreadProcessId, [wt.HWND, ctypes.POINTER(wt.DWORD)], wt.DWORD),
        (user.IsWindowVisible, [wt.HWND], wt.BOOL),
        (user.IsIconic, [wt.HWND], wt.BOOL),
        (user.GetWindow, [wt.HWND, wt.UINT], wt.HWND),
        (user.GetForegroundWindow, [], wt.HWND),
        (user.GetWindowTextLengthW, [wt.HWND], ctypes.c_int),
        (user.GetClassNameW, [wt.HWND, wt.LPWSTR, ctypes.c_int], ctypes.c_int),
    )
    for fn, args, result in signatures:
        fn.argtypes = args
        fn.restype = result
    return kernel, user, enum_proc


def find_driver(kernel, parent_pid):
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == wt.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    entry = ProcessEntry()
    entry.dwSize = ctypes.sizeof(entry)
    try:
        ok = kernel.Process32FirstW(handle, ctypes.byref(entry))
        while ok:
            if entry.th32ParentProcessID == parent_pid and entry.szExeFile.casefold() == "simappmemorytests.exe":
                return int(entry.th32ProcessID)
            ok = kernel.Process32NextW(handle, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(handle)
    return None


def main_window(user, enum_proc, pid):
    windows = []

    @enum_proc
    def visit(hwnd, _):
        owner_pid = wt.DWORD()
        tid = user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_pid))
        if owner_pid.value == pid and user.IsWindowVisible(hwnd) and not user.GetWindow(hwnd, 4):
            name = ctypes.create_unicode_buffer(256)
            user.GetClassNameW(hwnd, name, len(name))
            if name.value.startswith("Qt") and user.GetWindowTextLengthW(hwnd) > 0:
                windows.append((int(hwnd), int(tid), name.value))
        return True

    if not user.EnumWindows(visit, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    if len({w[1] for w in windows}) == 1:
        return windows[0]
    return None


def run_gui(build, output, seconds):
    kernel, user, enum_proc = windows_api()
    cmd = [sys.executable, "-B", str(ROOT / "tools/run_release_loopback.py"), "--build", str(build),
           "--seconds", str(seconds), "--visible", "--ndi-handoff-guard-us", "0", "--fragment-trace",
           "--output", str(output / "run")]
    print("Starting visible loopback. Keep the receiver page visible.", flush=True)
    process = subprocess.Popen(cmd)
    pid = None
    driver_handle = None
    identity = None
    samples = []
    deadline = time.monotonic() + seconds + 13
    try:
        while process.poll() is None:
            if time.monotonic() > deadline:
                raise RuntimeError("Loopback exceeded its scheduled capture window.")
            if not pid:
                pid = find_driver(kernel, process.pid)
                if pid:
                    driver_handle = kernel.OpenProcess(1, False, pid)  # PROCESS_TERMINATE for own child
            if pid and identity is None:
                win = main_window(user, enum_proc, pid)
                if win:
                    hwnd, tid, name = win
                    identity = {"pid": pid, "tid": tid, "hwnd": hwnd, "window_class": name,
                                "runner_pid": process.pid, "source": "GetWindowThreadProcessId",
                                "run_dir": str(output / "run")}
                    json_file(output / "gui-thread.json", identity)
                    print(f"GUI thread PID {pid}, TID {tid}.", flush=True)
            if identity:
                owner = wt.DWORD()
                tid = user.GetWindowThreadProcessId(identity["hwnd"], ctypes.byref(owner))
                if tid == identity["tid"] and owner.value == pid:
                    hwnd = identity["hwnd"]
                    samples.append({"utc": datetime.now(timezone.utc).isoformat(),
                                    "visible": bool(user.IsWindowVisible(hwnd)),
                                    "minimized": bool(user.IsIconic(hwnd)),
                                    "foreground": user.GetForegroundWindow() == hwnd})
            time.sleep(1 if identity else .2)
    finally:
        if process.poll() is None:
            process.terminate()
            if driver_handle:
                kernel.TerminateProcess(driver_handle, 1)
            process.wait(timeout=8)
        if driver_handle:
            kernel.CloseHandle(driver_handle)
        json_file(output / "window-samples.json", samples)
    if not identity:
        raise RuntimeError("Could not verify the GUI thread from the loopback driver's visible Qt window.")
    if process.returncode not in (0, 2):
        raise RuntimeError(f"Loopback driver failed, exit {process.returncode}. See run/driver.log.")
    report = json.loads((output / "run/joint.json").read_text(encoding="utf-8"))
    fragment = report["sender"]["fragment_trace"]
    if not fragment.get("enabled") or int(fragment.get("process_id", 0)) != pid:
        raise RuntimeError("The traced GUI PID does not match the joint report.")
    if not report["joint_scope"].get("eligible") or any(
            report["receiver"]["modules"][n]["consumed"] <= 0 for n in ("1", "2")):
        raise RuntimeError("Real paired loopback delivery was not confirmed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/jitter-gui-20261126/Release")
    parser.add_argument("--output", type=Path, help="new capture directory (default: unique directory near build)")
    parser.add_argument("--seconds", type=int, default=30, choices=range(30, 46), metavar="30..45")
    args = parser.parse_args()
    build = args.build.resolve()
    if os.name != "nt" or not (build / "SimAppMemoryTests.exe").is_file():
        parser.error("requires Windows and a Release build containing SimAppMemoryTests.exe")
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="gui-etw-", dir=build.parent))
    tag = uuid.uuid4().hex
    profile = make_profile(output, tag)
    instance = "SimTransferGui-" + tag
    (output / "scheduler.etl").touch(exist_ok=False)
    print(f"Capture directory: {output}", flush=True)
    helper = None
    active = False
    try:
        helper, code, attempt = start_collector(output, profile, instance, args.seconds, False)
        if code:
            message = (output / (attempt + ".start.log")).read_text(encoding="utf-8")
            if not privilege_error(code, message):
                raise RuntimeError(f"WPR could not start the private session. See {attempt}.start.log.")
            print("Kernel scheduling capture needs tracing privileges. Approve the Windows UAC "
                  "prompt for the collector. The GUI will use the original session.", flush=True)
            helper, code, attempt = start_collector(output, profile, instance, args.seconds, True)
        if code:
            raise RuntimeError(f"WPR start failed. See {attempt}.start.log in {output}")
        active = True
        run_gui(build, output, args.seconds)
    finally:
        (output / "stop.request").touch()
        if active:
            print("Saving scheduler.etl.", flush=True)
            ret = helper.wait(timeout=120)
            etl = output / "scheduler.etl"
            if ret or not etl.is_file() or not etl.stat().st_size:
                raise RuntimeError(f"WPR failed to save scheduler.etl. See {attempt}.stop.log and {attempt}.error.log in {output}.")
            with etl.open("rb") as stream:
                if not stream.read(1):
                    raise RuntimeError(f"Scheduler trace is empty: {etl}")
    (output / "capture.ready").touch(exist_ok=False)
    print(f"Finished capturing real GUI and ETW evidence in {output}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"Capture failed: {error}", file=sys.stderr)
        raise SystemExit(1)
