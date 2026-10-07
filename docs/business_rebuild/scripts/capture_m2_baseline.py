"""Freeze a new M2 baseline without overwriting original/M1 evidence."""
from pathlib import Path
import argparse
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[3]
ART = ROOT / 'artifacts/business_rebuild_docs'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--id', default='m2_capture_startup_v2')
    parser.add_argument('--parent', default='m1_pr12b_baseline.json')
    parser.add_argument('--scope', default='M2 opt-in DXGI/Windows OCR foundation; packaged regression/rollback; static original startup S01-S39 reconstruction. No full live business claim.')
    args = parser.parse_args()
    if not re.fullmatch(r'm2_[a-z0-9_]+', args.id) or Path(args.parent).name != args.parent:
        raise SystemExit('Explicit local baseline identity required.')
    destination = ART / (args.id + '_baseline.json')
    if destination.exists():
        raise SystemExit('Baseline already exists; preserve it and choose an explicit new revision for subsequent changes.')
    parent = ART / args.parent
    previous = json.loads(parent.read_text(encoding='utf-8'))
    names = set(previous['files'])
    names.update({'resources.qrc', 'SESSION_START.md', 'README.md', '.gitignore', 'CMakeLists.txt', 'build.ps1'})
    names.discard('docs/business_rebuild/implementation/readiness/verification.json')
    for directory in ['src', 'tests', 'docs/business_rebuild']:
        for path in (ROOT / directory).rglob('*'):
            relative = path.relative_to(ROOT).as_posix()
            if relative == 'docs/business_rebuild/implementation/readiness/verification.json':
                continue  # generated timestamped test output, not immutable source
            if path.is_file() and '__pycache__' not in path.parts and path.suffix != '.pyc':
                names.add(path.relative_to(ROOT).as_posix())
    names.update(p.relative_to(ROOT).as_posix() for p in (ROOT / 'dist/RelinkStudio').rglob('*') if p.is_file())
    hashes = {name: digest(ROOT / name) for name in sorted(names)}
    original = json.loads((ART / 'project_baseline.json').read_text(encoding='utf-8'))
    original = original.get('files', original)
    drift = [name for name, value in original.items() if hashes.get(name) != value]
    data = dict(baseline_id=args.id, created_at='2026-10-07',
                parent_baseline=parent.relative_to(ROOT).as_posix(),
                scope=args.scope,
                files=hashes, expected_project_baseline_drift=sorted(drift))
    destination.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    assert json.loads(destination.read_text(encoding='utf-8')) == data
    print(f'M2_BASELINE=PASS; files={len(hashes)}; previous_preserved=true')


if __name__ == '__main__':
    main()
