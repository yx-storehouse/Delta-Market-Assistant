"""SavedValue task import and collection-phase development transaction."""
import argparse
import json
import os
import sys
from pathlib import Path
import verify_m2_capture_package as base
ROOT=Path(__file__).resolve().parents[2]
base.TX=ROOT/'artifacts/m2_savedvalue_collection';base.BASELINE=base.TX/'baseline/release'
base.h.TX,base.h.BASELINE=base.TX,base.BASELINE
base.PHASE_TITLE='SavedValue task data and collection-first workflow'
base.CHANGED_BOUNDARIES='Bounded savedValue parser and wire-width metadata; dictionary resolution; receipt classifier; selected-card field association; pre-dispatch journal; bounded collection batch/row driver.'
base.PHASE_NOTES=['Original attachment unchanged. Existing specimen dump read as bytes only; no specimen code or process loaded.',
 'Explicit user task: perform collection through add-to-watchlist feedback, not just read-only recognition.',
 'Purchase is a later watchlist phase and is not entered by the collection runner. Actual live attempts recorded separately.',
 'Actual collection: 25 confirmed additions, zero pending. Source row 6, P90 Tianming B, card index 5 is already gold.',
 'Live workflow stopped at COLLECTION_UNSCANNED_SCROLL_REGION; remaining partial-row listings and remaining task rows are not marked complete.',
 'Release rollback never undoes game favorites. Final foreground was independently verified as Mirasim.']
base.EXTRA_REQUIRED_RECORDS=['SAVEDVALUE_TESTS','SAVEDVALUE_REAL_INPUT','COLLECTION_HELPERS','COLLECTION_LIVE_AUDIT']
def verify():
 base.verify()
 base.h.run('SAVEDVALUE_TESTS',[str(base.BUILD/'savedvalue_tests.exe'),str(ROOT/'docs/business_rebuild/implementation/domain/relink_0925_catalog.json')],
  'Synthetic data including truncations, duplicate keys and malformed tags; original public dropdown mapping.','SAVEDVALUE_TESTS=PASS')
 base.h.run('SAVEDVALUE_REAL_INPUT',[str(base.BUILD/'savedvalue_tests.exe'),str(ROOT/'docs/business_rebuild/implementation/domain/relink_0925_catalog.json'),str(base.TX/'input/source.savedValue')],
  'Read the preserved 8926-byte user file; exact decode/re-encode comparison; no specimen execution.','SAVEDVALUE_TESTS=PASS')
 base.h.run('COLLECTION_HELPERS',[sys.executable,'-X','utf8','-m','unittest','discover','-s','tests/manual','-p','test_collection*.py','-v'],
  'Offline same-frame labels, price/wear association, receipt identity, durable no-repeat journal; no game input.',env=os.environ.copy())
 base.h.run('COLLECTION_LIVE_AUDIT',[sys.executable,'-X','utf8',str(ROOT/'tests/manual/audit_collection_session.py')],
  'Read original input, preserved live failures, confirmed/reconciled journal; no new capture or input.','COLLECTION_LIVE_AUDIT=PASS',env=os.environ.copy())
def finalize():
 base.finalize()
 p=base.TX/'VERIFICATION.txt';s=p.read_text(encoding='utf-8').replace('user sentinel preserved; new helper removed.','user sentinel preserved; actual managed additions listed in added_release_files.json.')
 for path in sorted(base.TX.glob('live_*.json')):s+='\n'+path.name+'\n'+path.read_text(encoding='utf-8')+'\n'
 s+='\nCOLLECTION_SESSION_AUDIT\n'+(base.TX/'session_audit.json').read_text(encoding='utf-8')+'\n'
 p.write_text(s,encoding='utf-8')
 for name in ['MODIFIED_FILE.exe','DIFF_FILE','VERIFICATION.txt','ROLLBACK.sh']:assert (base.TX/name).read_bytes()
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('phase',choices=['build','verify','finalize']);phase=p.parse_args().phase
 base.build() if phase=='build' else globals()[phase]()
