"""Offline full-cycle package transaction and file-backed live evidence review.

This tool never builds, captures the game, sends input, or reruns the baseline.
The frozen baseline is read-only. A package pass is distinct from business
completion; unfinished/missing live runs remain explicitly unfinished.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import uuid

import verify_pr12a_package as h

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/collection_full_cycle'
RELEASE = ROOT / 'dist/RelinkStudio'
BASELINE = TX / 'baseline/release'
SOURCE_ROOTS = ('src', 'tests', 'docs')
SOURCE_FILES = ('.gitignore', 'CMakeLists.txt', 'build.ps1', 'SESSION_START.md', 'README.md', 'resources.qrc')
IGNORED_SOURCE_SUFFIXES = {'.pyc', '.exe', '.dll', '.png', '.jpg', '.jpeg', '.zip', '.onnx'}
REQUIRED = ('BASELINE', 'BASELINE_STORAGE', 'MODIFIED', 'MODIFIED_STORAGE',
            'WINDOWS_CAPTURE_TESTS', 'WINDOWS_OCR_TESTS', 'WORKSPACE_TESTS', 'UI_MODULE_TESTS',
            'MODIFIED_LAYOUT_TESTS', 'MODIFIED_COLLECTION_TESTS', 'MODIFIED_BATCH_TESTS',
            'MODIFIED_CURSOR_TESTS', 'ROLLBACK', 'RESTORED', 'RESTORED_STORAGE')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def confined(root, name):
    root = Path(root).resolve()
    path = (root / name).resolve()
    if path == root or not path.is_relative_to(root):
        raise ValueError('PATH_OUTSIDE_RECORDED_ROOT: ' + str(name))
    cursor = path
    while cursor != root.parent:
        if cursor.exists() and (cursor.is_symlink() or getattr(cursor.lstat(), 'st_file_attributes', 0) & 0x400):
            raise ValueError('REPARSE_POINT: ' + str(cursor))
        cursor = cursor.parent
    return path


def check_frozen(root=ROOT, tx=TX):
    for name, directory in (('baseline_release_hashes.json', 'baseline/release'),
                            ('baseline_source_hashes.json', 'baseline/source')):
        hashes = read_json(tx / name)
        if not isinstance(hashes, dict) or not hashes:
            raise ValueError('EMPTY_BASELINE_MANIFEST: ' + name)
        for relative, expected in hashes.items():
            path = confined(tx / directory, relative)
            if not path.is_file() or sha(path) != expected:
                raise ValueError('FROZEN_BASELINE_CHANGED: ' + relative)
    return read_json(tx / 'baseline_source_hashes.json')


def source_diff(root=ROOT, tx=TX):
    original = check_frozen(root, tx)
    names = set(original)
    for directory in SOURCE_ROOTS:
        names.update(p.relative_to(root).as_posix() for p in (root / directory).rglob('*')
                     if p.is_file() and '__pycache__' not in p.parts and p.suffix.lower() not in IGNORED_SOURCE_SUFFIXES)
    names.update(name for name in SOURCE_FILES if (root / name).is_file())
    changes, patch = [], []
    for relative in sorted(names):
        current = confined(root, relative)
        old = confined(tx / 'baseline/source', relative)
        before = old.read_bytes() if relative in original else b''
        after = current.read_bytes() if current.is_file() else b''
        if before == after:
            continue
        entry = dict(path=relative, baseline_sha256=hashlib.sha256(before).hexdigest() if old.is_file() else None,
                     modified_sha256=hashlib.sha256(after).hexdigest() if current.is_file() else None)
        try:
            a, b = before.decode('utf-8-sig'), after.decode('utf-8-sig')
            if '\x00' in a or '\x00' in b:
                raise UnicodeError('binary content')
            delta = list(difflib.unified_diff(a.splitlines(True), b.splitlines(True),
                                            fromfile='a/' + relative, tofile='b/' + relative))
            if not delta:
                delta = [f'Byte-only encoding/newline change: {relative}\n']
            patch.extend(delta)
            if patch and not patch[-1].endswith('\n'):
                patch[-1] += '\n\\ No newline at end of file\n'
            entry['kind'] = 'text'
        except UnicodeError:
            entry['kind'] = 'binary'
            patch.append(f'Binary files a/{relative} and b/{relative} differ\n'
                         f'baseline_sha256={entry["baseline_sha256"]}\nmodified_sha256={entry["modified_sha256"]}\n')
        changes.append(entry)
    return ''.join(patch), changes


def _attempt_keys(session, session_path):
    keys = set()
    for compact in session.get('steps', []):
        step = compact
        if compact.get('step_record'):
            path = Path(compact['step_record']).resolve()
            if not path.is_relative_to(session_path.parent.resolve()):
                raise ValueError('STEP_RECORD_OUTSIDE_RUN')
            step = read_json(path)
        candidate = step.get('collection_attempt')
        if candidate:
            identity = {key: candidate[key] for key in ('product', 'condition', 'wear')}
            keys.add(hashlib.sha256(json.dumps(identity, sort_keys=True, ensure_ascii=False).encode()).hexdigest())
    return keys


def probe_review(paths):
    """Keep failed real probes as failures, without treating them as collection."""
    results, errors = [], []
    for path in sorted(set(Path(p).resolve() for p in paths)):
        value = read_json(path)
        session_path = path.parent / 'session.json'
        session = read_json(session_path) if session_path.is_file() else {}
        invocation_path = path.parent / 'invocation.json'
        invocation = read_json(invocation_path) if invocation_path.is_file() else None
        captures, issues = [], []
        for compact in session.get('steps', []):
            step = compact
            if compact.get('step_record'):
                record = Path(compact['step_record']).resolve()
                if not record.is_relative_to(path.parent):
                    issues.append('PROBE_STEP_OUTSIDE_DIRECTORY')
                    continue
                step = read_json(record)
            attempts = step.get('capture_attempts') or step.get('attempts') or ([step] if step.get('kind') == 'capture' else [])
            for attempt in attempts:
                packet = attempt.get('result', {})
                captures.append(dict(command=packet.get('probe_actual_command', attempt.get('command')),
                    coordinator_requested_command=attempt.get('command'), exit_status=attempt.get('exit_status'),
                    capture_passed=packet.get('capture_passed'), error=packet.get('error'),
                    recognition_performed=packet.get('recognition_performed'), image_file_writes=packet.get('image_file_writes'),
                    game_input_sent=packet.get('game_input_sent'), stderr=attempt.get('stderr', '')))
                if packet.get('image_file_writes', 0) != 0:
                    issues.append('PROBE_GAME_IMAGE_FILE_WRITES')
        if not session:
            issues.append('PROBE_SESSION_NOT_AVAILABLE')
        elif (session.get('enter_calls') != 1 or session.get('leave_calls') != 1
              or session.get('ide_restored') is not True or value.get('ide_restored') is not True):
            issues.append('PROBE_FOREGROUND_NOT_RESTORED')
        if value.get('image_file_writes') != 0 or session.get('image_file_writes') != 0:
            issues.append('PROBE_GAME_IMAGE_FILE_WRITES')
        results.append(dict(path=str(path), sha256=sha(path), session_path=str(session_path),
            session_sha256=sha(session_path) if session else None, error=value.get('error'),
            ide_restored=value.get('ide_restored'), image_file_writes=value.get('image_file_writes'),
            runner_command=session.get('runner_command'), exit_status=session.get('exit_status'),
            session_exit_status_semantics='coordinator recorded outcome, not necessarily the outer Python process exit',
            actual_invocation=invocation,
            invocation_sha256=sha(invocation_path) if invocation else None,
            session_passed=session.get('passed'), capture_attempts=captures,
            reading_count=len(value.get('readings', [])), issues=issues,
            collection_completion_evidence=False))
        errors.extend(str(path) + ': ' + issue for issue in issues)
    return results, errors


def live_review(summary_paths, snapshot_path, journal_path, probe_paths=()):
    """Reopen evidence; never infer a finished rule or watchlist size from counts."""
    snapshot_path, journal_path = Path(snapshot_path), Path(journal_path)
    snapshot = read_json(snapshot_path) if snapshot_path.is_file() else None
    snapshot_sha = sha(snapshot_path) if snapshot is not None else None
    runs, keys, errors = [], set(), []
    for path in sorted(set(Path(p).resolve() for p in summary_paths), key=lambda p: (p.stat().st_mtime_ns, str(p))):
        summary = read_json(path)
        issues = []
        completed = summary.get('rows', [])
        enabled = summary.get('enabled_row_indices', [])
        if not isinstance(completed, list) or not isinstance(enabled, list):
            issues.append('INVALID_RULE_ARRAYS')
            completed, enabled = [], []
        if summary.get('phase') != 'collection_only' or summary.get('purchase_phase_started') is not False:
            issues.append('NOT_COLLECTION_ONLY')
        if summary.get('exhaustive_market_scan') is not False:
            issues.append('UNSUPPORTED_EXHAUSTIVE_MARKET_CLAIM')
        if summary.get('image_file_writes', 0) != 0:
            issues.append('GAME_IMAGE_FILE_WRITES')
        if [entry.get('row_index') for entry in completed] != enabled[:len(completed)]:
            issues.append('COMPLETED_ROWS_NOT_ENABLED_PREFIX')
        if summary.get('task_file_fully_completed') is True and (
                len(completed) != len(enabled) or not enabled or summary.get('error')
                or summary.get('ide_restored') is not True or summary.get('segment_finished') is not True
                or summary.get('pending_collection') is not None or summary.get('pending_geometry') is not None):
            issues.append('FALSE_OR_PENDING_COMPLETION')
        if snapshot is None:
            issues.append('SNAPSHOT_NOT_AVAILABLE')
        else:
            # Import a pure validator only. The module's live entrypoint is not invoked.
            manual = str(ROOT / 'tests/manual')
            if manual not in sys.path:
                sys.path.insert(0, manual)
            from run_collection_trial import validate_resume
            try:
                validate_resume(snapshot, snapshot_sha, summary)
            except (ValueError, KeyError, TypeError) as error:
                issues.append('RULE_EVIDENCE: ' + str(error))
        session_path = Path(summary.get('session_record') or path.parent / 'session.json').resolve()
        session = None
        if not session_path.is_relative_to(path.parent.resolve()):
            issues.append('SESSION_RECORD_OUTSIDE_RUN')
        elif session_path.is_file():
            session = read_json(session_path)
            if session.get('image_file_writes') != 0:
                issues.append('SESSION_GAME_IMAGE_FILE_WRITES')
            if summary.get('segment_finished') is True:
                if session.get('enter_calls') != session.get('leave_calls') or session.get('enter_calls') != 1:
                    issues.append('FOREGROUND_SESSION_NOT_PAIRED')
                if summary.get('ide_restored') is not True or session.get('ide_restored') is not True:
                    issues.append('IDE_NOT_RESTORED')
            try:
                keys.update(_attempt_keys(session, session_path))
            except (ValueError, KeyError, OSError) as error:
                issues.append('ATTEMPT_EVIDENCE: ' + str(error))
        else:
            issues.append('SESSION_RECORD_NOT_AVAILABLE')
        runs.append(dict(path=str(path), sha256=sha(path), summary=summary,
                         session_path=str(session_path), session_sha256=sha(session_path) if session else None,
                         completed_rule_count=len(completed), enabled_rule_count=len(enabled), issues=issues))
        errors.extend(str(path) + ': ' + issue for issue in issues)
    journal = {}
    for path in sorted(journal_path.glob('*.json')):
        value = read_json(path)
        key = value.get('key')
        if not key or key in journal:
            errors.append('JOURNAL_IDENTITY_OR_DUPLICATE: ' + str(path))
        else:
            journal[key] = value
    attributed = [journal[key] for key in sorted(keys) if key in journal]
    absent = sorted(keys - set(journal))
    if absent:
        errors.append('ATTEMPT_JOURNAL_MISSING: ' + ','.join(absent))
    confirmed_status = {'confirmed', 'confirmed_reconciliation'}
    pending = [key for key, value in journal.items() if value.get('status') not in confirmed_status]
    confirmed = [value for value in attributed if value.get('status') in confirmed_status]
    independent = [value for value in confirmed if value.get('reconciliation', {}).get('automatic_price_validation_passed') is False]
    probes, probe_errors = probe_review(probe_paths)
    errors.extend(probe_errors)
    latest = runs[-1] if runs else None
    complete = bool(latest and not errors and not pending and latest['summary'].get('task_file_fully_completed') is True)
    status = 'completed' if complete else ('incomplete' if runs else
        ('blocked_before_collection' if any(p['error'] for p in probes) else ('probe_only' if probes else 'not_started')))
    return dict(schema='collection-full-cycle-review-v1', evidence_valid=not errors, errors=errors,
                task_file_fully_completed=complete, business_status=status,
                completion_semantics='configured_rule_price_boundary; unobserved tail is not exhaustive',
                exhaustive_market_scan=False, latest_selection_basis='latest summary file modification time, path tie-break',
                latest_summary=str(latest['path']) if latest else None, runs=runs,
                snapshot_path=str(snapshot_path), snapshot_sha256=snapshot_sha,
                configured_enabled_rules=sum(row.get('enabled') is True for row in snapshot.get('rows', [])) if snapshot else None,
                recorded_run_count=len(runs), probes=probes, recorded_probe_count=len(probes), series_unique_star_attempts=len(keys),
                series_confirmed_actions=len(confirmed), series_independent_visual_confirmations=len(independent),
                series_automatic_confirmations=len(confirmed) - len(independent),
                historical_journal_records=len(journal), historical_confirmed_actions=sum(v.get('status') in confirmed_status for v in journal.values()),
                historical_count_is_not_current_watchlist_size=True, current_watchlist_size_known=False,
                pending_journal_keys=pending, pending_journal=len(pending))


def collect_review(args):
    paths = args.run_summary or sorted(args.runs_root.glob('run_*/summary.json'))
    probes = args.probe_result or sorted(args.runs_root.glob('*probe_*/result.json'))
    return live_review(paths, args.snapshot, args.journal, probes)


def prepare_rollback(current_hashes):
    original = read_json(TX / 'baseline_release_hashes.json')
    additions = {name: digest for name, digest in current_hashes.items() if name not in original}
    write_json(TX / 'modified_release_hashes.json', current_hashes)
    write_json(TX / 'added_release_files.json', additions)
    # Preflight every source/destination/addition before changing any file.
    # File operations remain entirely in PowerShell with LiteralPath.
    script = r'''param([Parameter(Mandatory=$true)][string]$Target)
$ErrorActionPreference = 'Stop'
$Source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'baseline\release')).TrimEnd('\')
$Destination = [IO.Path]::GetFullPath($Target).TrimEnd('\')
$Release = '__RELEASE__'
$Allowed = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'rollback_target.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Destination -ne $Release -and $Destination -ne $Allowed.path) { throw 'Unrecorded rollback target.' }
if (!(Test-Path -LiteralPath $Destination -PathType Container)) { throw 'Target must exist.' }
function Assert-NoReparse([string]$Path) {
    $cursor = $Path
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse point: $cursor" }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Child([string]$Root, [string]$Name) {
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Name.Replace('/', '\')))
    if (!$path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escaped root.' }
    Assert-NoReparse $path
    return $path
}
Assert-NoReparse $Source
Assert-NoReparse $Destination
$Original = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'baseline_release_hashes.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Modified = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'modified_release_hashes.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Added = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'added_release_files.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Entries = @()
foreach ($entry in $Modified.PSObject.Properties) {
    $path = Child $Destination $entry.Name
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw "Modified file changed: $($entry.Name)" }
}
foreach ($entry in $Original.PSObject.Properties) {
    $from = Child $Source $entry.Name
    $to = Child $Destination $entry.Name
    if ((Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw "Baseline hash mismatch: $($entry.Name)" }
    if (!$Modified.PSObject.Properties[$entry.Name] -and (Test-Path -LiteralPath $to)) { throw "Unmanaged destination collision: $($entry.Name)" }
    $Entries += [PSCustomObject]@{Source=$from; Destination=$to; Hash=$entry.Value}
}
foreach ($entry in $Entries) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($entry.Destination)) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Source -Destination $entry.Destination -Force
    if ((Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Hash) { throw 'Restored hash mismatch.' }
}
foreach ($entry in $Added.PSObject.Properties) {
    $path = Child $Destination $entry.Name
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw 'Added file changed; preserve it.' }
    Remove-Item -LiteralPath $path -Force
}
Write-Output 'ROLLBACK_RESTORED=PASS; baseline_hashes_restored=true; unmanaged_user_data_preserved=true; added_managed_files_removed=true'
'''.replace('__RELEASE__', str(RELEASE).replace("'", "''"))
    (TX / 'restore_release.ps1').write_text(script, encoding='utf-8')
    (TX / 'ROLLBACK.sh').write_text('''#!/usr/bin/env bash
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -ne 1 ]]; then echo "Usage: ROLLBACK.sh <recorded-target-directory>" >&2; exit 2; fi
powershell="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}\\System32\\WindowsPowerShell\\v1.0\\powershell.exe")"
"$powershell" -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/restore_release.ps1")" -Target "$1"
''', encoding='utf-8', newline='\n')


def verify(args):
    check_frozen()
    h.TX, h.BASELINE, h.RELEASE = TX, BASELINE, RELEASE
    baseline_records = {r['label']: r for r in read_json(TX / 'commands.json')}
    for label in ('BASELINE', 'BASELINE_STORAGE'):
        if baseline_records.get(label, {}).get('exit_status') != 0:
            raise ValueError('PRESERVED_BASELINE_RESULT_REQUIRED: ' + label)
    manifest = read_json(RELEASE / 'file_manifest.json')['files']
    actual = {p.relative_to(RELEASE).as_posix() for p in RELEASE.rglob('*') if p.is_file()}
    if actual != set(manifest) | {'file_manifest.json'}:
        raise ValueError('PACKAGE_FILE_SET_MISMATCH')
    for name, entry in manifest.items():
        path = confined(RELEASE, name)
        if sha(path) != entry['sha256'] or path.stat().st_size != entry['bytes']:
            raise ValueError('PACKAGE_FILE_MISMATCH: ' + name)
    for name in ('Qt6Sql.dll', 'sqldrivers/qsqlite.dll', 'platforms/qwindows.dll', 'platforms/qoffscreen.dll', 'vision/windows_ocr_worker.ps1'):
        if name not in manifest:
            raise ValueError('MISSING_PACKAGED_DEPENDENCY: ' + name)
    if sha(RELEASE / 'RelinkStudio.exe') != sha(args.build_dir / 'RelinkStudio.exe'):
        raise ValueError('BUILD_AND_PACKAGE_EXECUTABLE_DIFFER')
    if sha(RELEASE / 'RelinkStudio.exe') == sha(BASELINE / 'RelinkStudio.exe'):
        raise ValueError('PACKAGE_EXECUTABLE_UNCHANGED')
    shutil.copy2(RELEASE / 'RelinkStudio.exe', TX / 'MODIFIED_FILE.exe')
    current_hashes = {name: sha(RELEASE / name) for name in sorted(actual)}
    h.ui('MODIFIED', RELEASE, TX / 'snapshots')
    h.run('MODIFIED_STORAGE', [str(RELEASE / 'RelinkStudio.exe'), '--storage-self-test'],
          'Modified unpacked package; temporary SQLite; no game.', 'STORAGE_SELF_TEST=PASS')
    for target, marker, extra in [
        ('windows_capture_tests', 'WINDOWS_CAPTURE_TESTS=PASS', []),
        ('windows_ocr_tests', 'WINDOWS_OCR_TESTS=PASS', [str(RELEASE / 'vision/windows_ocr_worker.ps1')]),
        ('workspace_tests', 'WORKSPACE_TESTS=PASS', []), ('ui_module_tests', 'UI_MODULE_TESTS=PASS', []),
        ('collection_layout_tests', 'COLLECTION_LAYOUT_TESTS=PASS', []),
    ]:
        env = h.environment(RELEASE)
        env['QT_PLUGIN_PATH'] = str(RELEASE)
        label = 'MODIFIED_LAYOUT_TESTS' if target == 'collection_layout_tests' else target.upper()
        h.run(label, [str(args.build_dir / (target + '.exe')), *extra],
              'Synthetic/temporary input; packaged dependencies; offscreen; no game capture or input.', marker, env=env)
    for label, pattern in [('MODIFIED_COLLECTION_TESTS', 'test_collection*.py'),
                           ('MODIFIED_BATCH_TESTS', 'test_foreground*.py'),
                           ('MODIFIED_CURSOR_TESTS', 'test_cursor_motion.py')]:
        h.run(label, [sys.executable, '-X', 'utf8', '-m', 'unittest', 'discover', '-s', 'tests/manual', '-p', pattern],
              'Offline mocks and recorded/synthetic packets only; no real OS input.', env=os.environ.copy())
    rollback = TX / ('rollback_test' if not (TX / 'rollback_test').exists() else 'rollback_test_' + uuid.uuid4().hex[:10])
    shutil.copytree(RELEASE, rollback)
    sentinel = rollback / 'preserve-user-data.txt'
    sentinel.write_text('retain user data\n', encoding='utf-8')
    database = rollback / 'preserve-new-ledger.sqlite'
    with sqlite3.connect(database) as connection:
        connection.execute('CREATE TABLE sentinel (value TEXT NOT NULL)')
        connection.execute('INSERT INTO sentinel VALUES (?)', ('preserve new ledger',))
    saved = {str(path): sha(path) for path in (sentinel, database)}
    write_json(TX / 'rollback_target.json', dict(path=str(rollback)))
    prepare_rollback(current_hashes)
    bash = Path(r'C:\Program Files\Git\bin\bash.exe')
    script = (TX / 'ROLLBACK.sh').relative_to(ROOT).as_posix()
    h.run('ROLLBACK', [str(bash), '-c', f'chmod +x "{script}" && "./{script}" "$1"', 'rollback', str(rollback)],
          'Isolated modified-package copy; original managed files restored; text/SQLite user sentinels retained.',
          'ROLLBACK_RESTORED=PASS', runtime=bash.parent)
    original = read_json(TX / 'baseline_release_hashes.json')
    if any(sha(rollback / name) != digest for name, digest in original.items()):
        raise ValueError('RESTORED_PACKAGE_HASH_MISMATCH')
    if any((rollback / name).exists() for name in current_hashes if name not in original):
        raise ValueError('ADDED_MANAGED_FILE_REMAINS')
    if any(sha(Path(path)) != digest for path, digest in saved.items()):
        raise ValueError('USER_SENTINEL_CHANGED')
    h.ui('RESTORED', rollback, TX / 'restored_snapshots')
    h.run('RESTORED_STORAGE', [str(rollback / 'RelinkStudio.exe'), '--storage-self-test'],
          'Restored original package on isolated copy; temporary SQLite; no game.', 'STORAGE_SELF_TEST=PASS', runtime=rollback)
    check_frozen()
    if sha(RELEASE / 'RelinkStudio.exe') != sha(TX / 'MODIFIED_FILE.exe'):
        raise ValueError('MODIFIED_PACKAGE_NOT_RETAINED')
    write_json(TX / 'package_verification.json', dict(package_verified=True, rollback_target=str(rollback),
               release_exe_sha256=sha(RELEASE / 'RelinkStudio.exe'), modified_exe_sha256=sha(TX / 'MODIFIED_FILE.exe'),
               baseline_exe_sha256=sha(BASELINE / 'RelinkStudio.exe'), rollback_exe_sha256=sha(rollback / 'RelinkStudio.exe'),
               sentinels=saved, baseline_read_only=True, game_actions=0, build_invoked=False))
    print('COLLECTION_FULL_PACKAGE=PASS; isolated_rollback=true; baseline_read_only=true; live_business_not_asserted=true')


def review(args):
    value = collect_review(args)
    write_json(TX / 'execution_review.json', value)
    print('COLLECTION_FULL_EVIDENCE=' + ('PASS' if value['evidence_valid'] else 'FAIL')
          + '; business_status=' + value['business_status'] + '; runs=' + str(value['recorded_run_count'])
          + '; task_file_fully_completed=' + str(value['task_file_fully_completed']).lower())
    return 0 if value['evidence_valid'] else 1


def finalize(args):
    check_frozen()
    records = read_json(TX / 'commands.json')
    latest = {r['label']: r for r in records}
    for label in REQUIRED:
        if latest.get(label, {}).get('exit_status') != 0:
            raise ValueError('REQUIRED_VERIFICATION_NOT_PASSED: ' + label)
    package = read_json(TX / 'package_verification.json')
    rollback = Path(package['rollback_target'])
    if package.get('package_verified') is not True or sha(rollback / 'RelinkStudio.exe') != sha(BASELINE / 'RelinkStudio.exe'):
        raise ValueError('ROLLBACK_NOT_VERIFIED')
    if sha(RELEASE / 'RelinkStudio.exe') != sha(TX / 'MODIFIED_FILE.exe') or sha(TX / 'MODIFIED_FILE.exe') != package['modified_exe_sha256']:
        raise ValueError('PACKAGE_CHANGED_SINCE_VERIFY')
    for name, digest in read_json(TX / 'modified_release_hashes.json').items():
        if sha(confined(RELEASE, name)) != digest:
            raise ValueError('PACKAGE_DEPENDENCY_CHANGED_SINCE_VERIFY: ' + name)
    evidence = collect_review(args)
    write_json(TX / 'execution_review.json', evidence)
    if not evidence['evidence_valid']:
        raise ValueError('LIVE_EVIDENCE_INCONSISTENT: ' + '; '.join(evidence['errors']))
    if args.require_business_complete and not evidence['task_file_fully_completed']:
        raise ValueError('LIVE_BUSINESS_NOT_COMPLETE: ' + evidence['business_status'])
    patch, changed = source_diff()
    (TX / 'DIFF_FILE').write_text(patch or 'No source byte changes relative to frozen baseline.\n', encoding='utf-8')
    write_json(TX / 'source_changes.json', changed)
    branch = subprocess.run(['git', 'branch', '--show-current'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    fields = ('collection_numeric_price; local_title_ocr/local_catalog_ocr; '
              'collection_listing_filter_refinement; catalog_filter_season_refinement; '
              'collection_layout measured row/tail signals; card_geometry_match; '
              'rebind_after_scroll; same-snapshot resumable rule collection')
    lines = ['Delta Market Assistant - collection full-cycle transaction', 'Recorded at: ' + datetime.now().astimezone().isoformat(),
             'Changed branch: ' + (branch or '(detached)'), 'Changed fields: ' + fields,
             'Source comparison: preserved baseline/source originals and their frozen hashes, not git HEAD.',
             'PACKAGE_VERIFIED=true', 'BUSINESS_STATUS=' + evidence['business_status'],
             'TASK_FILE_FULLY_COMPLETED=' + str(evidence['task_file_fully_completed']).lower(),
             'EXHAUSTIVE_MARKET_SCAN=false; completion means configured rule price boundaries, never unseen market-tail coverage.',
             'Historical confirmed actions are not the current watchlist size.',
             'LIVE_ENTRY=explicit Python CLI --numeric-price --local-title-ocr; frontend Run button is not connected to this live workflow.',
             'Restored behavior/status: original package hashes and offscreen UI/workspace/SQLite passed on an isolated copy; user text/SQLite sentinels preserved.',
             'Rollback restores program files, not game favorites. Fixed unpacked release remains modified.',
             'DELIVERY: ' + str(RELEASE / 'RelinkStudio.exe'), 'BASELINE_SHA256: ' + sha(BASELINE / 'RelinkStudio.exe'),
             'MODIFIED_SHA256: ' + sha(TX / 'MODIFIED_FILE.exe'), 'ROLLBACK_SHA256: ' + sha(rollback / 'RelinkStudio.exe')]
    lines += [name + ': ' + str(TX / name) for name in ('MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh')]
    lines += ['', 'SOURCE_CHANGES:', json.dumps(changed, ensure_ascii=False, indent=2),
              '', 'LIVE_FILE_EVIDENCE:', json.dumps(evidence, ensure_ascii=False, indent=2),
              '', 'EXACT_COMMANDS_INPUTS_LITERAL_OUTPUT_AND_EXIT_STATUS:']
    for record in records:
        lines += ['', record['label'], 'COMMAND: ' + record['command'], 'INPUT: ' + record['input'],
                  'STDOUT:', record.get('stdout', '').rstrip() or '(empty)', 'STDERR:', record.get('stderr', '').rstrip() or '(empty)',
                  'EXIT_STATUS: ' + str(record['exit_status'])]
    (TX / 'VERIFICATION.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for name in ('MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh'):
        path = TX / name
        if not path.read_bytes():
            raise ValueError('EMPTY_TRANSACTION_ARTIFACT: ' + name)
        print('REOPEN=PASS; path=' + str(path) + '; sha256=' + sha(path))
    print('COLLECTION_FULL_TRANSACTION=PASS; package_verified=true; business_status=' + evidence['business_status'])


def self_test(_args):
    class ToolTests(unittest.TestCase):
        def test_no_runs_is_not_completion(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                result = live_review([], path / 'missing.json', path / 'journal')
                self.assertTrue(result['evidence_valid'])
                self.assertFalse(result['task_file_fully_completed'])
                self.assertEqual(result['business_status'], 'not_started')
                self.assertEqual(result['historical_journal_records'], 0)

        def test_journal_counts_are_data_not_watchlist_size(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                for i in range(7):
                    write_json(path / f'{i}.json', dict(key=str(i), status='confirmed' if i < 6 else 'dispatched'))
                result = live_review([], path / 'missing', path)
                self.assertEqual(result['historical_confirmed_actions'], 6)
                self.assertEqual(result['pending_journal'], 1)
                self.assertFalse(result['current_watchlist_size_known'])
                self.assertEqual(result['series_confirmed_actions'], 0)

        def test_false_completion_is_not_accepted(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                write_json(path / 'summary.json', dict(task_file_fully_completed=True, phase='collection_only',
                    purchase_phase_started=False, exhaustive_market_scan=False, rows=[], enabled_row_indices=[9]))
                result = live_review([path / 'summary.json'], path / 'missing', path / 'journal')
                self.assertFalse(result['evidence_valid'])
                self.assertFalse(result['task_file_fully_completed'])
                self.assertIn('FALSE_OR_PENDING_COMPLETION', result['runs'][0]['issues'])

        def test_failed_probe_is_real_evidence_not_completed_collection(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                write_json(path / 'result.json', dict(error='BATCH_STEP_FAILED', ide_restored=True, image_file_writes=0, readings=[]))
                write_json(path / 'session.json', dict(passed=False, enter_calls=1, leave_calls=1,
                    ide_restored=True, image_file_writes=0, exit_status=1, runner_command=['synthetic-probe'],
                    steps=[dict(kind='capture', capture_attempts=[dict(command=['synthetic-capture'], exit_status=1,
                        result=dict(error='E_WINDOW_OCCLUDED', capture_passed=False, game_input_sent=False, image_file_writes=0))])]))
                result = live_review([], path / 'missing', path / 'journal', [path / 'result.json'])
                self.assertTrue(result['evidence_valid'])
                self.assertFalse(result['task_file_fully_completed'])
                self.assertEqual(result['business_status'], 'blocked_before_collection')
                self.assertEqual(result['recorded_run_count'], 0)
                self.assertEqual(result['probes'][0]['capture_attempts'][0]['error'], 'E_WINDOW_OCCLUDED')
                self.assertEqual(result['probes'][0]['exit_status'], 1)

        def test_completed_rule_is_bound_to_snapshot_and_observed_boundary(self):
            manual = str(ROOT / 'tests/manual')
            if manual not in sys.path:
                sys.path.insert(0, manual)
            from run_collection_trial import progress_binding, rule_fingerprint
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                row = dict(row_index=17, enabled=True, task_id='synthetic:17', product_name='Synthetic Product',
                           condition_label='S', price_max='10')
                snapshot = dict(source_sha256='a' * 64, config_sha256='a' * 64, rows=[row])
                write_json(path / 'snapshot.json', snapshot)
                boundary = dict(product=row['product_name'], condition='S', row_index=17, price='11', wear='0.1',
                                source_frame_id='synthetic:boundary', source_frame_sha256='b' * 64)
                counters = dict(scanned_candidates=0, confirmed_new=0, already_favorited=0, unmatched=0)
                summary = dict(schema='collection-rule-progress-v1', phase='collection_only', purchase_phase_started=False,
                    exhaustive_market_scan=False, rows=[dict(row_index=17, task_id=row['task_id'],
                        rule_fingerprint=rule_fingerprint(row), status='original_price_stop_boundary',
                        unobserved_tail_exhaustive=False, visible_windows=1, boundary_candidate=boundary)],
                    completed_row_indices=[17], next_row_index=None, task_file_fully_completed=True,
                    segment_index=1, segment_finished=True, pending_collection=None, pending_geometry=None,
                    ide_restored=True, image_file_writes=0, cycle_totals=counters, **counters,
                    **progress_binding(snapshot, sha(path / 'snapshot.json')))
                write_json(path / 'session.json', dict(steps=[], enter_calls=1, leave_calls=1, ide_restored=True, image_file_writes=0))
                write_json(path / 'summary.json', summary)
                result = live_review([path / 'summary.json'], path / 'snapshot.json', path / 'journal')
                self.assertTrue(result['evidence_valid'], result['errors'])
                self.assertTrue(result['task_file_fully_completed'])
                self.assertFalse(result['exhaustive_market_scan'])
                summary['rows'][0]['boundary_candidate']['price'] = '9'
                write_json(path / 'summary.json', summary)
                invalid = live_review([path / 'summary.json'], path / 'snapshot.json', path / 'journal')
                self.assertFalse(invalid['evidence_valid'])
                self.assertFalse(invalid['task_file_fully_completed'])

        def test_source_diff_uses_frozen_bytes_including_binary(self):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory); tx = root / 'tx'
                for folder in ('baseline/source/src', 'baseline/release'):
                    (tx / folder).mkdir(parents=True)
                (root / 'src').mkdir()
                (tx / 'baseline/source/src/a.cpp').write_text('original\n', encoding='utf-8')
                (tx / 'baseline/source/src/b.ico').write_bytes(b'\x00old')
                (tx / 'baseline/release/app.exe').write_bytes(b'preserved fake package; never executed')
                write_json(tx / 'baseline_release_hashes.json', {'app.exe': sha(tx / 'baseline/release/app.exe')})
                write_json(tx / 'baseline_source_hashes.json', {n: sha(tx / 'baseline/source' / n) for n in ('src/a.cpp', 'src/b.ico')})
                (root / 'src/a.cpp').write_text('modified\n', encoding='utf-8')
                (root / 'src/b.ico').write_bytes(b'\x00new')
                patch, changes = source_diff(root, tx)
                self.assertIn('-original', patch); self.assertIn('+modified', patch)
                self.assertIn('Binary files', patch)
                self.assertEqual(len(changes), 2)
                (tx / 'baseline/source/src/a.cpp').write_text('tampered\n', encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'FROZEN_BASELINE_CHANGED'):
                    source_diff(root, tx)

        @unittest.skipUnless(os.name == 'nt', 'PowerShell rollback runs on Windows')
        def test_generated_rollback_preflights_and_preserves_unmanaged_data(self):
            global TX, RELEASE
            original_tx, original_release = TX, RELEASE
            try:
                with tempfile.TemporaryDirectory() as directory:
                    TX = Path(directory) / 'transaction'
                    RELEASE = Path(directory) / 'release'
                    source = TX / 'baseline/release'
                    target = TX / 'isolated-copy'
                    source.mkdir(parents=True); target.mkdir(parents=True)
                    (source / 'app.bin').write_bytes(b'original synthetic file; not executable')
                    (target / 'app.bin').write_bytes(b'modified synthetic file; not executable')
                    (target / 'new-helper.bin').write_bytes(b'new synthetic helper; not executable')
                    (target / 'user.txt').write_bytes(b'preserve me')
                    write_json(TX / 'baseline_release_hashes.json', {'app.bin': sha(source / 'app.bin')})
                    write_json(TX / 'rollback_target.json', dict(path=str(target)))
                    current = {name: sha(target / name) for name in ('app.bin', 'new-helper.bin')}
                    prepare_rollback(current)
                    command = [str(Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'),
                               '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(TX / 'restore_release.ps1'), '-Target', str(target)]
                    (target / 'new-helper.bin').write_bytes(b'unexpected later change')
                    blocked = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', creationflags=subprocess.CREATE_NO_WINDOW)
                    self.assertNotEqual(blocked.returncode, 0)
                    self.assertIn('Modified file changed', blocked.stderr)
                    self.assertEqual(sha(target / 'app.bin'), current['app.bin'], 'preflight must finish before any restore')
                    (target / 'new-helper.bin').write_bytes(b'new synthetic helper; not executable')
                    restored = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', creationflags=subprocess.CREATE_NO_WINDOW)
                    self.assertEqual(restored.returncode, 0, restored.stderr)
                    self.assertIn('ROLLBACK_RESTORED=PASS', restored.stdout)
                    self.assertEqual(sha(target / 'app.bin'), sha(source / 'app.bin'))
                    self.assertFalse((target / 'new-helper.bin').exists())
                    self.assertEqual((target / 'user.txt').read_bytes(), b'preserve me')
            finally:
                TX, RELEASE = original_tx, original_release

        def test_path_escape_rejected(self):
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(ValueError, 'PATH_OUTSIDE_RECORDED_ROOT'):
                    confined(Path(directory), '../outside.txt')

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ToolTests))
    print('COLLECTION_FULL_PACKAGE_TOOL_TESTS=' + ('PASS' if result.wasSuccessful() else 'FAIL')
          + '; tests=' + str(result.testsRun) + '; build_invoked=false; game_actions=0')
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('self-test', 'verify', 'review', 'finalize'))
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build_relocated')
    parser.add_argument('--runs-root', type=Path, default=TX)
    parser.add_argument('--run-summary', type=Path, action='append', help='Explicit actual summary path; repeat for selected segments.')
    parser.add_argument('--probe-result', type=Path, action='append', help='Explicit real probe result.json; repeat as needed; failed probes remain failed.')
    parser.add_argument('--snapshot', type=Path, default=TX / 'input/task_snapshot.json')
    parser.add_argument('--journal', type=Path, default=ROOT / 'artifacts/m2_savedvalue_collection/journal')
    parser.add_argument('--require-business-complete', action='store_true')
    args = parser.parse_args()
    args.build_dir, args.runs_root = args.build_dir.resolve(), args.runs_root.resolve()
    return {'self-test': self_test, 'verify': verify, 'review': review, 'finalize': finalize}[args.phase](args) or 0


if __name__ == '__main__':
    raise SystemExit(main())
