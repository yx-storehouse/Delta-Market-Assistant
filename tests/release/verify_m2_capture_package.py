"""Actual M2 package/rollback evidence. Live capture runs are separately recorded.

This verifier never opens a visible app or drives the game. It preserves the
already frozen baseline; the fixed release remains modified after rollback on
an isolated copy. All commands, literal output and statuses are retained.
"""
from __future__ import annotations
import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import verify_pr12a_package as h

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m2_live_capture_transaction'
RELEASE = ROOT / 'dist/RelinkStudio'
BASELINE = TX / 'baseline/release'
BUILD = ROOT / 'build_relocated'
h.TX, h.BASELINE = TX, BASELINE


def prepare_rollback():
    previous = (ROOT / 'artifacts/m1_pr12b_transaction/restore_release.ps1').read_text(encoding='utf-8')
    previous = previous.replace("Write-Output 'ROLLBACK_RESTORED=PASS; new_databases_preserved=true'", r'''
$Added = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'added_release_files.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $Added.PSObject.Properties) {
    $destPath = [IO.Path]::GetFullPath((Join-Path $Destination $entry.Name.Replace('/', '\')))
    if (!$destPath.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Added path escaped release root.' }
    Assert-NoReparse $destPath
    if (Test-Path -LiteralPath $destPath) {
        if ((Get-FileHash -LiteralPath $destPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw 'Added file changed since package validation; preserve it.' }
        Remove-Item -LiteralPath $destPath -Force
    }
}
Write-Output 'ROLLBACK_RESTORED=PASS; new_databases_preserved=true; added_helper_removed=true'
'''.strip())
    (TX / 'restore_release.ps1').write_text(previous, encoding='utf-8')
    (TX / 'ROLLBACK.sh').write_text((ROOT / 'artifacts/m1_pr12b_transaction/ROLLBACK.sh').read_text(encoding='utf-8'), encoding='utf-8', newline='\n')


def build():
    h.build()


def verify():
    original = json.loads((TX / 'baseline_release_hashes.json').read_text(encoding='utf-8'))
    assert all(h.sha(BASELINE / name) == value for name, value in original.items())
    manifest = json.loads((RELEASE / 'file_manifest.json').read_text(encoding='utf-8'))
    for name, entry in manifest['files'].items():
        file = (RELEASE / name).resolve()
        assert file.is_relative_to(RELEASE) and h.sha(file) == entry['sha256'], name
        assert file.stat().st_size == entry['bytes'], name
    actual = {p.relative_to(RELEASE).as_posix() for p in RELEASE.rglob('*') if p.is_file()}
    assert actual == set(manifest['files']) | {'file_manifest.json'}
    assert 'vision/windows_ocr_worker.ps1' in manifest['files']
    assert not (RELEASE / 'vision_worker_fixture.exe').exists()
    for path in (RELEASE, *RELEASE.rglob('*')):
        assert not getattr(path.lstat(), 'st_file_attributes', 0) & 0x400, str(path)
    shutil.copy2(RELEASE / 'RelinkStudio.exe', TX / 'MODIFIED_FILE.exe')
    assert h.sha(TX / 'MODIFIED_FILE.exe') == h.sha(BUILD / 'RelinkStudio.exe') != h.sha(BASELINE / 'RelinkStudio.exe')
    h.ui('MODIFIED', RELEASE, TX / 'snapshots')
    h.run('MODIFIED_STORAGE', [str(RELEASE / 'RelinkStudio.exe'), '--storage-self-test'],
          'Packaged Qt/SQLite only; temporary database; no game.', 'STORAGE_SELF_TEST=PASS')
    for target, marker, args in [
        ('windows_capture_tests', 'WINDOWS_CAPTURE_TESTS=PASS', []),
        ('windows_ocr_tests', 'WINDOWS_OCR_TESTS=PASS', [str(RELEASE / 'vision/windows_ocr_worker.ps1')]),
        ('workspace_tests', 'WORKSPACE_TESTS=PASS', []),
        ('ui_module_tests', 'UI_MODULE_TESTS=PASS', []),
    ]:
        env = h.environment(RELEASE); env['QT_PLUGIN_PATH'] = str(RELEASE)
        h.run(target.upper(), [str(BUILD / (target + '.exe')), *args],
              'Hidden/offscreen synthetic input, packaged dependencies; no game capture.', marker, env=env)
    h.run('STARTUP_CONTRACT', [sys.executable, '-X', 'utf8', str(ROOT / 'tests/business/startup_flow_contract_tests.py')],
          'Static historical evidence contract: 12 tests, not live business execution.', env=os.environ.copy())
    h.run('STARTUP_LOCATORS', [sys.executable, '-X', 'utf8', str(ROOT / 'docs/business_rebuild/scripts/analyze_startup_flow.py'), '--verify-only'],
          'Reopen original historical log locations; compare raw line hashes and message/anchor content.', '"result": "PASS"', env=os.environ.copy())
    from PIL import Image, ImageChops
    visual = {}
    for name in ('tasks.png', 'task_editor.png', 'run.png', 'run_full.png', 'favorites.png', 'prices.png'):
        old = Image.open(TX / 'baseline/ui_snapshots' / name).convert('RGBA')
        new = Image.open(TX / 'snapshots' / name).convert('RGBA')
        visual[name] = old.size == new.size and ImageChops.difference(old, new).getbbox() is None
        assert visual[name], name
    (TX / 'visual_comparison.json').write_text(json.dumps(visual, indent=2), encoding='utf-8')
    additions = {p.relative_to(RELEASE).as_posix(): h.sha(p) for p in RELEASE.rglob('*')
                 if p.is_file() and p.relative_to(RELEASE).as_posix() not in original}
    (TX / 'added_release_files.json').write_text(json.dumps(additions, indent=2), encoding='utf-8')
    prepare_rollback()
    rollback = TX / 'rollback_test'
    shutil.copytree(RELEASE, rollback, dirs_exist_ok=True)
    sentinel = rollback / 'preserve-user-data.txt'
    sentinel.write_text('retain user data\n', encoding='utf-8')
    sentinel_hash = h.sha(sentinel)
    bash = Path(r'C:\Program Files\Git\bin\bash.exe')
    command = 'chmod +x artifacts/m2_live_capture_transaction/ROLLBACK.sh && ./artifacts/m2_live_capture_transaction/ROLLBACK.sh "$1"'
    h.run('ROLLBACK', [str(bash), '-c', command, 'rollback', str(rollback)],
          'Isolated copy of modified package; restore baseline files; remove only new hash-matching managed helper; preserve user sentinel.',
          'ROLLBACK_RESTORED=PASS', runtime=bash.parent)
    assert all(h.sha(rollback / name) == value for name, value in original.items())
    assert all(not (rollback / name).exists() for name in additions)
    assert h.sha(sentinel) == sentinel_hash
    h.ui('RESTORED', rollback, TX / 'restored_snapshots')
    h.run('RESTORED_STORAGE', [str(rollback / 'RelinkStudio.exe'), '--storage-self-test'],
          'Restored original binary/plugins on separate copy; temporary SQLite.', 'STORAGE_SELF_TEST=PASS', runtime=rollback)
    assert h.sha(RELEASE / 'RelinkStudio.exe') == h.sha(TX / 'MODIFIED_FILE.exe')
    print('M2_PACKAGE=PASS; rollback_hash_restored=true; modified_retained=true; six_ui_images_identical=true')


def finalize():
    records = json.loads((TX / 'commands.json').read_text(encoding='utf-8'))
    latest = {r['label']: r for r in records}
    for label in ['BASELINE', 'BASELINE_STORAGE', 'MODIFIED_BUILD', 'MODIFIED', 'MODIFIED_STORAGE',
                  'WINDOWS_CAPTURE_TESTS', 'WINDOWS_OCR_TESTS', 'STARTUP_CONTRACT', 'STARTUP_LOCATORS',
                  'ROLLBACK', 'RESTORED', 'RESTORED_STORAGE']:
        assert latest[label]['exit_status'] == 0, label
    revision = (TX / 'baseline/source_revision.txt').read_text(encoding='ascii').strip()
    names = set(subprocess.check_output(['git', 'diff', '--name-only', revision], text=True, cwd=ROOT).splitlines())
    names.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], text=True, cwd=ROOT).splitlines())
    patch = []
    for name in sorted(names):
        if not (name.startswith(('src/', 'tests/', 'docs/')) or name in {'.gitignore', 'CMakeLists.txt', 'build.ps1', 'README.md'}):
            continue
        before = subprocess.run(['git', 'show', f'{revision}:{name}'], cwd=ROOT, capture_output=True)
        old = before.stdout.decode('utf-8').splitlines(True) if before.returncode == 0 else []
        new = (ROOT / name).read_text(encoding='utf-8').splitlines(True)
        patch.extend(difflib.unified_diff(old, new, fromfile='a/' + name, tofile='b/' + name))
    (TX / 'DIFF_FILE').write_text(''.join(patch), encoding='utf-8')
    lines = [
        'Delta Market Assistant — M2 capture + original startup reconstruction',
        'Date: 2026-10-07 (Asia/Shanghai)', 'Changed branch: main', 'Baseline source revision: ' + revision,
        'Changed fields/boundaries: DxgiObservationSource; TargetWindow/ForegroundReturnGuard; WindowsOcrRecognizer; opt-in --live-capture-check; startup evidence S01-S39.',
        'Business behavior unchanged: normal UI remains replay/configuration; original startup navigation not wired to game.',
        'Real capture passed; Windows OCR tested with synthetic text only, not validated game prices/wear.',
        'User correction: use original page dispatch and watchlist precheck; not ordinary material trading page.',
        'Original sample was never run, imported, patched or packaged.',
        'DELIVERY: ' + str(RELEASE / 'RelinkStudio.exe'),
        'MODIFIED_FILE: ' + str(TX / 'MODIFIED_FILE.exe'),
        'DIFF_FILE: ' + str(TX / 'DIFF_FILE'),
        'VERIFICATION: ' + str(TX / 'VERIFICATION.txt'),
        'ROLLBACK: ' + str(TX / 'ROLLBACK.sh'),
        'BASELINE_SHA256: ' + h.sha(BASELINE / 'RelinkStudio.exe'),
        'MODIFIED_SHA256: ' + h.sha(TX / 'MODIFIED_FILE.exe'),
        'ROLLBACK_SHA256: ' + h.sha(TX / 'rollback_test/RelinkStudio.exe'),
        'Restored behavior/status: isolated original UI/workspace/SQLite checks pass; user sentinel preserved; new helper removed.',
        'Fixed delivery and MODIFIED_FILE remain changed. Runtime image-file writes=0; UI test snapshots are explicitly requested diagnostics.',
        'Live round metadata is in live_capture_1.json; original failed focus and successful retry metadata remain separate.',
        'Exact command records (including all recorded attempts):',
    ]
    records.append(json.loads((TX / 'live_capture_1.json').read_text(encoding='utf-8')))
    for record in records:
        lines += ['', record['label'], 'COMMAND: ' + record['command'], 'INPUT: ' + record['input'],
                  'STDOUT:', record['stdout'].rstrip(), 'STDERR:', record['stderr'].rstrip() or '(empty)',
                  'EXIT_STATUS: ' + str(record['exit_status'])]
    (TX / 'VERIFICATION.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for name in ['MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh']:
        assert (TX / name).read_bytes()
        print('REOPEN=PASS; path=' + str(TX / name))
    assert h.sha(RELEASE / 'RelinkStudio.exe') == h.sha(TX / 'MODIFIED_FILE.exe')
    print('M2_TRANSACTION=PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=['build', 'verify', 'finalize'])
    args = parser.parse_args()
    globals()[args.phase]()
