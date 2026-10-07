"""Explicit one-shot live probe. Never invoked by CTest or offline package tests.

Requires the user-prepared game and IDE. Re-resolves HWND/PID each invocation,
records only JSON metadata/button anchors, and checks the restored foreground.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expected-page', required=True, choices=['lobby', 'warehouse', 'mandel', 'skin_home',
        'empty_watchlist', 'catalog_filter', 'skin_listings', 'watchlist_listings', 'game_settings', 'base'])
    parser.add_argument('--record', required=True, help='New JSON metadata file below workspace artifacts/.')
    parser.add_argument('--lobby-anchors', action='store_true')
    args = parser.parse_args()
    output = (ROOT / args.record).resolve()
    if not output.is_relative_to(ROOT / 'artifacts') or output.suffix != '.json' or output.exists():
        raise SystemExit('Use a new .json record under artifacts/; existing evidence is immutable.')
    query = r"""$ErrorActionPreference='Stop';
    $game=@(Get-Process DeltaForceClient-Win64-Shipping | Where-Object {$_.MainWindowHandle -ne 0});
    $ide=@(Get-Process Mirasim | Where-Object {$_.MainWindowHandle -ne 0});
    if ($game.Count -ne 1 -or $ide.Count -ne 1) {throw 'Window identity ambiguous'};
    @{target_hwnd=[string]$game[0].MainWindowHandle;target_pid=[string]$game[0].Id;
      return_hwnd=[string]$ide[0].MainWindowHandle;return_pid=[string]$ide[0].Id} | ConvertTo-Json -Compress
    """
    discovery = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', query],
        capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
    if discovery.returncode:
        raise SystemExit('Current game/IDE identity discovery failed before capture.')
    handles = json.loads(discovery.stdout)
    exe = ROOT / 'dist/RelinkStudio/RelinkStudio.exe'
    command = [str(exe), '--live-capture-check']
    for key in ['target_hwnd', 'target_pid', 'return_hwnd', 'return_pid']:
        command += ['--' + key.replace('_', '-'), handles[key]]
    command += ['--frames', '1', '--ocr', '--expected-page', args.expected_page]
    if args.lobby_anchors:
        command += ['--ocr-lobby-anchors']
    record = dict(timestamp=datetime.datetime.now().astimezone().isoformat(), command=command,
        binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        input='User-prepared page; one full-client in-memory frame; exact expected page; no game input.',
        stdout='', stderr='', exit_status=None)
    u = ctypes.WinDLL('user32', use_last_error=True)
    u.GetForegroundWindow.restype = ctypes.c_void_p
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=25,
            creationflags=subprocess.CREATE_NO_WINDOW)
        record.update(stdout=result.stdout.decode('utf-8'), stderr=result.stderr.decode('utf-8', errors='replace'),
            exit_status=result.returncode)
    except subprocess.TimeoutExpired:
        record.update(stderr='OUTER_PROBE_TIMEOUT', exit_status=124)
    finally:
        # An interrupted child cannot execute its C++ guard. Restore only the
        # specifically resolved IDE, with the existing identity-checked helper.
        if u.GetForegroundWindow() != int(handles['return_hwnd']):
            restore = subprocess.run([sys.executable, str(ROOT / 'artifacts/m2_live_capture_transaction/restore_ide.py'),
                '--hwnd', handles['return_hwnd'], '--pid', handles['return_pid']],
                capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
            record['outer_restore'] = dict(exit_status=restore.returncode,
                stdout=restore.stdout.decode('utf-8', errors='replace'), stderr=restore.stderr.decode('utf-8', errors='replace'))
        record['foreground_after'] = u.GetForegroundWindow()
        record['outer_ide_restored'] = record['foreground_after'] == int(handles['return_hwnd'])
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x', encoding='utf-8') as file:
            json.dump(record, file, ensure_ascii=False, indent=2)
    print(json.dumps(record, ensure_ascii=False, indent=2))
    if not record['outer_ide_restored']:
        return 3
    return record['exit_status']


if __name__ == '__main__':
    raise SystemExit(main())
