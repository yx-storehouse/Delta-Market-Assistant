"""Offline release/rollback acceptance for the separately recorded live lobby fix."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import verify_m2_capture_package as base

ROOT = Path(__file__).resolve().parents[2]
base.TX = ROOT / 'artifacts/m2_lobby_calibration'
base.BASELINE = base.TX / 'baseline/release'
base.h.TX, base.h.BASELINE = base.TX, base.BASELINE
base.PHASE_TITLE = 'M2 live lobby OCR calibration'
base.CHANGED_BOUNDARIES = 'SkinPageClassifier scoped lobby alias; --expected-page; ForegroundPolicy standalone/caller-owned; whole-batch focus ownership; lobby fixture regression.'
base.PHASE_NOTES = [
    'Capture diagnostics are read-only; user-authorized manual navigation clicks are recorded separately. Images remain in memory.',
    'User corrected per-action flashing: a whole batch now owns foreground, child probes never switch, outer cleanup restores once.',
    'Windows OCR read the visible 开始游戏 label as 开始游观. Alias requires exact provider, bounded button location, warehouse and prepare anchors.',
    'Only the observed lobby sample is calibrated; no claim that all game pages, navigation or transaction flow passed.',
    'Initial foreground-loss and unknown-page outcomes are preserved in live records, not relabeled as success.',
]
base.EXTRA_REQUIRED_RECORDS = ['LOBBY_CALIBRATION_TESTS', 'STARTUP_OBSERVER_TESTS', 'PACKAGED_STARTUP_SELF_TEST', 'LOBBY_CLI_ARGUMENT_TESTS', 'FOREGROUND_POLICY_TESTS', 'FOREGROUND_BATCH_TESTS']


def cli_tests():
    exe = str(base.RELEASE / 'RelinkStudio.exe')
    env = base.h.environment(base.RELEASE)
    checks = [
        (['--live-capture-check', '--focus-policy', 'invalid'], 'E_DIAGNOSTIC_FOCUS_POLICY'),
        (['--live-capture-check', '--expected-page', 'lobby'], 'E_DIAGNOSTIC_EXPECTED_PAGE'),
        (['--live-capture-check', '--ocr', '--expected-page', 'invented'], 'E_DIAGNOSTIC_EXPECTED_PAGE'),
        (['--live-capture-check', '--ocr', '--expected-page', 'unknown'], 'E_DIAGNOSTIC_EXPECTED_PAGE'),
        (['--live-capture-check', '--ocr-lobby-anchors', '--target-hwnd', '1', '--target-pid', '1',
          '--return-hwnd', '2', '--return-pid', '2'], 'E_DIAGNOSTIC_ARGUMENTS'),
    ]
    for flags, error in checks:
        args = [exe, *flags]
        result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, encoding='utf-8',
                                timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
        output = json.loads(result.stdout)
        assert result.returncode == 2 and output['error'] == error
        assert output['frames'] == [] and not output['recognition_performed'] and not output['game_input_sent']
    print('LOBBY_CLI_ARGUMENT_TESTS=PASS; assertions=5; live_capture=false')


def verify():
    base.verify()
    env = base.h.environment(base.RELEASE); env['QT_PLUGIN_PATH'] = str(base.RELEASE)
    for name, fixture in [('lobby_calibration_tests', 'lobby_live_20261007.json'), ('startup_observer_tests', 'startup_pages.json')]:
        base.h.run(name.upper(), [str(base.BUILD / (name + '.exe')),
            str(ROOT / 'docs/business_rebuild/implementation/runtime/fixtures' / fixture)],
            'Recorded text projections and synthetic negative cases; no live capture.', name.upper() + '=PASS', env=env)
    base.h.run('PACKAGED_STARTUP_SELF_TEST', [str(base.RELEASE / 'RelinkStudio.exe'), '--startup-observer-self-test'],
        'Embedded 27 historical pages, 5 startup routes and 1 recorded live lobby text projection; no game capture.', 'STARTUP_OBSERVER_SELF_TEST=PASS')
    base.h.run('LOBBY_CLI_ARGUMENT_TESTS', [sys.executable, '-X', 'utf8', str(Path(__file__).resolve()), 'cli-tests'],
        'Invalid CLI combinations must exit 2 before any window binding or capture.', 'LOBBY_CLI_ARGUMENT_TESTS=PASS', env=os.environ.copy())
    base.h.run('FOREGROUND_POLICY_TESTS', [str(base.BUILD/'foreground_policy_tests.exe')],
        'Injected focus callbacks: single final restore, zero child switching, interruption and exception paths.', 'FOREGROUND_POLICY_TESTS=PASS', env=env)
    base.h.run('FOREGROUND_BATCH_TESTS', [sys.executable,'-X','utf8',str(ROOT/'tests/manual/test_foreground_batch_core.py')],
        'Pure batch coordinator: multiple actions one lease, stop on failure/user focus loss, one finally cleanup.', env=os.environ.copy())


def finalize():
    base.h.run('LOBBY_LIVE_EVIDENCE', [sys.executable, '-X', 'utf8', str(ROOT / 'tests/manual/verify_lobby_evidence.py')],
        'Reopen recorded live metadata and the fixed current binary; no new game capture.', 'LOBBY_LIVE_EVIDENCE=PASS', env=os.environ.copy())
    base.h.run('FOREGROUND_BATCH_LIVE', [sys.executable,'-X','utf8',str(ROOT/'tests/manual/verify_foreground_batch_evidence.py')],
        'Verify saved two-capture batch, current EXE hash and focus ownership; no new foreground change.', 'FOREGROUND_BATCH_LIVE=PASS', env=os.environ.copy())
    base.finalize()
    path = base.TX / 'VERIFICATION.txt'
    text = path.read_text(encoding='utf-8')
    # Earlier harness wording implied a newly added helper even for empty manifests.
    additions = json.loads((base.TX / 'added_release_files.json').read_text(encoding='utf-8'))
    text = text.replace('user sentinel preserved; new helper removed.',
        f'user sentinel preserved; actual added managed files removed={len(additions)}.')
    text += '\nLIVE CAPTURE RECORDS (recorded failures are retained):\n'
    for record_path in sorted(base.TX.glob('live_*.json')):
        record = json.loads(record_path.read_text(encoding='utf-8'))
        if not isinstance(record, dict) or 'command' not in record:
            continue
        text += '\n' + record_path.name + '\nCOMMAND: ' + subprocess.list2cmdline(record['command'])
        text += '\nINPUT: ' + record.get('input', 'Explicit in-memory visual inspection; preview pixels excluded from this record.')
        text += '\nSTDOUT/RESULT:\n' + record.get('stdout', json.dumps(record.get('result'), ensure_ascii=False))
        text += '\nSTDERR: ' + (record.get('stderr') or '(empty)') + '\nEXIT_STATUS: ' + str(record['exit_status']) + '\n'
    text += '\nRollback stdout retains legacy added_helper_removed field; actual additions count is reported above.\n'
    text += '\nUSER-AUTHORIZED MANUAL NAVIGATION AND BATCH FOCUS RECORDS:\n'
    for evidence in [*sorted(base.TX.glob('navigation_*.json')), base.TX/'foreground_batch_live.json']:
        text += '\n' + evidence.name + '\n' + evidence.read_text(encoding='utf-8') + '\n'
    path.write_text(text, encoding='utf-8')
    for name in ['MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh']:
        assert (base.TX / name).read_bytes()
    print('LOBBY_TRANSACTION_REOPEN=PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=['build', 'verify', 'finalize', 'cli-tests'])
    phase = parser.parse_args().phase
    if phase == 'build': base.build()
    elif phase == 'cli-tests': cli_tests()
    else: globals()[phase]()
