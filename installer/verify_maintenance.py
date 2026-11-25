"""Exercise native Windows Installer dialogs and read their actual on-screen controls."""
import ctypes
from ctypes import wintypes
import subprocess
import time

ui = ctypes.WinDLL("user32")
HWND = wintypes.HWND
callback = ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, wintypes.LPARAM)
ui.EnumWindows.argtypes = [callback, wintypes.LPARAM]
ui.EnumChildWindows.argtypes = [HWND, callback, wintypes.LPARAM]
ui.GetWindowTextW.argtypes = [HWND, wintypes.LPWSTR, ctypes.c_int]
ui.GetWindowThreadProcessId.argtypes = [HWND, ctypes.POINTER(wintypes.DWORD)]
ui.IsWindowVisible.argtypes = [HWND]
ui.PostMessageW.argtypes = [HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
ui.SendMessageTimeoutW.argtypes = [HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
                                  wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
ui.GetClassNameW.argtypes = [HWND, wintypes.LPWSTR, ctypes.c_int]
ui.IsWindowEnabled.argtypes = [HWND]


def windows(parent=None):
    result = []

    @callback
    def collect(hwnd, unused):
        if ui.IsWindowVisible(hwnd):
            result.append(hwnd)
        return True

    if parent:
        ui.EnumChildWindows(parent, collect, 0)
    else:
        ui.EnumWindows(collect, 0)
    return result


def caption(handle):
    buf = ctypes.create_unicode_buffer(2048)
    ui.GetWindowTextW(handle, buf, len(buf))
    return buf.value.replace("&", "")


def process_id(handle):
    pid = wintypes.DWORD()
    ui.GetWindowThreadProcessId(handle, ctypes.byref(pid))
    return pid.value


def send(handle, message, wparam=0, lparam=0):
    result = ctypes.c_size_t()
    if not ui.SendMessageTimeoutW(handle, message, wparam, lparam, 2, 3000, ctypes.byref(result)):
        raise RuntimeError("the installer control stopped responding")
    return result.value


def control(dialog, label):
    return next((h for h in windows(dialog) if caption(h).startswith(label)), None)


def current_dialog(proc, title=None, pid=None, label=None, timeout=35):
    until = time.monotonic() + timeout
    while proc.poll() is None and time.monotonic() < until:
        for h in windows():
            if pid and process_id(h) != pid:
                continue
            if title and caption(h) != title:
                continue
            if ui.IsWindowEnabled(h) and (not label or control(h, label)):
                return h
        time.sleep(.1)
    raise RuntimeError(f"installer page missing: {title or label} (exit={proc.poll()})")


def click(dialog, label):
    button = control(dialog, label)
    if not button or not ui.IsWindowEnabled(button) or not ui.PostMessageW(button, 0x00F5, 0, 0):
        raise RuntimeError(f"installer control unavailable: {label}")


def edit_path(dialog, directory):
    for item in windows(dialog):
        name = ctypes.create_unicode_buffer(128)
        ui.GetClassNameW(item, name, len(name))
        if name.value.lower() == "edit" or name.value.lower().startswith("richedit"):
            text = ctypes.create_unicode_buffer(str(directory))
            if not send(item, 0x000C, 0, ctypes.addressof(text)):  # WM_SETTEXT
                raise RuntimeError("cannot enter the chosen folder")
            return
    raise RuntimeError("folder browser lacks an editable path")


def cancel_dialogs(pid, proc):
    deadline = time.monotonic() + 15
    while proc.poll() is None and time.monotonic() < deadline:
        for page in windows():
            if process_id(page) != pid or not ui.IsWindowEnabled(page):
                continue
            for label in ("是(", "完成(", "取消"):
                if control(page, label) and ui.IsWindowEnabled(control(page, label)):
                    click(page, label)
                    break
        time.sleep(.2)


def confirm_and_finish(proc, brand, pid, folder):
    confirm = current_dialog(proc, title=brand + " 确认操作", pid=pid)
    if not any(str(folder).casefold() in caption(h).casefold() for h in windows(confirm)):
        raise RuntimeError("confirmation page lost the selected directory")
    click(confirm, "确认(")
    complete = current_dialog(proc, pid=pid, label="完成(", timeout=120)
    click(complete, "完成(")
    if proc.wait(timeout=35) != 0:
        raise RuntimeError("interactive installer operation failed")


OPTIONS = ("安装命令行工具", "创建桌面快捷方式")


def install_through_wizard(command, brand, folder):
    proc = subprocess.Popen(command)
    pid = None
    try:
        welcome = current_dialog(proc, title=brand + " 安装", label="下一步")
        pid = process_id(welcome)
        click(welcome, "下一步")
        directory = current_dialog(proc, pid=pid, label="更改(")
        click(directory, "更改(")
        browser = current_dialog(proc, pid=pid, label="确定")
        edit_path(browser, folder)
        click(browser, "确定")
        directory = current_dialog(proc, pid=pid, label="更改(")
        click(directory, "下一步")
        features = current_dialog(proc, title=brand + " 安装选项", pid=pid)
        for option in OPTIONS:
            box = control(features, option)
            if not box or send(box, 0x00F0):  # BM_GETCHECK
                raise RuntimeError(f"unexpected default: {option}")
        click(features, "下一步")
        confirm_and_finish(proc, brand, pid, folder)
    finally:
        if pid and proc.poll() is None:
            cancel_dialogs(pid, proc)


def maintain_through_wizard(command, brand, folder, action, selected=None):
    proc = subprocess.Popen(command)
    pid = None
    try:
        page = current_dialog(proc, title=brand + " 维护", label=action)
        pid = process_id(page)
        texts = [caption(h) for h in windows(page)]
        choices = [s for s in texts if s.startswith(("修改(", "修复(", "卸载("))]
        if len(choices) != 3 or not any(str(folder).casefold() in s.casefold() for s in texts):
            raise RuntimeError("missing native maintenance controls or install path")
        click(page, action)
        if action == "修改(":
            page = current_dialog(proc, title=brand + " 安装选项", pid=pid)
            for option in OPTIONS:
                box = control(page, option)
                if not box or bool(send(box, 0x00F0)) == selected:
                    raise RuntimeError(f"previous selection was not restored: {option}")
                click(page, option)
            click(page, "下一步")
        confirm_and_finish(proc, brand, pid, folder)
        return choices
    finally:
        if pid and proc.poll() is None:
            cancel_dialogs(pid, proc)
