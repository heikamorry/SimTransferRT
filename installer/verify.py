"""Exercise the Windows installer in an isolated folder. Machine mode requires elevation."""
import argparse
import codecs
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import winreg

from package import ROOT, build_installer, guid, sha256
from verify_maintenance import install_through_wizard, maintain_through_wizard

msi_api = ctypes.WinDLL("msi")
msi_api.MsiQueryProductStateW.argtypes = [wintypes.LPCWSTR]
msi_api.MsiEnumRelatedProductsW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPWSTR]


def check(ok, reason):
    if not ok:
        raise RuntimeError(reason)


def related(family):
    code = ctypes.create_unicode_buffer(39)
    found = set()
    while True:
        result = msi_api.MsiEnumRelatedProductsW("{" + guid(family + ":msi-upgrade") + "}", 0, len(found), code)
        if result == 259:
            return found
        check(result == 0 and code.value not in found, f"cannot enumerate product registrations ({result})")
        found.add(code.value.upper())


def registry_value(hive, key, name, view=winreg.KEY_WOW64_64KEY):
    try:
        with winreg.OpenKey(hive, key, access=winreg.KEY_READ | view) as handle:
            return winreg.QueryValueEx(handle, name)[0]
    except FileNotFoundError:
        return None


def application_entries(family):
    subkey = r"Software\Microsoft\Windows\CurrentVersion\Uninstall"
    entries = []
    seen = set()
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
            try:
                parent = winreg.OpenKey(hive, subkey, access=winreg.KEY_READ | view)
            except FileNotFoundError:
                continue
            with parent:
                for index in range(winreg.QueryInfoKey(parent)[0]):
                    key = subkey + "\\" + winreg.EnumKey(parent, index)
                    if (hive, key) in seen or registry_value(hive, key, "DisplayName", view) != family:
                        continue
                    if registry_value(hive, key, "SystemComponent", view) == 1:
                        continue
                    seen.add((hive, key))
                    entries.append({"version": registry_value(hive, key, "DisplayVersion", view),
                                    "cache": registry_value(hive, key, "BundleCachePath", view)})
    return entries


def shell_folder(csidl):
    buf = ctypes.create_unicode_buffer(260)
    check(ctypes.windll.shell32.SHGetFolderPathW(None, csidl, None, 0, buf) == 0, "shell folder unavailable")
    return Path(buf.value)


def global_paths():
    return (registry_value(winreg.HKEY_CURRENT_USER, "Environment", "Path"),
            registry_value(winreg.HKEY_LOCAL_MACHINE,
                           r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment", "Path"))


def check_rollback_log_permissions(work):
    for path in work.glob("*.log"):
        raw = path.read_bytes()
        encoding = "utf-16" if raw.startswith(codecs.BOM_UTF16_LE) else "utf-8-sig"
        report = raw.decode(encoding, errors="replace")
        check("Setting File Security failed" not in report and "Note: 1: 1926 " not in report,
              f"Windows Installer reported a rollback-file permission error in {path.name}")


def command(args, *, expected=True, timeout=210, env=None, cwd=None):
    invocation = [str(a) for a in args]
    if Path(args[0]).name.lower() == "msiexec.exe":
        # msiexec parses NAME="value with spaces" itself, before splitting arguments.
        def msi_arg(value):
            key, sep, text = value.partition("=")
            if sep and key.isidentifier():
                return key + '="' + text.replace('"', '""') + '"'
            return subprocess.list2cmdline([value])
        invocation = " ".join(msi_arg(a) for a in invocation)
    p = subprocess.run(invocation, timeout=timeout, env=env, cwd=cwd,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (p.returncode == 0) != expected:
        raise RuntimeError(f"{Path(args[0]).name} returned {p.returncode}. "
                           + p.stdout.decode(errors="replace")[-1200:])
    return p.returncode


def check_network(root, work, env):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        port = str(s.getsockname()[1])
    ready = work / "receiver.ready"
    rx_report = work / "rx.json"
    tx_report = work / "tx.json"
    peer = subprocess.Popen([sys.executable, ROOT / "tools/v2_receiver.py", "--port", port,
                             "--duration", "4", "--report", rx_report, "--ready-file", ready],
                            cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 10
        while not ready.exists():
            check(time.monotonic() < deadline and peer.poll() is None, "UDP peer did not start")
            time.sleep(.05)
        command([root / "bin/SimTransferCli.exe", "--protocol", "v2", "--transport", "asio-async",
                 "--bind", "127.0.0.1", "--target", "127.0.0.1", "--port", port, "--command-port", "0",
                 "--duration", "1", "--report", tx_report], env=env, cwd=work)
        check(peer.wait(timeout=12) == 0, "UDP peer failed")
        tx = json.loads(tx_report.read_bytes())["modules"]
        rx = json.loads(rx_report.read_bytes())["modules"]
        sent = [tx[k]["initial_complete"] for k in ("1", "2")]
        received = [rx[k]["completed"] for k in ("1", "2")]
        check(all(received) and sent == received, f"loopback frames {sent} -> {received}")
        return {"ndi": received[0], "ep": received[1]}
    finally:
        if peer.poll() is None:
            peer.kill()
            peer.wait()


def exercise(release, upgrade, work, machine):
    brand = release.get("family", "SimTransferRT")
    original = Path(release["setup"])
    stage = Path(release["stage"])
    check(not related(brand) and not application_entries(brand), "an existing installation must be left alone")
    check(not machine or ctypes.windll.shell32.IsUserAnAdmin(), "machine scope requires administrator consent")
    if release.get("sha256"):
        check(sha256(original) == release["sha256"], "the EXE differs from the recorded release")
    # A per-user MSI on a secondary volume can produce Windows rollback-ACL
    # warnings when the ordinary account cannot update that volume's Config.Msi.
    # Test the actual per-machine product on the requested volume. Use the
    # user's own application-data volume for the isolated per-user fixture.
    base = work if machine else Path(tempfile.mkdtemp(prefix=brand + "-", dir=os.environ["LOCALAPPDATA"]))
    install_dir = base / "医疗 软件" / brand
    install_dir.mkdir(parents=True, exist_ok=False)
    data = install_dir / "自有模型.ply"
    data.write_bytes(b"preserve user data\n")
    paths_before = global_paths()
    desktop = shell_folder(25 if machine else 16) / (brand + ".lnk")
    menu = shell_folder(23 if machine else 2) / brand
    main_link = menu / (brand + ".lnk")
    cli_link = menu / (brand + " 命令行.lnk")
    check(not desktop.exists() and not menu.exists(), "pre-existing shortcuts must be left alone")
    hive = winreg.HKEY_LOCAL_MACHINE if machine else winreg.HKEY_CURRENT_USER
    own_key = r"Software\SimTransferLab\Installer" + "\\" + brand
    check(registry_value(hive, own_key, "InstallFolder") is None, "existing install directory record")
    copy = work / original.name
    shutil.copy2(original, copy)
    msi = Path(release["msi"])
    product = "{" + release["productCode"] + "}"
    candidate = "{" + upgrade["productCode"] + "}" if upgrade else None
    allowed = {product}
    if candidate:
        allowed.add(candidate)
    results = {}

    def record(name, value=True):
        results[name] = value
        (work / "observations.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        print(name, json.dumps(value, ensure_ascii=True), flush=True)

    def apply_msi(name, arguments, *, ok=True):
        return command(["msiexec.exe", *arguments, "/qn", "/norestart", "/l*v", work / (name + ".log")], expected=ok)

    def bundle(exe, name, action, *properties, ok=True):
        return command([exe, action, "/quiet", "/norestart", "/log", work / (name + ".log"), *properties], expected=ok)

    def maintain(exe, name, action, selected=None):
        log = work / (name + ".log")
        args = [exe, "/norestart", "/log", log] if machine else ["msiexec.exe", "/i", msi, "/norestart", "/l*v", log]
        return maintain_through_wizard(args, brand, install_dir, action, selected)

    def owned():
        value = registry_value(hive, own_key, "InstallFolder")
        return value and Path(value).resolve() == install_dir.resolve()

    def selected(on):
        paths = [install_dir / ("bin/" + exe) for exe in ("SimTransferCli.exe", "SimRtProbe.exe", "SimTransferShell.cmd")]
        check(all(p.is_file() == on for p in [*paths, desktop, cli_link]), "optional files or shortcuts disagree")

    try:
        if machine:
            install_command = [copy, "/norestart", "/log", work / "01-install.log"]
        else:
            install_command = ["msiexec.exe", "/i", msi, "/norestart", "/l*v", work / "01-install.log"]
        install_through_wizard(install_command, brand, install_dir)
        check(owned() and related(brand) == {product} and main_link.is_file(), "custom installation / start menu failed")
        selected(False)
        record("defaultFeaturesAndCustomDirectory")
        entries = application_entries(brand)
        check(len(entries) == 1, f"expected one visible application entry: {entries}")
        cache = Path(entries[0]["cache"]) if machine else None
        if machine:
            check(cache.is_file(), "maintenance cache missing")
            record("singleInstalledAppsEntry", entries[0]["version"])
        record("nativeMaintenanceChoices", maintain(copy, "02-add", "修改(", True))
        copy.unlink()
        selected(True)
        record("modifyAddsToolsAndShortcuts")
        clean_env = {k: v for k, v in os.environ.items() if not k.upper().startswith(("QT_", "QML", "VTK", "HDF5", "CMAKE_PREFIX"))}
        clean_env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32") + ";" + os.environ["SystemRoot"]
        fixture = ROOT / "build/windows-v2-release/tests/model-fixtures/surface.ply"
        command([install_dir / "bin/SimTransfer.exe", "--model", fixture, "--screenshot", work / "installed-model.png"], env=clean_env, cwd=work)
        check((work / "installed-model.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "model screenshot missing")
        record("packagedModelRender")
        record("loopbackFrames", check_network(install_dir, work, clean_env))
        plugin = Path("plugins/platforms/qwindows.dll")
        library = Path("bin/hdf5.dll")
        (install_dir / plugin).unlink()
        with (install_dir / library).open("r+b") as out:
            out.seek(-1, 2)
            pos = out.tell()
            byte = out.read(1)[0]
            out.seek(pos)
            out.write(bytes([byte ^ 1]))
        check(sha256(install_dir / library) != sha256(stage / library), "damage fixture did not change")
        desktop.unlink()
        maintain(cache, "03-cached-repair", "修复(")
        for item in (plugin, library):
            check(sha256(install_dir / item) == sha256(stage / item), f"repair mismatch: {item}")
        selected(True)
        record("repairRestoresMissingPluginCorruptDllAndShortcut", "cached EXE" if machine else "MSI")
        maintain(cache, "04-remove-options", "修改(", False)
        selected(False)
        record("modifyRemovesOptions")
        if upgrade:
            apply_msi("05-enable-tools", ["/i", msi, "ADDLOCAL=CommandLine"])
            code = apply_msi("06-rollback", ["/i", upgrade["msi"], "SIM_TEST_ROLLBACK=1"], ok=False)
            check(code == 1603 and owned() and related(brand) == {product} and (install_dir / "bin/SimTransferCli.exe").is_file(),
                  "upgrade rollback did not restore original product")
            command([install_dir / "bin/SimTransfer.exe", "--smoke-test"], env=clean_env, cwd=work)
            record("failedUpgradeRestoredPreviousVersion", code)
            bundle(upgrade["setup"], "07-upgrade", "/install")
            check(owned() and related(brand) == {candidate} and (install_dir / "bin/SimTransferCli.exe").is_file()
                  and not desktop.exists(), "upgrade lost directory, features or product registration")
            record("upgradePreservesDirectoryAndFeatures")
            code = bundle(original, "08-downgrade", "/install", ok=False)
            check(related(brand) == {candidate} and owned(), "downgrade changed the installed version")
            record("downgradeBlocked", code)
            entries = application_entries(brand)
            check(len(entries) == 1 and entries[0]["version"].startswith(upgrade["version"]), "wrong Apps entry after upgrade")
            cache = Path(entries[0]["cache"])
        if machine:
            bundle(cache, "09-uninstall", "/uninstall")
        else:
            apply_msi("09-uninstall", ["/x", product])
        check(not related(brand) and not application_entries(brand) and not owned(), "product registration survived uninstall")
        check(not (install_dir / "bin/SimTransfer.exe").exists() and not main_link.exists() and not desktop.exists(), "product file survived uninstall")
        check(data.read_bytes() == b"preserve user data\n" and paths_before == global_paths(), "user data or PATH changed")
        record("uninstallPreservedDataAndPath")
        if machine:
            bundle(original, "10-reinstall", "/install", f"InstallFolder={install_dir}")
            check(owned() and data.is_file(), "reinstallation failed")
            entries = application_entries(brand)
            maintain(Path(entries[0]["cache"]), "11-remove-reinstall", "卸载(")
            check(not related(brand) and not application_entries(brand), "reinstall cleanup failed")
            record("reinstallAfterUninstall")
        else:
            data.unlink()
            for folder in (install_dir, install_dir.parent, base):
                folder.rmdir()
    finally:
        # Only uninstall the registered test transaction at its own unique path.
        registered = related(brand)
        if registered and registered <= allowed and owned():
            entries = application_entries(brand)
            if machine and len(entries) == 1 and entries[0]["cache"]:
                bundle(Path(entries[0]["cache"]), "cleanup-bundle", "/uninstall")
            else:
                for current in registered:
                    apply_msi("cleanup-msi", ["/x", current])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("work", type=Path, help="a new evidence directory")
    parser.add_argument("--machine-upgrade", type=Path, help="exercise the actual machine bundle and test upgrade")
    parser.add_argument("--prepare-upgrade", action="store_true", help="build the next-version rollback fixture only")
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    data = json.loads(args.release.read_bytes())
    if args.prepare_upgrade and args.machine_upgrade:
        parser.error("upgrade preparation and machine installation are separate steps")
    if not args.machine_upgrade and (args.prepare_upgrade or data.get("scope") != "user"):
        build_work = work / "packages"
        build_work.mkdir()
        family = "SimTransferRT" if args.prepare_upgrade else "SimTransferRT-QA-" + os.urandom(4).hex()
        version = data["version"]
        if args.prepare_upgrade:
            major, minor, patch = version.split(".")
            version = f"{major}.{minor}.{int(patch) + 1}"
        exe, msi, code = build_installer(ROOT / "build/windows-v2-release", build_work, Path(data["stage"]),
                                         version, family, data["wix"], per_user=not args.prepare_upgrade,
                                         rollback_test=args.prepare_upgrade)
        data = {"family": family, "msi": str(msi), "setup": str(exe), "productCode": code,
                "version": version, "sha256": sha256(exe), "stage": data["stage"],
                "scope": "machine" if args.prepare_upgrade else "user"}
        target = work / ("upgrade.json" if args.prepare_upgrade else "user.json")
        target.write_text(json.dumps(data, indent=2), encoding="utf-8")
        if args.prepare_upgrade:
            print(target)
            return
    upgrade = json.loads(args.machine_upgrade.read_bytes()) if args.machine_upgrade else None
    try:
        exercise(data, upgrade, work, upgrade is not None)
        check_rollback_log_permissions(work)
    except Exception as error:
        (work / "failure.txt").write_text(str(error), encoding="utf-8")
        raise
    (work / "passed.txt").write_text("PASS\n", encoding="ascii")


if __name__ == "__main__":
    main()
