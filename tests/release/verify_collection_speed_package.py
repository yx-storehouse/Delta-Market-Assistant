"""Reuse the tested package/rollback verifier with a separate frozen baseline.

No live capture or input. A successful package verification is not a claim
about the live collection cycle. All historical transactions stay untouched.
"""
import argparse
import json
from pathlib import Path
import subprocess

import verify_collection_full_package as shared
import verify_pr12a_package as h

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/collection_speed'


def configure():
    shared.TX = h.TX = TX
    shared.BASELINE = h.BASELINE = TX / 'baseline/release'
    shared.check_frozen.__defaults__ = (ROOT, TX)
    shared.source_diff.__defaults__ = (ROOT, TX)


def finalize():
    shared.check_frozen()
    commands = shared.read_json(TX / 'commands.json')
    latest = {record['label']: record for record in commands}
    for name in (*shared.REQUIRED, 'CAPTURE_SERVER_TESTS'):
        if latest.get(name, {}).get('exit_status') != 0:
            raise ValueError('MISSING_VERIFICATION:' + name)
    package = shared.read_json(TX / 'package_verification.json')
    live = shared.read_json(TX / 'live_result.json')
    for name, digest in shared.read_json(TX / 'modified_release_hashes.json').items():
        assert shared.sha(shared.RELEASE / name) == digest, name
    assert shared.sha(shared.RELEASE / 'RelinkStudio.exe') == shared.sha(TX / 'MODIFIED_FILE.exe')
    assert shared.sha(Path(package['rollback_target']) / 'RelinkStudio.exe') == shared.sha(shared.BASELINE / 'RelinkStudio.exe')
    diff, changes = shared.source_diff(ROOT, TX)
    (TX / 'DIFF_FILE').write_text(diff, encoding='utf-8')
    shared.write_json(TX / 'source_changes.json', changes)
    branch = subprocess.check_output(['git', 'branch', '--show-current'], cwd=ROOT, text=True).strip()
    fields = ('live-capture-server; capture-request OCR metrics; collection-local-price; '
              'local_price_ocr; persistent_capture; bounded_dynamic_readback_v1; fast_settle; condition_evidence')
    lines = ['Collection speed and numeric recognition transaction',
        'Changed branch: ' + branch, 'Changed fields: ' + fields,
        'Baseline comparison: exact frozen working tree, not git HEAD.',
        'BASELINE_SHA256=' + shared.sha(shared.BASELINE / 'RelinkStudio.exe'),
        'MODIFIED_SHA256=' + shared.sha(TX / 'MODIFIED_FILE.exe'),
        'DELIVERY=' + str(shared.RELEASE / 'RelinkStudio.exe'),
        'Restored behavior/status: isolated original package hashes, offscreen UI and SQLite passed; unmanaged user data retained.',
        'Rollback restores program files only; existing game favorites and user config are not reverted.',
        'Fixed unpacked release remains MODIFIED. No visible frontend was launched.',
        'LIVE_BUSINESS_STATUS=' + live['status'],
        'TASK_FILE_FULLY_COMPLETED=' + str(live['task_file_fully_completed']).lower(),
        'COMPLETED_RULES=' + str(live['completed_rule_count']) + '/' + str(live['configured_rule_count']),
        'LATEST_BLOCKER=COLLECTION_CONFIRMED_STATE_CONFLICT; historical confirmed 230, current white 320; no new toggle.',
        'SPEED_SAMPLE=run_03 observed steps, not a total-cycle or BBZPS comparative acceleration factor.',
        'Frontend Run is not connected to the live collection CLI; this transaction does not claim otherwise.',
        'Price provider never accepts expected values or configuration thresholds as recognition input.',
        'Historical in-memory 480 regression is not a new actionable game observation.',
        'LIVE_RESULTS=' + str(TX / 'live_result.json'),
        'Detailed speed comparison=' + str(TX / 'after_timing.json')]
    for name in ('MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh'):
        lines.append(name + '=' + str(TX / name))
    for entry in commands:
        lines.extend(['', entry['label'], 'COMMAND: ' + entry['command'],
            'INPUT: ' + entry['input'], 'STDOUT:', entry['stdout'],
            'STDERR:', entry['stderr'], 'EXIT_STATUS: ' + str(entry['exit_status'])])
    lines += ['', 'PROGRAM_ROLLBACK_TEST=PASS', 'DELIVERY_REMAINS_MODIFIED=true']
    (TX / 'VERIFICATION.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    reopened = {}
    for name in ('MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh'):
        path = TX / name
        data = path.read_bytes()
        assert data
        reopened[name] = dict(path=str(path), bytes=len(data), sha256=shared.sha(path))
    shared.write_json(TX / 'reopened_artifacts.json', reopened)
    print('COLLECTION_SPEED_DELIVERY=PASS; rollback=PASS; release_remains_modified=true')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('verify', 'finalize'))
    args = parser.parse_args()
    configure()
    if args.phase == 'verify':
        shared.verify(argparse.Namespace(build_dir=ROOT / 'build_relocated'))
        h.run('CAPTURE_SERVER_TESTS', [str(ROOT / 'build_relocated/live_capture_server_tests.exe'),
            str(shared.RELEASE / 'RelinkStudio.exe')],
            'Invalid/synthetic requests only; real hidden server reuse/EOF; no game input.',
            'LIVE_CAPTURE_SERVER_TESTS=PASS')
    else:
        finalize()


if __name__ == '__main__':
    main()
