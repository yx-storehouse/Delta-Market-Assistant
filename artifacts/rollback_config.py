from pathlib import Path
import hashlib
import json
import sys

root = Path(__file__).resolve().parent
target = Path(sys.argv[1]).resolve()
if target.parent != root or target.suffix != '.json':
    raise SystemExit('Expected a JSON fixture inside artifacts.')
hashes = json.loads((root / 'fixture_hashes.json').read_text(encoding='utf-8'))
baseline = (root / 'BASELINE.json').read_bytes()
assert hashlib.sha256(baseline).hexdigest() == hashes['baseline']
assert hashlib.sha256(target.read_bytes()).hexdigest() == hashes['modified']
target.write_bytes(baseline)
assert target.read_bytes() == baseline
assert hashlib.sha256((root / 'MODIFIED_FILE.json').read_bytes()).hexdigest() == hashes['modified']
print('ROLLBACK=PASS; restored_equal_baseline=true; modified_left_changed=true')
