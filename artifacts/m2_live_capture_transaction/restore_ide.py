"""Restore the explicitly supplied Mirasim window; no game input or capture."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import time

parser = argparse.ArgumentParser()
parser.add_argument('--hwnd', type=int, required=True)
parser.add_argument('--pid', type=int, required=True)
args = parser.parse_args()
u = c.WinDLL('user32', use_last_error=True)
k = c.WinDLL('kernel32', use_last_error=True)
u.GetForegroundWindow.restype = w.HWND
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowThreadProcessId.restype = w.DWORD
u.SetForegroundWindow.argtypes = [w.HWND]
u.SetForegroundWindow.restype = w.BOOL
u.IsWindow.argtypes = [w.HWND]
u.IsIconic.argtypes = [w.HWND]
u.ShowWindowAsync.argtypes = [w.HWND, c.c_int]
u.AttachThreadInput.argtypes = [w.DWORD, w.DWORD, w.BOOL]
u.AttachThreadInput.restype = w.BOOL
u.PeekMessageW.argtypes = [c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT]
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
k.CloseHandle.argtypes = [w.HANDLE]
k.GetCurrentThreadId.restype = w.DWORD
pid = w.DWORD()
destination = u.GetWindowThreadProcessId(args.hwnd, c.byref(pid))
assert u.IsWindow(args.hwnd) and pid.value == args.pid
process = k.OpenProcess(0x1000, False, args.pid)
assert process
try:
    name = c.create_unicode_buffer(32768)
    size = w.DWORD(len(name))
    assert k.QueryFullProcessImageNameW(process, 0, name, c.byref(size))
    assert name.value.rsplit('\\', 1)[-1].lower() == 'mirasim.exe'
finally:
    k.CloseHandle(process)
message = w.MSG()
u.PeekMessageW(c.byref(message), None, 0x400, 0x400, 0)
current = k.GetCurrentThreadId()
foreground_thread = u.GetWindowThreadProcessId(u.GetForegroundWindow(), None)
attached = []
try:
    for tid in dict.fromkeys([foreground_thread, destination]):
        if tid and tid != current and u.AttachThreadInput(current, tid, True):
            attached.append(tid)
    if u.IsIconic(args.hwnd):
        u.ShowWindowAsync(args.hwnd, 9)
    u.SetForegroundWindow(args.hwnd)
finally:
    for tid in reversed(attached):
        u.AttachThreadInput(current, tid, False)
for _ in range(20):
    if u.GetForegroundWindow() == args.hwnd:
        break
    time.sleep(0.01)
actual = u.GetForegroundWindow()
restored = actual == args.hwnd
print(json.dumps(dict(ide_foreground_restored=restored, expected_hwnd=args.hwnd,
                      foreground_hwnd=actual, game_capture=False, game_input_sent=False)))
raise SystemExit(0 if restored else 1)
