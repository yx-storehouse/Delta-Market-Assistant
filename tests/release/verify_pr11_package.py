"""Verify the extracted PR11 package and rollback without SDK PATH or visible UI."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m1_pr11_transaction'
RELEASE = ROOT / 'dist/RelinkStudio'
BASELINE = TX / 'baseline/release'
ROLLBACK = TX / 'rollback_test'


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    TX.mkdir(parents=True, exist_ok=True)
    records: list[dict] = []
    env = os.environ.copy()
    for key in ['QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH']:
        env.pop(key, None)
    env['QT_QPA_PLATFORM'] = 'offscreen'
    env['QT_SCALE_FACTOR'] = '1'
    system = Path(os.environ.get('SystemRoot', r'C:\Windows'))

    def run(label: str, args: list[str], required: str, runtime: Path, inputs: str) -> None:
        process_env = env.copy()
        process_env['PATH'] = ';'.join(map(str, [runtime, system / 'System32', system]))
        if label == 'WORKSPACE_TESTS':
            # The console test binary lives in build_relocated, not next to the
            # package qt.conf. Explicitly use only the released plugin directory.
            process_env['QT_PLUGIN_PATH'] = str(RELEASE)
        result = subprocess.run(args, cwd=ROOT, env=process_env, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=180,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        records.append(dict(label=label, command=subprocess.list2cmdline(args), input=inputs,
                            stdout=result.stdout, stderr=result.stderr, exit_status=result.returncode))
        (TX / 'package_commands.json').write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print(f'{label}_EXIT={result.returncode}')
        for line in result.stdout.splitlines():
            if required in line:
                print(line)
        if result.returncode or required not in result.stdout:
            raise RuntimeError(f'{label}: {result.stdout}\n{result.stderr}')

    assert (RELEASE / 'Qt6Sql.dll').is_file()
    assert (RELEASE / 'sqldrivers/qsqlite.dll').is_file()
    assert (RELEASE / 'platforms/qwindows.dll').is_file()
    assert (RELEASE / 'platforms/qoffscreen.dll').is_file()
    assert 'Plugins=.' in (RELEASE / 'qt.conf').read_text(encoding='utf-8')
    manifest = json.loads((RELEASE / 'file_manifest.json').read_text(encoding='utf-8'))
    for name, entry in manifest['files'].items():
        path = RELEASE / name
        assert path.is_relative_to(RELEASE) and sha(path) == entry['sha256'], name
    assert sha(RELEASE / 'RelinkStudio.exe') == sha(ROOT / 'build_relocated/RelinkStudio.exe')

    # An immutable local binary copy is the transaction's MODIFIED_FILE.
    shutil.copy2(RELEASE / 'RelinkStudio.exe', TX / 'MODIFIED_FILE.exe')
    base_hashes = {p.relative_to(BASELINE).as_posix(): sha(p) for p in sorted(BASELINE.rglob('*')) if p.is_file()}
    assert base_hashes['RelinkStudio.exe'] != sha(TX / 'MODIFIED_FILE.exe')
    (TX / 'baseline_release_hashes.json').write_text(json.dumps(base_hashes, indent=2) + '\n', encoding='utf-8')
    run('BASELINE', [str(BASELINE / 'RelinkStudio.exe'), '--self-test', '--snapshot-dir', str(TX / 'baseline/ui_snapshots'), '--config', str(TX / 'baseline-config.json')],
        'UI_SELF_TEST=PASS', BASELINE, 'Original extracted release; isolated synthetic config; package-only PATH.')
    run('MODIFIED', [str(RELEASE / 'RelinkStudio.exe'), '--storage-self-test'],
        'STORAGE_SELF_TEST=PASS', RELEASE, 'QTemporaryDir SQLite database; committed dispatch, reopen, Unknown recovery, backup.')
    run('MODIFIED_UI', [str(RELEASE / 'RelinkStudio.exe'), '--self-test', '--snapshot-dir', str(TX / 'snapshots'), '--config', str(TX / 'modified-config.json')],
        'UI_SELF_TEST=PASS', RELEASE, 'Synthetic UI/config; offscreen page snapshots; package-only PATH.')
    modified_ui = next(item for item in records if item['label'] == 'MODIFIED_UI')
    assert 'WORKSPACE_UI_SELF_TEST=PASS' in modified_ui['stdout']
    ui_results = json.loads((TX / 'snapshots/workspace/ui_results.json').read_text(encoding='utf-8'))
    assert ui_results['passed'] and ui_results['image_file_write_count'] == 0
    assert not ui_results['game_connected'] and not ui_results['system_input_sent']
    run('WORKSPACE_TESTS', [str(ROOT / 'build_relocated/workspace_tests.exe')],
        'WORKSPACE_TESTS=PASS', RELEASE,
        'Temporary workspace; reviewed disabled profiles; real SQLite busy/commit failures; historical replay; CSV and source validation.')

    # Re-run only within this fixed, isolated path; retain any database already there.
    shutil.copytree(RELEASE, ROLLBACK, dirs_exist_ok=True)
    marker = ROLLBACK / 'preserve-new-ledger.sqlite'
    import sqlite3
    with sqlite3.connect(marker) as database:
        database.execute('CREATE TABLE IF NOT EXISTS sentinel (id INTEGER PRIMARY KEY, value TEXT)')
        database.execute("INSERT OR REPLACE INTO sentinel VALUES (1, 'preserve new ledger on application rollback')")
    marker_hash = sha(marker)
    bash = Path(r'C:\Program Files\Git\bin\bash.exe')
    command = 'chmod +x artifacts/m1_pr11_transaction/ROLLBACK.sh && ./artifacts/m1_pr11_transaction/ROLLBACK.sh "$1"'
    run('ROLLBACK', [str(bash), '-c', command, 'rollback', str(ROLLBACK)],
        'ROLLBACK_RESTORED=PASS', bash.parent,
        'Modified extracted release copy; restore original release files; preserve new ledger database.')
    assert sha(marker) == marker_hash
    for name, expected in base_hashes.items():
        assert sha(ROLLBACK / name) == expected, name
    run('RESTORED', [str(ROLLBACK / 'RelinkStudio.exe'), '--self-test', '--snapshot-dir', str(ROLLBACK / 'ui_snapshots'), '--config', str(TX / 'restored-config.json')],
        'UI_SELF_TEST=PASS', ROLLBACK, 'Restored baseline executable and dependencies; isolated config; package-only PATH.')
    assert sha(RELEASE / 'RelinkStudio.exe') == sha(TX / 'MODIFIED_FILE.exe')
    assert sha(BASELINE / 'RelinkStudio.exe') == base_hashes['RelinkStudio.exe']
    assert sha(RELEASE / 'RelinkStudio.exe') != sha(ROLLBACK / 'RelinkStudio.exe')
    print('PACKAGE_MANIFEST=PASS')
    print('RESTORED_HASH=PASS; baseline_unchanged=true; modified_file_retained=true; new_database_preserved=true')
    print('PR11_PACKAGE_VERIFICATION=PASS')


if __name__ == '__main__':
    main()
