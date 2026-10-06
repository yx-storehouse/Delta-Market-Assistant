"""Record PR12A baseline/build/package/rollback evidence without visible windows.

The baseline must be frozen before edits. Each phase records actual commands,
outputs and exit status; historical transaction directories are never reused.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m1_pr12a_transaction'
RELEASE = ROOT / 'dist/RelinkStudio'
BASELINE = TX / 'baseline/release'


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def environment(runtime: Path) -> dict:
    env = os.environ.copy()
    for key in ('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'RELINK_TEST_SCALE'):
        env.pop(key, None)
    system = Path(os.environ.get('SystemRoot', r'C:\Windows'))
    env.update(PATH=';'.join(map(str, (runtime, system / 'System32', system))),
               QT_QPA_PLATFORM='offscreen', QT_SCALE_FACTOR='1', PYTHONIOENCODING='utf-8')
    return env


def run(label: str, args: list[str], inputs: str, required: str = '',
        runtime: Path = RELEASE, env: dict | None = None) -> dict:
    result = subprocess.run(args, cwd=ROOT, env=env or environment(runtime),
                            capture_output=True, text=True, encoding='utf-8',
                            errors='replace', timeout=900,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    record = dict(label=label, command=subprocess.list2cmdline(args), input=inputs,
                  stdout=result.stdout, stderr=result.stderr, exit_status=result.returncode)
    path = TX / 'commands.json'
    records = json.loads(path.read_text(encoding='utf-8')) if path.exists() else []
    records.append(record)
    path.write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'{label}_EXIT={result.returncode}', flush=True)
    for line in result.stdout.splitlines():
        if (required and required in line) or 'tests passed' in line or 'SELF_TEST=' in line:
            print(line, flush=True)
    if result.returncode or (required and required not in result.stdout):
        raise RuntimeError(f'{label}: {result.stdout}\n{result.stderr}')
    return record


def ui(label: str, runtime: Path, output: Path) -> None:
    run(label, [str(runtime / 'RelinkStudio.exe'), '--self-test', '--snapshot-dir', str(output),
                '--config', str(TX / (label.lower() + '-config.json'))],
        'Synthetic UI/config and temporary workspace; Qt offscreen; package-only PATH.',
        'UI_SELF_TEST=PASS', runtime)
    for name in ('ui_results.json', 'workspace/ui_results.json'):
        data = json.loads((output / name).read_text(encoding='utf-8'))
        assert data['passed'] and not data['game_connected'] and not data['system_input_sent']
        if name.startswith('workspace/'):
            assert data['image_file_write_count'] == 0


def baseline() -> None:
    hashes = json.loads((TX / 'baseline_release_hashes.json').read_text(encoding='utf-8'))
    assert all(sha(BASELINE / name) == digest for name, digest in hashes.items())
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    revision_path = TX / 'baseline/source_revision.txt'
    if revision_path.exists():
        assert revision_path.read_text(encoding='ascii').strip() == revision
    else:
        revision_path.write_text(revision + '\n', encoding='ascii')
    ui('BASELINE', BASELINE, TX / 'baseline/ui_snapshots')
    run('BASELINE_STORAGE', [str(BASELINE / 'RelinkStudio.exe'), '--storage-self-test'],
        'Original extracted PR11; temporary SQLite database and recovery/backup checks.',
        'STORAGE_SELF_TEST=PASS', BASELINE)


def build() -> None:
    run('MODIFIED_BUILD', ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                          str(ROOT / 'build.ps1'), '-Test', '-Package'],
        'Current source; actual build directory selected by build.ps1; CTest then extracted deployment.',
        'PACKAGE=PASS', env=os.environ.copy())


def modified(build_dir: Path) -> None:
    manifest = json.loads((RELEASE / 'file_manifest.json').read_text(encoding='utf-8'))
    for name, entry in manifest['files'].items():
        path = (RELEASE / name).resolve()
        assert path.is_relative_to(RELEASE) and sha(path) == entry['sha256'], name
    for name in ('Qt6Sql.dll', 'sqldrivers/qsqlite.dll', 'platforms/qwindows.dll', 'platforms/qoffscreen.dll'):
        assert (RELEASE / name).is_file(), name
    assert 'Plugins=.' in (RELEASE / 'qt.conf').read_text(encoding='utf-8')
    assert sha(RELEASE / 'RelinkStudio.exe') == sha(build_dir / 'RelinkStudio.exe')
    shutil.copy2(RELEASE / 'RelinkStudio.exe', TX / 'MODIFIED_FILE.exe')
    assert sha(TX / 'MODIFIED_FILE.exe') != sha(BASELINE / 'RelinkStudio.exe')
    ui('MODIFIED', RELEASE, TX / 'snapshots')
    run('MODIFIED_STORAGE', [str(RELEASE / 'RelinkStudio.exe'), '--storage-self-test'],
        'Updated extracted binary; real packaged SQLite plugin; temporary transactions and recovery.',
        'STORAGE_SELF_TEST=PASS')
    for target, marker in [('workspace_tests', 'WORKSPACE_TESTS=PASS'),
                           ('ui_module_tests', 'UI_MODULE_TESTS=PASS')]:
        env = environment(RELEASE)
        env['QT_PLUGIN_PATH'] = str(RELEASE)
        run(target.upper(), [str(build_dir / (target + '.exe'))],
            'Independent regression executable; package-only PATH; offscreen; temporary state.', marker, env=env)
    for name in ('ui_results.json', 'workspace/ui_results.json'):
        original = json.loads((TX / 'baseline/ui_snapshots' / name).read_text(encoding='utf-8'))
        current = json.loads((TX / 'snapshots' / name).read_text(encoding='utf-8'))
        def check_names(value):
            return {c.get('check', c.get('name')) for c in value['checks']}
        assert check_names(original) <= check_names(current), 'Baseline checks removed: ' + name
    rollback = TX / 'rollback_test'
    shutil.copytree(RELEASE, rollback, dirs_exist_ok=True)
    database = rollback / 'preserve-new-ledger.sqlite'
    with sqlite3.connect(database) as connection:
        connection.execute('CREATE TABLE IF NOT EXISTS sentinel (id INTEGER PRIMARY KEY, value TEXT)')
        connection.execute("INSERT OR REPLACE INTO sentinel VALUES (1, 'preserve new ledger')")
    profile = rollback / 'profiles/preserve-reviewed-profile.json'
    profile.parent.mkdir(exist_ok=True)
    profile.write_text('{"enabled":false,"activation_required":true}\n', encoding='utf-8')
    saved = {p: sha(p) for p in (database, profile)}
    bash = Path(r'C:\Program Files\Git\bin\bash.exe')
    command = 'chmod +x artifacts/m1_pr12a_transaction/ROLLBACK.sh && ./artifacts/m1_pr12a_transaction/ROLLBACK.sh "$1"'
    run('ROLLBACK', [str(bash), '-c', command, 'rollback', str(rollback)],
        'Separate updated package copy; restore original program files; retain new database/profile sentinels.',
        'ROLLBACK_RESTORED=PASS', bash.parent)
    hashes = json.loads((TX / 'baseline_release_hashes.json').read_text(encoding='utf-8'))
    assert all(sha(rollback / name) == digest == sha(BASELINE / name) for name, digest in hashes.items())
    assert all(sha(p) == digest for p, digest in saved.items())
    ui('RESTORED', rollback, rollback / 'ui_snapshots')
    assert sha(RELEASE / 'RelinkStudio.exe') == sha(TX / 'MODIFIED_FILE.exe')
    print('PR12A_PACKAGE_VERIFICATION=PASS; baseline_unchanged=true; modified_file_retained=true; new_database_and_profile_preserved=true')


def finalize() -> None:
    records = json.loads((TX / 'commands.json').read_text(encoding='utf-8'))
    latest = {record['label']: record for record in records}
    for label in ('BASELINE', 'BASELINE_STORAGE', 'MODIFIED_BUILD', 'MODIFIED', 'MODIFIED_STORAGE',
                  'WORKSPACE_TESTS', 'UI_MODULE_TESTS', 'ROLLBACK', 'RESTORED'):
        assert latest[label]['exit_status'] == 0, label
    revision = (TX / 'baseline/source_revision.txt').read_text(encoding='ascii').strip()
    names = set(subprocess.check_output(['git', 'diff', '--name-only', revision], cwd=ROOT, text=True).splitlines())
    names.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT, text=True).splitlines())
    patch = []
    for name in sorted(names):
        if not (name.startswith(('src/', 'tests/', 'docs/')) or name in {'.gitignore', 'CMakeLists.txt', 'build.ps1', 'SESSION_START.md', 'README.md'}):
            continue
        previous = subprocess.run(['git', 'show', f'{revision}:{name}'], cwd=ROOT, capture_output=True)
        old = previous.stdout.decode('utf-8').splitlines(True) if previous.returncode == 0 else []
        new = (ROOT / name).read_text(encoding='utf-8').splitlines(True) if (ROOT / name).exists() else []
        patch.extend(difflib.unified_diff(old, new, fromfile='a/' + name, tofile='b/' + name))
    (TX / 'DIFF_FILE').write_text(''.join(patch), encoding='utf-8')
    modified_file = TX / 'MODIFIED_FILE.exe'
    assert sha(modified_file) == sha(RELEASE / 'RelinkStudio.exe')
    assert sha(BASELINE / 'RelinkStudio.exe') == sha(TX / 'rollback_test/RelinkStudio.exe') != sha(modified_file)
    lines = ['Delta Market Assistant — PR12A verification', 'Date: 2026-10-06 (Asia/Shanghai)',
             'Changed branch: main', 'Original source revision: ' + revision,
             'Changed fields/boundaries: page-owned widgets and callbacks; MainWindow composition; main startup separated from UI self-test runner.',
             'Compatibility: existing objectNames, CLI, UI assertions, Win11 light appearance, profile flags and schema v2 retained.',
             'Full UI diagnostics remain linked in the executable for packaged self-test compatibility; this phase is not binary-size optimization.',
             'Remaining: controller service split, history-query optimization, remaining UI extraction and full PR12 acceptance.',
             f'DELIVERY: {RELEASE / "RelinkStudio.exe"}', f'MODIFIED_FILE: {modified_file}',
             f'DIFF_FILE: {TX / "DIFF_FILE"}', f'VERIFICATION: {TX / "VERIFICATION.txt"}', f'ROLLBACK: {TX / "ROLLBACK.sh"}',
             'BASELINE_SHA256: ' + sha(BASELINE / 'RelinkStudio.exe'), 'MODIFIED_SHA256: ' + sha(modified_file),
             'Restored behavior/status: original PR11 UI/workspace self-tests pass with exit 0 on separate restored copy.',
             'Rollback preserves new database/profile sentinels. Fixed release and MODIFIED_FILE remain modified.',
             'Runtime image-file writes remain zero; snapshots below are explicitly requested offscreen test artifacts.',
             '', 'Exact commands, inputs, literal output and exit status (including any earlier failed attempts):']
    for record in records:
        lines += ['', record['label'], 'COMMAND: ' + record['command'], 'INPUT: ' + record['input'],
                  'STDOUT:', record['stdout'].rstrip(), 'STDERR:', record['stderr'].rstrip() or '(empty)',
                  'EXIT_STATUS: ' + str(record['exit_status'])]
    (TX / 'VERIFICATION.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for path in (modified_file, TX / 'DIFF_FILE', TX / 'VERIFICATION.txt', TX / 'ROLLBACK.sh'):
        assert path.read_bytes()
        print('REOPEN=PASS; path=' + str(path))
    print('PR12A_TRANSACTION_VERIFICATION=PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=['baseline', 'build', 'modified', 'finalize'])
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build_relocated')
    args = parser.parse_args()
    if args.phase == 'modified':
        modified(args.build_dir.resolve())
    else:
        globals()[args.phase]()
