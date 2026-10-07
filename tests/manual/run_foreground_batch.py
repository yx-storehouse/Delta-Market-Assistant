"""Run a user-authorized finite navigation/observation batch with one focus lease.

No probe in the batch may activate the game or restore the IDE itself.
The outer finally restores once, including on failure/timeout. Not part of CTest.
"""
import argparse
import base64
import ctypes as c
from ctypes import wintypes as w
import datetime
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import threading
import time

from foreground_batch_core import execute_batch, stabilize_capture, filter_expectation_matches
from navigate_lobby_to_warehouse import u, activate, Input

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True)
    parser.add_argument('--record', required=True)
    args = parser.parse_args()
    plan = json.loads(Path(args.plan).read_text(encoding='utf-8'))
    output = (ROOT / args.record).resolve()
    assert output.is_relative_to(ROOT / 'artifacts') and output.suffix == '.json' and not output.exists()
    steps = plan['steps']
    assert isinstance(steps, list) and 1 <= len(steps) <= 20
    assert all(s.get('kind') in ('capture', 'click', 'hover') for s in steps)
    assert steps[0]['kind'] == 'capture'
    timeout = plan.get('timeout_seconds', 25)
    assert 1 <= timeout <= 30
    query = r"""$ErrorActionPreference='Stop';
    $g=@(Get-Process DeltaForceClient-Win64-Shipping|Where-Object {$_.MainWindowHandle -ne 0});
    $i=@(Get-Process Mirasim|Where-Object {$_.MainWindowHandle -ne 0});
    if($g.Count -ne 1 -or $i.Count -ne 1){throw 'Window identity ambiguous'};
    @{target_hwnd=[string]$g[0].MainWindowHandle;target_pid=[string]$g[0].Id;
      return_hwnd=[string]$i[0].MainWindowHandle;return_pid=[string]$i[0].Id}|ConvertTo-Json -Compress
    """
    found = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', query],
        capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
    assert found.returncode == 0
    handles = {key: int(value) for key, value in json.loads(found.stdout).items()}
    exe = ROOT / 'dist/RelinkStudio/RelinkStudio.exe'
    command = [str(exe), '--live-capture-check', '--focus-policy', 'caller-owned']
    for key, value in handles.items(): command += ['--' + key.replace('_', '-'), str(value)]
    command += ['--frames', '1', '--ocr']
    # Resolve identity in the diagnostic on every capture. This never relaxes
    # the existing executable allowlist, viewport or occlusion validation.
    previous = {}; last_preview = None; clicks = 0
    dpi = u.SetThreadDpiAwarenessContext(c.c_void_p(-4)); assert dpi
    pointer = w.POINT(); assert u.GetCursorPos(c.byref(pointer))
    transitions = []
    halt = threading.Event()
    def sample():
        window = u.GetForegroundWindow()
        if not transitions or transitions[-1]['hwnd'] != window:
            transitions.append(dict(hwnd=window, monotonic_ms=round(time.monotonic()*1000)))
    def monitor():
        while not halt.is_set(): sample(); halt.wait(.01)
    watcher = threading.Thread(target=monitor, daemon=True); watcher.start()
    def enter():
        activate(handles['target_hwnd'], handles['target_pid'])
        origin = w.POINT(); rect = w.RECT()
        assert u.ClientToScreen(handles['target_hwnd'], c.byref(origin))
        assert u.GetClientRect(handles['target_hwnd'], c.byref(rect))
        neutral = w.POINT(origin.x+rect.right//2, origin.y+rect.bottom//3)
        if u.GetAncestor(u.WindowFromPoint(neutral),2) == handles['target_hwnd']:
            u.SetCursorPos(neutral.x, neutral.y)
        time.sleep(.4)
    def perform(step, remaining):
        nonlocal previous, last_preview, clicks
        if step['kind'] == 'capture':
            if step.get('neutral_pointer'):
                rect=w.RECT();origin=w.POINT()
                assert u.GetForegroundWindow()==handles['target_hwnd']
                assert u.GetClientRect(handles['target_hwnd'],c.byref(rect))
                assert u.ClientToScreen(handles['target_hwnd'],c.byref(origin))
                point=w.POINT(origin.x+rect.right//2,origin.y+rect.bottom//3)
                assert u.GetAncestor(u.WindowFromPoint(point),2)==handles['target_hwnd']
                assert u.SetCursorPos(point.x,point.y)
                time.sleep(.15)
            argv = list(command)
            if step.get('expected_page'): argv += ['--expected-page', step['expected_page']]
            if step.get('preview'): argv += ['--preview-stdout']
            if step.get('market_anchors'): argv += ['--ocr-market-anchors']
            if step.get('ui_regions'): argv += ['--ocr-ui-regions']
            if step.get('filter_state'): argv += ['--catalog-filter-state']
            def once(budget):
                nonlocal previous,last_preview
                p = subprocess.run(argv, capture_output=True, timeout=max(.1,min(20,budget)),
                    creationflags=subprocess.CREATE_NO_WINDOW)
                result = json.loads(p.stdout)
                preview = result.pop('preview_png_base64', None)
                if preview: last_preview = preview
                previous = result
                passed = p.returncode == 0 and result.get('capture_passed') and result.get('ocr_passed')
                passed = passed and result.get('focus_activation_requests') == 0 and result.get('focus_restore_requests') == 0
                if step.get('expected_filter') is not None:
                    result['filter_expectation_passed']=filter_expectation_matches(result,step['expected_filter'])
                    passed=passed and result['filter_expectation_passed']
                return dict(kind='capture', passed=bool(passed), command=argv, result=result,
                    stderr=p.stderr.decode('utf-8',errors='replace'), exit_status=p.returncode)
            return stabilize_capture(once,lambda:u.GetForegroundWindow()==handles['target_hwnd'],
                attempts=step.get('attempts',3 if step.get('expected_page') else 1),deadline=time.monotonic()+remaining)
        assert previous.get('startup_page',{}).get('page') == step['expected_before'] != 'unknown'
        assert previous['startup_page']['overlay'] == 'none'
        rect = w.RECT(); origin = w.POINT()
        assert u.GetClientRect(handles['target_hwnd'],c.byref(rect)) and u.ClientToScreen(handles['target_hwnd'],c.byref(origin))
        assert (rect.right,rect.bottom) == (previous['frames'][0]['width'],previous['frames'][0]['height'])
        if step.get('viewport'): assert [rect.right,rect.bottom] == step['viewport']
        x,y = step['point']; assert isinstance(x,int) and isinstance(y,int) and 0<x<rect.right and 0<y<rect.bottom
        point = w.POINT(origin.x+x,origin.y+y)
        assert u.GetAncestor(u.WindowFromPoint(point),2) == handles['target_hwnd']
        assert not any(u.GetAsyncKeyState(key)&0x8000 for key in [1,2,16,17,18])
        assert u.SetCursorPos(point.x,point.y)
        if step['kind'] == 'click':
            assert u.GetForegroundWindow() == handles['target_hwnd']
            pair=(Input*2)();pair[0].data.mi.dwFlags=2;pair[1].data.mi.dwFlags=4
            sent=u.SendInput(2,pair,c.sizeof(Input))
            if sent==1:
                release=Input();release.data.mi.dwFlags=4;u.SendInput(1,c.byref(release),c.sizeof(Input))
            assert sent==2
            clicks+=1
            previous={}  # Require a fresh page observation before another click.
        time.sleep(min(.6,max(.01,remaining/4)))
        return dict(kind=step['kind'],point=[x,y],passed=True)
    def leave():
        activate(handles['return_hwnd'], handles['return_pid'])
        u.SetCursorPos(pointer.x,pointer.y)
        sample()
        return u.GetForegroundWindow()==handles['return_hwnd']
    try:
        report = execute_batch(steps, enter, perform, leave,
            lambda: u.GetForegroundWindow()==handles['target_hwnd'], timeout=timeout)
    finally:
        halt.set();watcher.join(timeout=1);u.SetThreadDpiAwarenessContext(dpi)
    report.update(timestamp=datetime.datetime.now().astimezone().isoformat(),
        binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), plan=plan, identities=handles,
        ocr_helper_sha256=hashlib.sha256((exe.parent/'vision/windows_ocr_worker.ps1').read_bytes()).hexdigest(),
        runner_command=[sys.executable,*sys.argv],exit_status=0 if report['passed'] else 1,
        foreground_transitions=transitions, foreground_sampling_interval_ms=10,
        manual_clicks=clicks, image_file_writes=0)
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.open('x',encoding='utf-8') as file: json.dump(report,file,ensure_ascii=False,indent=2)
    encoded=None
    if last_preview:
        from PIL import Image
        im=Image.open(io.BytesIO(base64.b64decode(last_preview))); buf=io.BytesIO(); im.save(buf,format='JPEG',quality=68)
        encoded=base64.b64encode(buf.getvalue()).decode('ascii')
    print(json.dumps(dict(metadata=report,preview_jpeg=encoded),ensure_ascii=False))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
