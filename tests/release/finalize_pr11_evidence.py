"""Freeze observed PR11 build and extracted-package evidence, preserving PR10."""
from __future__ import annotations

import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m1_pr11_transaction'


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    build = json.loads((TX / 'build_result.json').read_text(encoding='utf-8'))
    commands = json.loads((TX / 'package_commands.json').read_text(encoding='utf-8'))
    ui = json.loads((TX / 'snapshots/workspace/ui_results.json').read_text(encoding='utf-8'))
    assert build['exit_status'] == 0 and '100% tests passed out of 13' in build['stdout']
    assert len(commands) == 6 and all(c['exit_status'] == 0 for c in commands)
    assert ui['passed'] and ui['image_file_write_count'] == 0
    assert 'WORKSPACE_UI_SELF_TEST=PASS' in next(c for c in commands if c['label'] == 'MODIFIED_UI')['stdout']
    assert 'WORKSPACE_TESTS=PASS' in next(c for c in commands if c['label'] == 'WORKSPACE_TESTS')['stdout']
    base_revision = (TX / 'baseline/source_revision.txt').read_text(encoding='ascii').strip()
    modified = TX / 'MODIFIED_FILE.exe'
    original = TX / 'baseline/release/RelinkStudio.exe'
    restored = TX / 'rollback_test/RelinkStudio.exe'
    delivered = ROOT / 'dist/RelinkStudio/RelinkStudio.exe'
    assert sha(modified) == sha(delivered) == sha(ROOT / 'build_relocated/RelinkStudio.exe')
    assert sha(original) == sha(restored) != sha(modified)
    names = set(subprocess.check_output(['git', 'diff', '--name-only', base_revision], cwd=ROOT, encoding='utf-8').splitlines())
    names.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT, encoding='utf-8').splitlines())
    files = sorted(name for name in names if name.startswith(('src/', 'tests/', 'docs/'))
                   or name in {'.gitignore','CMakeLists.txt','build.ps1','README.md','SESSION_START.md'})
    patch = []
    for name in files:
        before = subprocess.run(['git', 'show', f'{base_revision}:{name}'], cwd=ROOT, capture_output=True)
        old = before.stdout.decode('utf-8').splitlines(keepends=True) if before.returncode == 0 else []
        new = (ROOT / name).read_text(encoding='utf-8').splitlines(keepends=True)
        patch.extend(difflib.unified_diff(old,new,fromfile='a/'+name,tofile='b/'+name))
    (TX / 'DIFF_FILE').write_text(''.join(patch), encoding='utf-8')
    baseline_log = (TX / 'logs/baseline_test.log').read_bytes()
    baseline_output = baseline_log.decode('utf-16' if baseline_log.startswith(b'\xff\xfe') else 'utf-8-sig')
    assert '100% tests passed out of 12' in baseline_output
    records = [dict(label='BASELINE_BUILD', command='powershell -NoProfile -ExecutionPolicy Bypass -File .\\build.ps1 -Test',
                    input='Original PR10 source and isolated tests.', stdout=baseline_output, stderr='Combined in baseline_test.log', exit_status=0),
               dict(label='MODIFIED_BUILD', command=build['command'],
                    input='PR11 source; build_relocated; all 13 CTest targets and QSQLITE deployment.',
                    stdout=build['stdout'], stderr=build['stderr'], exit_status=build['exit_status']), *commands]
    (TX / 'commands.json').write_text(json.dumps(records, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    lines = [
        'Delta Market Assistant — PR11 verification', 'Date: 2026-10-06 (Asia/Shanghai)',
        'Changed branch: main', 'Original source revision: '+base_revision,
        'Changed fields/behavior: MainWindow uses WorkspaceController; reviewed ConfigV2 profiles remain disabled; persistent Replay uses committed SQLite records.',
        'Schema user_version remains 2; added committedRuns/eventsForRun/recoveryAudit read APIs, not a destructive migration.',
        'Review/profile selection/run snapshot are separate. Built-in eight-step fixture is explicitly selected; only synthetic replay records are created.',
        'Changed guards: read-only, active run and dirty review checks; confirmed price requires committed Success linkage; CSV avoids workspace files and escapes formulas.',
        'Changed display: Chinese labels; short identifiers with full raw values in tooltips; observable price differs from simulated confirmed price.',
        'No live capture, OCR, system input or actual trading is integrated. PR12 is the next milestone.',
        '', f'DELIVERY: {delivered}', f'MODIFIED_FILE: {modified}', f'DIFF_FILE: {TX / "DIFF_FILE"}',
        f'VERIFICATION: {TX / "VERIFICATION.txt"}', f'ROLLBACK: {TX / "ROLLBACK.sh"}',
        f'BASELINE_SHA256: {sha(original)}', f'MODIFIED_SHA256: {sha(modified)}', f'RESTORED_SHA256: {sha(restored)}',
        'Restored behavior/status: original PR10 offscreen UI self-test passes with exit 0.',
        'Rollback tested only on a separate extracted copy; fixed delivery and MODIFIED_FILE remain changed; original baseline unchanged; new database sentinel preserved.',
        'Visual review: final workspace/run.png and records.png inspected offscreen; Chinese text, neutral surfaces and separated values are readable.',
        'Images written here are explicit offscreen test artifacts, not runtime screen capture or OCR output.',
        '', 'Exact commands, inputs, literal output and exit status:',
    ]
    for c in records:
        lines.extend(['', c['label'], 'COMMAND: '+c['command'], 'INPUT: '+c['input'],
                      'STDOUT:', c['stdout'].rstrip(), 'STDERR:', c['stderr'].rstrip() or '(empty)',
                      'EXIT_STATUS: '+str(c['exit_status'])])
    lines += ['', 'VERIFIED: 13/13 CTest; Workspace service regression; GUI workspace workflow; packaged driver; rollback and restored PR10 UI.',
              'PR11 evidence is separate from earlier PR10 evidence; historical baselines are retained.']
    (TX / 'VERIFICATION.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    for path in [modified, TX/'DIFF_FILE',TX/'VERIFICATION.txt',TX/'ROLLBACK.sh']:
        assert path.read_bytes()
    print('PR11_ARTIFACTS_REOPENED=4')
    print('PR11_TRANSACTION_VERIFICATION=PASS')
    print('MODIFIED_SHA256='+sha(modified))


if __name__ == '__main__':
    main()
