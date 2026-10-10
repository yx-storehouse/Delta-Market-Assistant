"""Run actual frozen/delivered/restored Python policy with a no-input backend."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/collection_roi_speed'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('phase', choices=('baseline', 'modified', 'rollback'))
    args = parser.parse_args()
    roots = dict(baseline=TX/'baseline/release', modified=ROOT/'dist/RelinkStudio',
                 rollback=TX/'rollback_test')
    runtime = roots[args.phase] / 'collection'
    os.environ['RELINK_PROJECT_ROOT'] = str(ROOT)
    sys.path.insert(0, str(runtime))
    import run_collection_observed as observed
    from collection_live_session import WindowsBackend
    assert Path(observed.__file__).resolve() == runtime / 'run_collection_observed.py'
    backend = object.__new__(observed.MemoryReviewBackend)
    backend.local_title = object()
    backend.local_price = None
    command = ['fixture', '--collection-observation']
    original = command[:]
    reply = subprocess.CompletedProcess(command, 1, json.dumps(dict(
        capture_passed=False, error='OFFLINE_TEST_PACKET', image_file_writes=0)).encode(), b'')
    with patch.object(WindowsBackend, 'capture', return_value=reply) as capture:
        result = backend.capture(command, 10)
    sent = capture.call_args.args[0]
    changed = args.phase == 'modified'
    assert ('--preview-stdout' in sent) is not changed
    assert ('--collection-local-text' in sent) is changed
    assert '--collection-title-image' in sent and '--collection-catalog-image' in sent
    assert command == original and result.returncode == 1
    assert json.loads(result.stdout)['error'] == 'OFFLINE_TEST_PACKET'
    facts = dict(phase=args.phase, full_preview='--preview-stdout' in sent,
        duplicate_native_text_omitted='--collection-local-text' in sent,
        module_sha256=hashlib.sha256(Path(observed.__file__).read_bytes()).hexdigest(),
        game_actions=0, actual_backend_requests=0, passed=True)
    path = TX / ('policy_' + args.phase + '.json')
    path.write_text(json.dumps(facts, indent=2) + '\n', 'utf-8')
    assert json.loads(path.read_text('utf-8')) == facts
    print(args.phase.upper() + '_POLICY=PASS')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
