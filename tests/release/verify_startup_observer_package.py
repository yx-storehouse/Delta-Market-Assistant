"""Reuse package/rollback acceptance with this phase's own frozen transaction."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import verify_m2_capture_package as base

ROOT = Path(__file__).resolve().parents[2]
base.TX = ROOT / 'artifacts/m2_startup_observer_transaction'
base.BASELINE = base.TX / 'baseline/release'
base.h.TX, base.h.BASELINE = base.TX, base.BASELINE
base.PHASE_TITLE = 'M2 original startup page classification and read-only observer'
base.CHANGED_BOUNDARIES = 'SkinPageClassifier; StartupObserver S04-S10/S32-S33; --startup-observer-self-test; live OCR diagnostic page summary; helper-PID UI checks.'
base.PHASE_NOTES = [
    'This phase performed NO live game capture, focus switching or game input; current-version live calibration awaits user preparation.',
    '27 historical OCR projections are text/geometry fixtures, not image recognition accuracy measurements.',
    'Original watchlist precheck and existing-list continuation are implemented as read-only observation checkpoints; no navigation executor or purchase authorization.',
    'Windows OCR helper focus validation is PID-scoped so user app switching is not misattributed to the helper.',
]
base.EXTRA_REQUIRED_RECORDS = ['STARTUP_OBSERVER_TESTS', 'PACKAGED_STARTUP_SELF_TEST', 'DIAGNOSTIC_MODE_CONFLICT']


def verify():
    base.verify()
    env = base.h.environment(base.RELEASE)
    env['QT_PLUGIN_PATH'] = str(base.RELEASE)
    base.h.run('STARTUP_OBSERVER_TESTS', [str(base.BUILD / 'startup_observer_tests.exe'),
        str(ROOT / 'docs/business_rebuild/implementation/runtime/fixtures/startup_pages.json')],
        'Historical OCR projections plus synthetic negative cases; no game/image capture.', 'STARTUP_OBSERVER_TESTS=PASS', env=env)
    base.h.run('PACKAGED_STARTUP_SELF_TEST', [str(base.RELEASE / 'RelinkStudio.exe'), '--startup-observer-self-test'],
        'Packaged embedded historical fixtures; 27 page checks and 5 original route checks; QCoreApplication only.', 'STARTUP_OBSERVER_SELF_TEST=PASS')
    args = [str(base.RELEASE / 'RelinkStudio.exe'), '--startup-observer-self-test', '--live-capture-check']
    result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, timeout=10,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    assert result.returncode == 2 and 'E_DIAGNOSTIC_MODE_CONFLICT' in result.stderr
    path = base.TX / 'commands.json'
    records = json.loads(path.read_text(encoding='utf-8'))
    records.append(dict(label='DIAGNOSTIC_MODE_CONFLICT', command=subprocess.list2cmdline(args),
        input='Both offline and live flags; expected early refusal before any window binding/capture.',
        stdout=result.stdout, stderr=result.stderr, exit_status=result.returncode,
        expected_exit_status=2, assertion_passed=True))
    path.write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('DIAGNOSTIC_MODE_CONFLICT=PASS; actual_exit=2; expected_exit=2')


def finalize():
    # Expected negative command is intentionally exit 2, not rewritten as exit 0.
    base.EXTRA_REQUIRED_RECORDS = ['STARTUP_OBSERVER_TESTS', 'PACKAGED_STARTUP_SELF_TEST']
    records = json.loads((base.TX / 'commands.json').read_text(encoding='utf-8'))
    negative = next(r for r in reversed(records) if r['label'] == 'DIAGNOSTIC_MODE_CONFLICT')
    assert negative['exit_status'] == 2 and negative['assertion_passed']
    base.finalize()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=['build', 'verify', 'finalize'])
    phase = parser.parse_args().phase
    base.build() if phase == 'build' else globals()[phase]()
