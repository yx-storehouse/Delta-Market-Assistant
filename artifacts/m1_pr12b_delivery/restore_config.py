from pathlib import Path
import shutil,sys
root=Path(__file__).resolve().parent
target=Path(sys.argv[1]).resolve()
assert target == root/'ROLLBACK_TEST.json', 'isolated target required'
shutil.copy2(root/'BASELINE.json', target)
assert target.read_bytes()==(root/'BASELINE.json').read_bytes()
print('CONFIG_ROLLBACK=PASS')
