"""User-authorized, one-click manual calibration; not linked to the application.

Resolve current windows, require a freshly recognized lobby and OCR warehouse
button, click that button once, then read back the page and restore the IDE.
No inventory, watchlist or purchase actions.
"""
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m2_lobby_calibration'
u = c.WinDLL('user32', use_last_error=True)
k = c.WinDLL('kernel32', use_last_error=True)
u.GetForegroundWindow.restype = w.HWND
u.IsIconic.argtypes = [w.HWND]
u.ShowWindowAsync.argtypes = [w.HWND,c.c_int]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowThreadProcessId.restype = w.DWORD
u.SetForegroundWindow.argtypes = [w.HWND]
u.AttachThreadInput.argtypes = [w.DWORD, w.DWORD, w.BOOL]
u.AttachThreadInput.restype = w.BOOL
u.PeekMessageW.argtypes = [c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT]
u.SetThreadDpiAwarenessContext.argtypes = [w.HANDLE]
u.SetThreadDpiAwarenessContext.restype = w.HANDLE
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
u.WindowFromPoint.argtypes = [w.POINT]
u.WindowFromPoint.restype = w.HWND
u.GetAncestor.argtypes = [w.HWND, w.UINT]
u.GetAncestor.restype = w.HWND
u.GetCursorPos.argtypes = [c.POINTER(w.POINT)]
u.SetCursorPos.argtypes = [c.c_int, c.c_int]
u.GetAsyncKeyState.argtypes = [c.c_int]
u.GetAsyncKeyState.restype = c.c_short
k.GetCurrentThreadId.restype = w.DWORD

class MouseInput(c.Structure):
    _fields_ = [('dx', w.LONG), ('dy', w.LONG), ('mouseData', w.DWORD), ('dwFlags', w.DWORD),
                ('time', w.DWORD), ('dwExtraInfo', c.c_size_t)]
class InputUnion(c.Union):
    _fields_ = [('mi', MouseInput)]
class Input(c.Structure):
    _fields_ = [('type', w.DWORD), ('data', InputUnion)]
u.SendInput.argtypes = [w.UINT, c.POINTER(Input), c.c_int]
u.SendInput.restype = w.UINT


def activate(hwnd, pid):
    actual = w.DWORD()
    destination = u.GetWindowThreadProcessId(hwnd, c.byref(actual))
    assert actual.value == pid and destination
    if u.IsIconic(hwnd):
        u.ShowWindowAsync(hwnd,9)
    message = w.MSG(); u.PeekMessageW(c.byref(message), None, 0x400, 0x400, 0)
    current = k.GetCurrentThreadId()
    foreground = u.GetWindowThreadProcessId(u.GetForegroundWindow(), None)
    attached = []
    try:
        for tid in dict.fromkeys([foreground, destination]):
            if tid and tid != current and u.AttachThreadInput(current, tid, True): attached.append(tid)
        u.SetForegroundWindow(hwnd)
    finally:
        for tid in reversed(attached): u.AttachThreadInput(current, tid, False)
    for _ in range(20):
        if u.GetForegroundWindow() == hwnd: return
        time.sleep(.01)
    raise RuntimeError('Game activation did not stick; no click sent.')


def main():
    # Legacy CLI delegates to the batch wrapper, not per-action restoration.
    command = [sys.executable, str(ROOT / 'tests/manual/desktop_navigation_probe.py'),
        '--key', 'lobby_warehouse_batch', '--from-page', 'lobby', '--to-page', 'warehouse',
        '--x', '430', '--y', '73', '--width', '2560', '--height', '1440', '--action', 'click']
    return subprocess.run(command, creationflags=subprocess.CREATE_NO_WINDOW).returncode

if __name__ == '__main__':
    raise SystemExit(main())
