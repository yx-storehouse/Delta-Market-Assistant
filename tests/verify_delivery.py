"""Validate the packaged frontend without using the interactive desktop."""
from pathlib import Path
import difflib
import hashlib
import json
import os
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
os.chdir(root)
art = root / 'artifacts'
art.mkdir(exist_ok=True)
exe = root / 'dist/RelinkStudio/RelinkStudio.exe'
env = os.environ.copy()
env['PATH'] = str(exe.parent) + ';' + os.environ.get('WINDIR', r'C:\Windows') + '\\System32;' + os.environ.get('WINDIR', r'C:\Windows')
for key in ['QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH']:
    env.pop(key, None)
env['QT_QPA_PLATFORM'] = 'offscreen'
env['QT_SCALE_FACTOR'] = '1'
records = []

def run(label, args, display, input_text, process_env=env):
    p = subprocess.run(args, cwd=root, env=process_env, capture_output=True, text=True,
                       encoding='utf-8', errors='replace', timeout=90, creationflags=0x08000000)
    record = {'label': label, 'command': display, 'input': input_text,
              'stdout': p.stdout, 'stderr': p.stderr, 'exit_status': p.returncode}
    records.append(record)
    print(label, p.returncode, p.stdout.strip())
    if p.returncode != 0:
        raise RuntimeError(json.dumps(record, ensure_ascii=False))
    return record

run('DOMAIN', [str(root/'build/domain_tests.exe')],
    r'build\domain_tests.exe', '48 data-model assertions; package-only runtime PATH; QCoreApplication.')
run('PACKAGE_UI', [str(exe), '--self-test', '--snapshot-dir', str(art/'screenshots'), '--config', str(art/'ui-test-config.json')],
    r'dist\RelinkStudio\RelinkStudio.exe --self-test --snapshot-dir artifacts\screenshots --config artifacts\ui-test-config.json',
    'Synthetic application state; package-only PATH; Qt offscreen; no screen capture/input.')
run('DEMO_FIXTURE', [str(exe), '--write-demo-config', str(art/'BASELINE.json')],
    r'dist\RelinkStudio\RelinkStudio.exe --write-demo-config artifacts\BASELINE.json', 'Fresh 12-skin / 5-task demo fixture.')
baseline = (art/'BASELINE.json').read_bytes()
data = json.loads(baseline)
assert data['tasks'][0]['maxPrice'] == 650 and data['tasks'][0]['quantity'] == 3
data['tasks'][0]['maxPrice'] = 700
data['tasks'][0]['quantity'] = 4
modified = (json.dumps(data, ensure_ascii=False, sort_keys=True, indent=4) + '\n').encode('utf-8')
(art/'MODIFIED_FILE.json').write_bytes(modified)
(art/'DIFF_FILE.diff').write_text(''.join(difflib.unified_diff(baseline.decode('utf-8').splitlines(True), modified.decode('utf-8').splitlines(True), fromfile='BASELINE.json', tofile='MODIFIED_FILE.json')), encoding='utf-8')
hashes = {'baseline': hashlib.sha256(baseline).hexdigest(), 'modified': hashlib.sha256(modified).hexdigest(), 'application': hashlib.sha256(exe.read_bytes()).hexdigest()}
(art/'fixture_hashes.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
run('BASELINE', [str(exe), '--validate-config', str(art/'BASELINE.json')],
    r'dist\RelinkStudio\RelinkStudio.exe --validate-config artifacts\BASELINE.json', 'First task maxPrice=650, quantity=3.')
run('MODIFIED', [str(exe), '--validate-config', str(art/'MODIFIED_FILE.json')],
    r'dist\RelinkStudio\RelinkStudio.exe --validate-config artifacts\MODIFIED_FILE.json', 'First task maxPrice=700, quantity=4.')
run('EQUALS_HEADLESS', [str(exe), '--validate-config='+str(art/'BASELINE.json')],
    r'dist\RelinkStudio\RelinkStudio.exe --validate-config=artifacts\BASELINE.json', 'Equals-style CLI argument; still forced offscreen.')
shutil.copyfile(art/'MODIFIED_FILE.json', art/'ROLLBACK_TEST.json')
bash = r'C:\Program Files\Git\bin\bash.exe'
rollback_args = 'chmod +x artifacts/ROLLBACK.sh && ./artifacts/ROLLBACK.sh artifacts/ROLLBACK_TEST.json'
shell_env = os.environ.copy(); shell_env['PYTHONIOENCODING'] = 'utf-8'
run('ROLLBACK', [bash, '-c', rollback_args],
    "& 'C:\\Program Files\\Git\\bin\\bash.exe' -c '"+rollback_args+"'", 'ROLLBACK_TEST.json copied byte-for-byte from MODIFIED_FILE.json.', shell_env)
run('RESTORED', [str(exe), '--validate-config', str(art/'ROLLBACK_TEST.json')],
    r'dist\RelinkStudio\RelinkStudio.exe --validate-config artifacts\ROLLBACK_TEST.json', 'Restored baseline fixture.')
assert (art/'ROLLBACK_TEST.json').read_bytes() == baseline
assert hashlib.sha256(exe.read_bytes()).hexdigest() == hashes['application']
ui = json.loads((art/'screenshots/ui_results.json').read_text(encoding='utf-8'))
assert ui['passed'] and ui['offscreen'] and not ui['system_input_sent'] and not ui['game_connected']
lines = [
    'Relink Studio frontend verification — 2026-10-04',
    'Working directory: ' + str(root),
    'Changed fields in a TEST CONFIG COPY: tasks[0].maxPrice 650 -> 700; tasks[0].quantity 3 -> 4.',
    'The fixture is not the default user configuration. No original game/helper EXE was patched.',
    'MODIFIED_FILE: ' + str(art/'MODIFIED_FILE.json'),
    'DIFF_FILE: ' + str(art/'DIFF_FILE.diff'),
    'VERIFICATION: ' + str(art/'VERIFICATION.txt'),
    'ROLLBACK: ' + str(art/'ROLLBACK.sh'),
    'Baseline SHA256: ' + hashes['baseline'],
    'Modified SHA256: ' + hashes['modified'],
    'Packaged EXE SHA256: ' + hashes['application'],
    'UI assertion count: ' + str(len(ui['checks'])),
    'All UI tests use Qt offscreen. Packaged tests use no SDK paths. Native interactive desktop testing was not performed.',
    ''
]
for record in records:
    lines += [record['label'], 'COMMAND: '+record['command'], 'INPUT: '+record['input'],
              'STDOUT:', record['stdout'].rstrip(), 'STDERR: '+(record['stderr'].rstrip() or '<empty>'),
              'EXIT_STATUS: '+str(record['exit_status']), '']
lines += ['RESTORED: test copy equals BASELINE.json exactly and the application validator accepts 650.00 / 3.',
          'LEFT CHANGED: MODIFIED_FILE.json remains 700 / 4.',
          'APPLICATION: delivered executable unchanged by baseline/modified/rollback tests.', '']
(art/'VERIFICATION.txt').write_text('\n'.join(lines), encoding='utf-8')
(art/'delivery_test_results.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
for name in ['MODIFIED_FILE.json', 'DIFF_FILE.diff', 'VERIFICATION.txt', 'ROLLBACK.sh']:
    assert (art/name).read_bytes()
    print('REOPEN=PASS; file='+name)
print('DELIVERY_VERIFICATION=PASS; ui_checks='+str(len(ui['checks'])))
