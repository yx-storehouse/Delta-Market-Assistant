"""Record the completed PR10 build/package/rollback evidence without rerunning it."""
from __future__ import annotations

import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m1_pr10_transaction'


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_log(name: str) -> str:
    data = (TX / 'logs' / name).read_bytes()
    return data.decode('utf-16' if data.startswith(b'\xff\xfe') else 'utf-8-sig').replace('\r\n', '\n')


def main() -> None:
    package = json.loads((TX / 'package_commands.json').read_text(encoding='utf-8'))
    assert len(package) == 5 and all(record['exit_status'] == 0 for record in package)
    baseline = read_log('baseline_test.log')
    final_build = read_log('final_build.log')
    sqlite = read_log('sqlite_ledger_tests.log')
    assert '100% tests passed out of 10' in baseline
    assert '100% tests passed out of 12' in final_build and 'PACKAGE=PASS' in final_build
    assert 'SQLITE_LEDGER_TESTS=PASS; assertions=316; failures=0' in sqlite
    modified_ui = next(record for record in package if record['label'] == 'MODIFIED_UI')
    for marker in ['UI_NAVIGATION_TEXT_UTF8=PASS', 'UI_ACTION_TEXT_UTF8=PASS', 'UI_NO_MOJIBAKE_LABELS=PASS']:
        assert marker in modified_ui['stdout']

    # Preserve a reviewable source patch, including newly added source files.
    names = set(subprocess.check_output(['git', 'diff', '--name-only'], cwd=ROOT, encoding='utf-8').splitlines())
    names.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT, encoding='utf-8').splitlines())
    roots = ('.gitignore', 'CMakeLists.txt', 'build.ps1', 'README.md', 'SESSION_START.md')
    selected = sorted(name for name in names if name in roots or name.startswith(('src/', 'tests/', 'docs/')))
    patch = []
    base_revision = (TX / 'baseline/source_revision.txt').read_text(encoding='ascii').strip()
    for name in selected:
        prior = subprocess.run(['git', 'show', f'{base_revision}:{name}'], cwd=ROOT, capture_output=True)
        old = prior.stdout.decode('utf-8').splitlines(keepends=True) if prior.returncode == 0 else []
        new = (ROOT / name).read_text(encoding='utf-8').splitlines(keepends=True)
        patch.extend(difflib.unified_diff(old, new, fromfile='a/' + name, tofile='b/' + name))
    (TX / 'DIFF_FILE').write_text(''.join(patch), encoding='utf-8')

    release = ROOT / 'dist/RelinkStudio/RelinkStudio.exe'
    original = TX / 'baseline/release/RelinkStudio.exe'
    restored = TX / 'rollback_test/RelinkStudio.exe'
    modified = TX / 'MODIFIED_FILE.exe'
    assert digest(release) == digest(modified) == digest(ROOT / 'build_relocated/RelinkStudio.exe')
    assert digest(original) == digest(restored) != digest(modified)
    commands = [
        dict(label='BASELINE_BUILD', command='powershell -NoProfile -ExecutionPolicy Bypass -File .\\build.ps1 -Test',
             input='Original source at ' + base_revision, stdout=baseline, stderr='Captured together in baseline_test.log', exit_status=0),
        dict(label='FINAL_BUILD', command='powershell -NoProfile -ExecutionPolicy Bypass -File .\\build.ps1 -Test -Package',
             input='PR10 source and final corrected Chinese UI; build_relocated; isolated application tests.',
             stdout=final_build, stderr='Captured together in final_build.log', exit_status=0),
        dict(label='SQLITE_REGRESSION', command='.\\build_relocated\\sqlite_ledger_tests.exe',
             input='Temporary databases; package DLL/plugin path; four controlled child exits; no external actions.',
             stdout=sqlite, stderr='Captured together in sqlite_ledger_tests.log', exit_status=0),
        *package,
    ]
    (TX / 'commands.json').write_text(json.dumps(commands, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    lines = [
        'Delta Market Assistant PR10 — completed transaction verification',
        'Date: 2026-10-06 (Asia/Shanghai)',
        f'CWD: {ROOT}',
        f'Original source revision: {base_revision}',
        'Changed branch: main; SQLite PRAGMA user_version 0 -> 1 -> 2.',
        'Changed behavior: durable event/attempt/receipt/reservation/ledger transactions; committed-only projection; restart reconciliation; backup.',
        'Changed guards: commitLedger rejects RECEIPT_UNCONFIRMED and RECEIPT_OUTCOME_MISMATCH in both stores.',
        'Changed UI fields: 330 Chinese string literals restored; no layout/code-structure/ASCII-key change; 3 visible-text assertions added.',
        'Normal UI SQLite/profile/records integration remains PR11. This PR does not enable screen capture, OCR or external actions.',
        '',
        f'MODIFIED_FILE: {modified}', f'DIFF_FILE: {TX / "DIFF_FILE"}',
        f'VERIFICATION: {TX / "VERIFICATION.txt"}', f'ROLLBACK: {TX / "ROLLBACK.sh"}',
        f'Original EXE SHA256: {digest(original)}', f'Modified EXE SHA256: {digest(modified)}',
        f'Restored EXE SHA256: {digest(restored)}', f'Delivery EXE: {release}',
        'Rollback copies original application files only; it does not revert Git or delete new database files.',
        'Restored behavior/status: original UI_SELF_TEST=PASS, exit=0; historical baseline text is preserved, not claimed as fixed.',
        'Fixed delivery and MODIFIED_FILE remain changed; original release stays byte-identical; database sentinel survives rollback.',
        'Visual review: final run.png and favorites.png reopened; normal Chinese, neutral white/gray store layout and intact content/controls verified.',
        'Baseline, modified and restored snapshots use separate directories; restored snapshots do not overwrite modified screenshots.',
        '', 'Exact commands, inputs, literal outputs and exit status:',
    ]
    for record in commands:
        lines += ['', record['label'], 'COMMAND: ' + record['command'], 'INPUT: ' + record['input'],
                  'STDOUT:', record['stdout'].rstrip(), 'STDERR:', record['stderr'] or '(empty)',
                  'EXIT_STATUS: ' + str(record['exit_status'])]
    lines += ['', 'RESOLVED DURING VERIFICATION:',
              'Initial DB_FULL regression exposed automatic SQLite rollback; fixed and covered by 316 passing assertions.',
              'Rollback shell initially lacked PowerShell on clean PATH; fixed by resolving the system executable explicitly; rerun exit=0.',
              'Visual review exposed existing Chinese mojibake; repaired, rebuilt and all delivery checks rerun.',
              'Four controlled child exits test process recovery, not physical power-loss guarantees.',
              'Final result: BUILD=PASS; CTEST=12/12; SQLITE=316/316; STORAGE_PACKAGE=22/22; UI=PASS; ROLLBACK=PASS.']
    (TX / 'VERIFICATION.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    # Reopen all four transaction artifacts and verify they contain actual bytes.
    for path in [modified, TX / 'DIFF_FILE', TX / 'VERIFICATION.txt', TX / 'ROLLBACK.sh']:
        assert path.read_bytes()
    print('PR10_ARTIFACTS_REOPENED=4')
    print('PR10_TRANSACTION_VERIFICATION=PASS')
    print('MODIFIED_SHA256=' + digest(modified))


if __name__ == '__main__':
    main()
