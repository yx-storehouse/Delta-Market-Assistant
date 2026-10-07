"""Catalog filter observation transaction; no live input from this verifier."""
import argparse
import json
import os
import sys
from pathlib import Path
import verify_m2_capture_package as base
ROOT=Path(__file__).resolve().parents[2]
base.TX=ROOT/'artifacts/m2_catalog_filter'
base.BASELINE=base.TX/'baseline/release'
base.h.TX,base.h.BASELINE=base.TX,base.BASELINE
base.PHASE_TITLE='M2 same-frame catalog filter state and original ordered difference plan'
base.CHANGED_BOUNDARIES='CatalogFilterReader: season/ownership/grade tri-state; CatalogFilterPlan: S10-S14 readback boundaries; explicit --catalog-filter-state; quiet delivery preference.'
base.PHASE_NOTES=[
 'Current geometry profile is 2560x1440, 144 DPI only. Unsupported geometry stays Unknown.',
 'Pixels stay in memory. Filter target snapshot is explicit, never inferred from demonstration tasks.',
 'Read-only difference plans are not a game input executor. Normal frontend remains configuration/replay.',
 'Real live evidence is recorded separately, including temporary checkbox calibration and restoration if performed.'
]
base.EXTRA_REQUIRED_RECORDS=['CATALOG_FILTER_TESTS','FOREGROUND_BATCH_TESTS']
def verify():
 base.verify()
 base.h.run('CATALOG_FILTER_TESTS',[str(base.BUILD/'catalog_filter_tests.exe'),str(ROOT/'docs/business_rebuild/implementation/runtime/fixtures/catalog_filter_signals_20261007.json')],
     'Synthetic pixels/OCR and exact explicit target snapshots; no live input.','CATALOG_FILTER_TESTS=PASS')
 base.h.run('FOREGROUND_BATCH_TESTS',[sys.executable,'-X','utf8',str(ROOT/'tests/manual/test_foreground_batch_core.py')],
     'Pure foreground batch regression; no OS input.',env=os.environ.copy())
def finalize():
 base.h.run('CATALOG_FILTER_LIVE_EVIDENCE',[sys.executable,'-X','utf8',str(ROOT/'tests/manual/verify_catalog_filter_evidence.py')],
     'Reopen all real batches including failed attempts, numeric signals and current binary hashes; no live input.',
     'CATALOG_FILTER_LIVE_EVIDENCE=PASS',env=os.environ.copy())
 base.finalize();p=base.TX/'VERIFICATION.txt';report=p.read_text(encoding='utf-8')
 additions=json.loads((base.TX/'added_release_files.json').read_text(encoding='utf-8'))
 report=report.replace('user sentinel preserved; new helper removed.',f'user sentinel preserved; actual added managed files removed={len(additions)}.')
 for path in sorted(base.TX.glob('live_*.json')):
  report+='\nLIVE RECORD '+path.name+'\n'+path.read_text(encoding='utf-8')+'\n'
 p.write_text(report,encoding='utf-8')
 # Include the current communication preference (the shared base predates it).
 import difflib,subprocess
 revision=(base.TX/'baseline/source_revision.txt').read_text(encoding='ascii').strip()
 old=subprocess.check_output(['git','show',revision+':AGENTS.md'],cwd=ROOT).decode('utf-8')
 with (base.TX/'DIFF_FILE').open('a',encoding='utf-8') as f:
  f.write(''.join(difflib.unified_diff(old.splitlines(True),(ROOT/'AGENTS.md').read_text(encoding='utf-8').splitlines(True),fromfile='a/AGENTS.md',tofile='b/AGENTS.md')))
 for name in ['MODIFIED_FILE.exe','DIFF_FILE','VERIFICATION.txt','ROLLBACK.sh']:
  assert (base.TX/name).read_bytes()
 print('CATALOG_FILTER_TRANSACTION_REOPEN=PASS')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('phase',choices=['build','verify','finalize']);phase=p.parse_args().phase
 base.build() if phase=='build' else globals()[phase]()
