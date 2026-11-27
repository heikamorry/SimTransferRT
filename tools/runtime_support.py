"""Local diagnostics only; never adds fields to the wire protocol."""
from __future__ import annotations
import ctypes
import hashlib
import json
import os
import platform
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def built_executable(build, name, subdir="src"):
    """Resolve the flat Windows release and the standard CMake build/install layouts."""
    filename = name + (".exe" if os.name == "nt" else "")
    for directory in (build, build / subdir, build / "bin"):
        if (directory / filename).is_file():
            return directory / filename
    return build / filename


def probe_udp_loopback():
    """Measure local IPv4 UDP delivery in both directions using live, bound endpoints."""
    report = {"address": "127.0.0.1", "available": False, "exchanges": []}
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as a, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as b:
            for sock in (a, b):
                sock.bind((report["address"], 0))
            for sender, receiver in ((a, b), (b, a)):
                source = sender.getsockname()
                destination = receiver.getsockname()
                exchange = {"source": source, "destination": destination, "attempts": 0,
                            "sent": 0, "matched": 0, "unrelated": 0}
                report["exchanges"].append(exchange)
                payload = os.urandom(24)
                for _ in range(3):
                    exchange["attempts"] += 1
                    if sender.sendto(payload, destination) == len(payload):
                        exchange["sent"] += 1
                    deadline = time.monotonic() + 0.5
                    while (remaining := deadline - time.monotonic()) > 0:
                        receiver.settimeout(remaining)
                        try:
                            packet, peer = receiver.recvfrom(256)
                        except TimeoutError:
                            break
                        if peer == source and packet == payload:
                            exchange["matched"] += 1
                            break
                        exchange["unrelated"] += 1
                    if exchange["matched"]:
                        break
        report["available"] = all(item["matched"] for item in report["exchanges"])
    except OSError as error:
        report["socket_error"] = str(error)
        report["error_code"] = getattr(error, "winerror", None) or error.errno
    return report


def atomic_json(path, value):
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         prefix=path.name + '.', suffix='.tmp', delete=False) as f:
            temporary = Path(f.name)
            json.dump(value, f, indent=2)
            f.write('\n')
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            primary_error = sys.exc_info()[0]
            try:
                temporary.unlink()
            except OSError as cleanup_error:
                # A temp cleanup failure must not replace the write/replace error.
                if primary_error is None:
                    raise
                print(f'WARNING report temp cleanup failed: {temporary}: {cleanup_error}', file=sys.stderr)


def configure_udp_connreset(sock, *, platform_name=None, winsock=None):
    """Windows SDK 10.0.26100.0 um/MSWSock.h: _WSAIOW(IOC_VENDOR,12).
    IOC_IN(0x80000000)|IOC_VENDOR(0x18000000)|12 = 0x9800000C.
    Python 3.13 ioctl supports only RCVALL/KEEPALIVE_VALS/LOOPBACK_FAST_PATH;
    call WSAIoctl explicitly. Success says nothing about peer availability.
    """
    platform_name = sys.platform if platform_name is None else platform_name
    result = dict(platform=platform_name, implementation='not_applicable',
                  status='not_applicable', error_code=None, control_code='0x9800000C')
    if platform_name != 'win32':
        return result
    result['implementation'] = 'ctypes.Ws2_32.WSAIoctl'
    try:
        ws = winsock if winsock is not None else ctypes.WinDLL('Ws2_32.dll')
        # SOCKET=UINT_PTR; BOOL/LONG and DWORD are 32-bit even on Windows x64.
        ws.WSAIoctl.argtypes = [ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p,
                               ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                               ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p, ctypes.c_void_p]
        ws.WSAIoctl.restype = ctypes.c_int
        ws.WSAGetLastError.argtypes = []
        ws.WSAGetLastError.restype = ctypes.c_int
        disabled = ctypes.c_int32(0)
        returned = ctypes.c_uint32(0)
        rc = ws.WSAIoctl(sock.fileno(), 0x9800000C, ctypes.byref(disabled),
                         ctypes.sizeof(disabled), None, 0, ctypes.byref(returned), None, None)
        if rc != 0:
            result.update(status='failed', error_code=ws.WSAGetLastError())
        else:
            result['status'] = 'configured'
    except (OSError, AttributeError) as exc:
        result.update(status='failed', error_code=getattr(exc, 'winerror', None), error=repr(exc))
    return result


def provenance():
    root = Path(__file__).resolve().parents[1]
    digest = hashlib.sha256()
    for path in sorted((root / 'tools').glob('*.py')):
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    info = dict(pid=os.getpid(), python=sys.version, platform=platform.platform(),
                source_sha256=digest.hexdigest(), source_hash_scope='tools/*.py',
                build_type='Python (not Debug/Release)', qt_version=None)
    try:
        info['git_revision'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'],
            cwd=root, text=True, stderr=subprocess.DEVNULL).strip()
        info['git_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'],
            cwd=root, text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError):
        info.update(git_revision=None, git_dirty=None)
    return info
