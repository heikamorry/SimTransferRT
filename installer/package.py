"""Build an offline MSI/Burn installer and portable ZIP from one CMake Release installation."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
WIX_NS = "http://wixtoolset.org/schemas/v4/wxs"
CLI = {"bin/simtransfercli.exe", "bin/simrtprobe.exe", "bin/simtransfershell.cmd"}
REDIST = "bin/vc_redist.x64.exe"
ET.register_namespace("", WIX_NS)


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def guid(name):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "urn:simtransferlab:simtransferrt:" + name)).upper()


def identifier(prefix, path):
    return prefix + hashlib.sha256(path.lower().encode("utf-8")).hexdigest()[:28]


def payload_file(stage, output):
    def node(parent, tag, **attributes):
        return ET.SubElement(parent, f"{{{WIX_NS}}}{tag}", attributes)

    root = ET.Element(f"{{{WIX_NS}}}Wix")
    fragment = node(root, "Fragment")
    top = node(fragment, "DirectoryRef", Id="INSTALLFOLDER")
    groups = {name: node(fragment, "ComponentGroup", Id=name) for name in ("CorePayload", "CliPayload")}
    sizes = {name: 0 for name in groups}
    directories = {Path("."): top}
    paths = sorted(stage.rglob("*"), key=lambda p: p.relative_to(stage).as_posix().lower())
    for path in paths:
        relative = path.relative_to(stage)
        key = relative.as_posix().lower()
        parent = directories[relative.parent]
        if path.is_dir():
            directories[relative] = node(parent, "Directory", Id=identifier("D", key), Name=path.name)
            continue
        if key == REDIST:
            continue
        if path.suffix.lower() == ".exe" and key not in CLI | {"bin/simtransfer.exe"}:
            raise ValueError(f"unexpected executable in release stage: {relative}")
        group = "CliPayload" if key in CLI else "CorePayload"
        component_id = identifier("C", key)
        component = node(parent, "Component", Id=component_id, Guid="*")
        node(component, "File", Id=identifier("F", key), Name=path.name, KeyPath="yes",
             Source=str(path))
        node(groups[group], "ComponentRef", Id=component_id)
        sizes[group] += path.stat().st_size
    ET.indent(root, space="  ")
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)
    return math.ceil(sizes["CorePayload"] / 2**20), math.ceil(sum(sizes.values()) / 2**20)


def cmake_setting(cache, key):
    match = re.search(rf"^{re.escape(key)}:[^=]+=(.*)$", cache, re.MULTILINE)
    return match.group(1).strip() if match else ""


def stage_product(build, stage, cache):
    run(["cmake", "--install", build, "--config", "Release", "--prefix", stage])
    shutil.copy2(HERE / "SimTransferShell.cmd", stage / "bin")
    for name in (*CLI, REDIST, "bin/simtransfer.exe", "bin/qt.conf", "bin/hdf5.dll",
                 "plugins/platforms/qwindows.dll"):
        if not (stage / name).is_file():
            raise FileNotFoundError(stage / name)
    licenses = stage / "share/licenses"
    shutil.copytree(HERE / "licenses", licenses / "Qt", dirs_exist_ok=True)
    qt_path = Path(cmake_setting(cache, "Qt6_DIR")).parents[2]
    sbom = sorted((qt_path / "sbom").glob("*.cdx.json"))
    for doc in sbom:
        if doc.name.split("-")[0] in {"qtbase", "qtsvg", "qttranslations", "qttools"}:
            shutil.copy2(doc, licenses / "Qt")
    boost = cmake_setting(cache, "SIM_BOOST_ROOT")
    candidates = [Path(boost)] if boost else []
    candidates += [ROOT / "build/asio-deps/boost_1_88_0",
                   Path(os.environ["LOCALAPPDATA"]) / "SimTransferRT/deps/boost_1_88_0"]
    boost_license = next((p / "LICENSE_1_0.txt" for p in candidates if (p / "LICENSE_1_0.txt").is_file()), None)
    if not boost_license:
        raise FileNotFoundError("Boost LICENSE_1_0.txt missing from the configured SDK")
    (licenses / "Boost").mkdir(exist_ok=True)
    shutil.copy2(boost_license, licenses / "Boost")


def runtime_version(redist):
    script = """
$ErrorActionPreference='Stop'
$env:PSModulePath=(Join-Path $PSHOME 'Modules')
$f=Get-Item -LiteralPath $env:SIM_VERIFY_REDIST
$s=Get-AuthenticodeSignature -LiteralPath $f.FullName
if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(,|$)') {
    throw 'Expected a Microsoft-signed Visual C++ Redistributable'
}
$v=$f.VersionInfo
'{0}.{1}.{2}.{3}' -f $v.FileMajorPart,$v.FileMinorPart,$v.FileBuildPart,$v.FilePrivatePart
"""
    return run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
               env={**os.environ, "SIM_VERIFY_REDIST": str(redist)}, capture_output=True, text=True).stdout.strip()


def portable_runtime(stage, cache):
    # Use the CRT shipped with the same redistributable as the Burn prerequisite.
    roots = []
    if os.environ.get("VCToolsRedistDir"):
        roots.append(Path(os.environ["VCToolsRedistDir"]))
    compiler = Path(cmake_setting(cache, "CMAKE_CXX_COMPILER"))
    vc = next((parent for parent in compiler.parents if parent.name.upper() == "VC"), None)
    if vc:
        roots.extend((vc / "Redist/MSVC").glob("*"))
    expected = sha256(stage / REDIST)
    for root in roots:
        exe = root / "vc_redist.x64.exe"
        if exe.is_file() and sha256(exe) == expected:
            files = sorted(root.glob("x64/Microsoft.VC*.CRT/*.dll"))
            if {"msvcp140.dll", "vcruntime140.dll"} <= {p.name.lower() for p in files}:
                return files
    raise FileNotFoundError("matching x64 MSVC redistributable CRT DLLs missing; run scripts/env.cmd")


def portable_zip(stage, runtime, target):
    files = {p.relative_to(stage).as_posix(): p for p in stage.rglob("*") if p.is_file()}
    # The portable build loads the CRT beside the application, without running the bootstrapper.
    del files[REDIST]
    for dll in runtime:
        name = "bin/" + dll.name
        if name in files and sha256(files[name]) != sha256(dll):
            raise ValueError(f"conflicting runtime DLL in release stage: {name}")
        files[name] = dll
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, source in sorted(files.items()):
            archive.write(source, name)
    with zipfile.ZipFile(target) as archive:
        if sorted(archive.namelist()) != sorted(files):
            raise ValueError("portable ZIP file list differs from release inputs")
        for name, source in files.items():
            with archive.open(name) as content:
                if hashlib.file_digest(content, "sha256").hexdigest() != sha256(source):
                    raise ValueError(f"portable ZIP file differs from release input: {name}")
    return len(files)


def wix_tools(cache, version, env):
    tools_dir = cache / ("wix" + version.split(".")[0]) / version
    executable = tools_dir / "wix.exe"
    if not executable.is_file():
        run(["dotnet", "tool", "install", "wix", "--tool-path", tools_dir, "--version", version,
             "--allow-roll-forward", "--source", "https://api.nuget.org/v3/index.json"], env=env)
    ext_root = cache / "wix-extensions"
    ext_root.mkdir(exist_ok=True)
    for name in ("UI", "Bal", "Util"):
        package = f"WixToolset.{name}.wixext"
        folder = ext_root / ".wix/extensions" / package / version
        if not list(folder.glob("wixext*/*.wixext.dll")):
            run([executable, "extension", "add", f"{package}/{version}"], cwd=ext_root, env=env)
    return executable, ext_root


def sign(path, thumbprint):
    if not re.fullmatch(r"[A-Fa-f0-9]{40}", thumbprint):
        raise ValueError("signing certificate thumbprint must be 40 hexadecimal characters")
    signtool = shutil.which("signtool.exe")
    if not signtool:
        kits = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/bin"
        signtool = next(iter(sorted(kits.glob("*/x64/signtool.exe"), reverse=True)), None)
    if not signtool:
        raise FileNotFoundError("Windows SDK signtool.exe")
    run([signtool, "sign", "/sha1", thumbprint, "/fd", "SHA256", "/tr",
         os.environ.get("SIM_TIMESTAMP_URL", "http://timestamp.digicert.com"), "/td", "SHA256", path])
    run([signtool, "verify", "/pa", path])


def build_installer(build, work, stage, version, family, wix_version, thumbprint=None,
                    per_user=False, rollback_test=False):
    env = {**os.environ, "DOTNET_ROLL_FORWARD": "Major"}
    wix, ext_root = wix_tools(ROOT / "build/installer", wix_version, env)
    brand = family
    msi_code = guid(f"{family}:msi:{version}")
    bundle_file = work / f"{brand}-{version}-x64-Setup.exe"
    msi = work / f"{brand}.msi"
    generated = work / "Payload.wxs"
    core_size, full_size = payload_file(stage, generated)
    redist = stage / REDIST
    definitions = {
        "Brand": brand, "Version": version, "ProductCode": msi_code, "MsiFamily": guid(family + ":msi-upgrade"),
        "BundleFamily": guid(family + ":bundle-upgrade"), "IconFile": ROOT / "resources/app.ico",
        "RegistryRoot": "HKCU" if per_user else "HKLM", "RegistryKey": f"Software\\SimTransferLab\\Installer\\{family}",
        "Scope": "perUser" if per_user else "perMachine",
        "InstallParent": "LocalAppDataFolder" if per_user else "ProgramFiles64Folder",
        "Actions": build / "SimSetupActions.dll", "Msi": msi, "Redist": redist,
        "RedistVersion": runtime_version(redist), "CoreMiB": core_size, "FullMiB": full_size,
    }
    if rollback_test:
        definitions["Validation"] = 1
    base = [wix, "build", "-arch", "x64", "-culture", "zh-CN", "-pdbtype", "none"]
    if int(wix_version.split(".")[0]) >= 7 and os.environ.get("SIM_WIX_ACCEPT_EULA"):
        base += ["-acceptEula", os.environ["SIM_WIX_ACCEPT_EULA"]]
    for key, value in definitions.items():
        base += ["-d", f"{key}={value}"]

    def extension(name):
        folder = ext_root / ".wix/extensions" / f"WixToolset.{name}.wixext" / wix_version
        return next(folder.glob("wixext*/*.wixext.dll"))

    run([*base, "-ext", extension("UI"), "-o", msi, HERE / "Product.wxs", HERE / "Interface.wxs",
         generated], cwd=ext_root, env=env)
    if thumbprint:
        sign(msi, thumbprint)
    run([*base, "-ext", extension("Bal"), "-ext", extension("Util"), "-o", bundle_file,
         HERE / "Bundle.wxs"], cwd=ext_root, env=env)
    if thumbprint:
        engine = work / "engine.exe"
        run([wix, "burn", "detach", bundle_file, "-engine", engine], env=env)
        sign(engine, thumbprint)
        signed = work / "reattached.exe"
        run([wix, "burn", "reattach", bundle_file, "-engine", engine, "-o", signed], env=env)
        signed.replace(bundle_file)
        sign(bundle_file, thumbprint)
    return bundle_file, msi, msi_code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/windows-v2-release")
    parser.add_argument("--out", type=Path, default=ROOT / "dist")
    parser.add_argument("--sign-thumbprint", default=os.environ.get("SIM_SIGN_THUMBPRINT"))
    args = parser.parse_args()
    build = args.build.resolve()
    settings = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    if any(cmake_setting(settings, key) != expected for key, expected in
           {"CMAKE_BUILD_TYPE": "Release", "SIM_MODEL_PREVIEW": "ON", "SIM_DEPLOY_QT": "ON"}.items()):
        parser.error("requires a Release build with model preview and Qt deployment")
    version = cmake_setting(settings, "CMAKE_PROJECT_VERSION")
    wix_version = os.environ.get("SIM_WIX_VERSION", "5.0.2")
    cache = ROOT / "build/installer"
    cache.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="release-", dir=cache))
    stage = work / "stage"
    stage_product(build, stage, settings)
    exe, msi, product = build_installer(build, work, stage, version, "SimTransferRT", wix_version,
                                        args.sign_thumbprint)
    dest = args.out.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    prefix = f"SimTransferRT-{version}-x64"
    with tempfile.TemporaryDirectory(dir=dest) as tmp:
        setup = Path(tmp) / exe.name
        shutil.copyfile(exe, setup)
        portable = Path(tmp) / f"{prefix}-Portable.zip"
        verified = portable_zip(stage, portable_runtime(stage, settings), portable)
        sums = Path(tmp) / f"{prefix}-SHA256SUMS.txt"
        setup_hash, portable_hash = sha256(setup), sha256(portable)
        sums.write_text(f"{setup_hash}  {setup.name}\n{portable_hash}  {portable.name}\n",
                        encoding="ascii", newline="\n")
        for artifact in (setup, portable, sums):
            artifact.replace(dest / artifact.name)
    evidence = {"setup": str(dest / exe.name), "sha256": setup_hash, "bytes": exe.stat().st_size,
                "portable": str(dest / portable.name), "portableSha256": portable_hash,
                "portableBytes": (dest / portable.name).stat().st_size, "verifiedPortableFiles": verified,
                "checksums": str(dest / sums.name),
                "version": version, "wix": wix_version, "signed": bool(args.sign_thumbprint),
                "msi": str(msi), "productCode": product, "stage": str(stage), "work": str(work)}
    (work / "release.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
