"""Collection-only trial delivery: preserved baseline, offscreen QA and rollback."""
import argparse
import difflib
import json
import os
from pathlib import Path
import sys

import verify_pr12a_package as h
import verify_m2_capture_package as package

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/collection_all_rules'
BASELINE = TX / 'baseline/release'
h.TX, h.BASELINE = TX, BASELINE
package.TX, package.BASELINE = TX, BASELINE


def build():
    h.build()


def verify():
    package.verify()
    h.run('MODIFIED_COLLECTION_TESTS', [sys.executable, '-X', 'utf8', '-m', 'unittest', 'discover',
        '-s', 'tests/manual', '-p', 'test_collection*.py'], 'All offline collection/session/current-config/trial regressions.', env=os.environ.copy())
    h.run('MODIFIED_BATCH_TESTS', [sys.executable, '-X', 'utf8', '-m', 'unittest', 'discover',
        '-s', 'tests/manual', '-p', 'test_foreground*.py'], 'Legacy finite batch semantics retained.', env=os.environ.copy())
    h.run('MODIFIED_CURSOR_TESTS', [sys.executable, '-X', 'utf8', '-m', 'unittest', 'discover',
        '-s', 'tests/manual', '-p', 'test_cursor_motion.py'],
        'Pure cursor mocks, synthetic geometry, and actual local high-resolution/quantized clocks; no OS input.', env=os.environ.copy())
    h.run('MODIFIED_LAYOUT_TESTS', [str(package.BUILD / 'collection_layout_tests.exe'),
        str(TX / 'layout_probe_04.json')],
        'Synthetic raw detector cases plus four recorded live numeric edge profiles; no screenshot reconstruction or game input.',
        'COLLECTION_LAYOUT_TESTS=PASS')
    h.run('LIVE_EVIDENCE_REVIEW', [sys.executable, '-X', 'utf8', 'tests/manual/audit_collection_all.py'],
        'Reopen five actual failed full-config attempts and four durable additions; distinguish three automated readbacks from one independent visual review. No game input.',
        'ALL_RULES_EVIDENCE=PASS', env=os.environ.copy())


def finalize():
    records = json.loads((TX / 'commands.json').read_text(encoding='utf-8'))
    latest = {r['label']: r for r in records}
    for label in ('BASELINE', 'BASELINE_STORAGE', 'MODIFIED_BUILD', 'MODIFIED', 'WINDOWS_OCR_TESTS',
                  'MODIFIED_COLLECTION_TESTS', 'MODIFIED_BATCH_TESTS', 'MODIFIED_CURSOR_TESTS',
                  'MODIFIED_LAYOUT_TESTS', 'ROLLBACK', 'RESTORED'):
        assert latest[label]['exit_status'] == 0, label
    assert latest['LIVE_EVIDENCE_REVIEW']['exit_status'] == 0
    original = json.loads((TX / 'baseline_source_hashes.json').read_text(encoding='utf-8'))
    paths = set(original)
    for directory in ('src', 'tests', 'docs'):
        paths.update(p.relative_to(ROOT).as_posix() for p in (ROOT / directory).rglob('*')
                     if p.is_file() and '__pycache__' not in p.parts and p.suffix not in ('.pyc', '.exe', '.dll', '.png', '.jpg'))
    paths.update(name for name in ('CMakeLists.txt', 'build.ps1', 'SESSION_START.md', 'resources.qrc')
                 if (ROOT / name).is_file())
    patch = []
    changed = []
    for name in sorted(paths):
        path = ROOT / name
        if name in original and path.is_file() and h.sha(path) == original[name]:
            continue
        before = TX / 'baseline/source' / name
        a = before.read_text(encoding='utf-8-sig').splitlines(True) if before.exists() else []
        b = path.read_text(encoding='utf-8-sig').splitlines(True) if path.exists() else []
        changed.append(name)
        patch.extend(difflib.unified_diff(a, b, fromfile='a/' + name, tofile='b/' + name))
    (TX / 'DIFF_FILE').write_text(''.join(patch), encoding='utf-8')
    lines = ['Delta Market Assistant - all-config collection and cursor trajectory', 'Date: 2026-10-07', 'Changed branch: main',
        'Changed fields: dynamic same-frame card layout/scroll; full enabled-task traversal; deterministic cursor trajectory; full collection field binding and evidence.',
        'The OCR helper persists within one diagnostic process, not across separate capture EXEs. No unsupported throughput claim.',
        'Original BBZPS executable was not run. No purchase phase or inventory movement is implemented by this driver.',
        'Restored behavior/status: original package hashes, offscreen UI/workspace/SQLite on isolated copy, user sentinel preserved.',
        'Program rollback does not reverse game favorites. Fixed unpacked release remains modified.',
        'LIVE BUSINESS NOT COMPLETE: all 21 configured rules were scheduled but no full rule finished. Recognition stability stopped each run; no all-gun completion is claimed.',
        'Four new star actions: three confirmed by automated fresh-frame readback, one independently checked from the in-memory preview. That last automatic OCR failure remains failed; no digit was substituted.',
        'DELIVERY: ' + str(h.RELEASE / 'RelinkStudio.exe'), 'BASELINE_SHA256: ' + h.sha(BASELINE / 'RelinkStudio.exe'),
        'MODIFIED_SHA256: ' + h.sha(TX / 'MODIFIED_FILE.exe')]
    for name in ('MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh'):
        lines.append(name + ': ' + str(TX / name))
    lines += ['SOURCE_FILES:', *changed]
    lines += ['', 'EXECUTION_REVIEW:', (TX / 'execution_review.json').read_text(encoding='utf-8')]
    for summary in sorted(TX.glob('run_*/summary.json')):
        lines += ['', 'LIVE_RUN: ' + str(summary), summary.read_text(encoding='utf-8')]
    for r in records:
        lines += ['', r['label'], 'COMMAND: ' + r['command'], 'INPUT: ' + r['input'], 'STDOUT:',
                  r['stdout'].rstrip() or '(empty)', 'STDERR:', r['stderr'].rstrip() or '(empty)', 'EXIT_STATUS: ' + str(r['exit_status'])]
    (TX / 'VERIFICATION.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for name in ('MODIFIED_FILE.exe', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh'):
        assert (TX / name).read_bytes()
        print('REOPEN=PASS; path=' + str(TX / name))
    assert h.sha(TX / 'MODIFIED_FILE.exe') == h.sha(h.RELEASE / 'RelinkStudio.exe')
    assert h.sha(BASELINE / 'RelinkStudio.exe') == h.sha(TX / 'rollback_test/RelinkStudio.exe')
    print('COLLECTION_TRIAL_TRANSACTION=PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('build', 'verify', 'finalize'))
    globals()[parser.parse_args().phase]()

