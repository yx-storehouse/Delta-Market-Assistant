"""Offline build/package/rollback evidence for current market page calibration."""
import argparse
import json
import os
from pathlib import Path
import sys
import verify_m2_capture_package as base
ROOT=Path(__file__).resolve().parents[2]
base.TX=ROOT/'artifacts/m2_market_calibration'
base.BASELINE=base.TX/'baseline/release'
base.h.TX,base.h.BASELINE=base.TX,base.BASELINE
base.PHASE_TITLE='M2 market angle/ROI calibration and original empty-watchlist route'
base.CHANGED_BOUNDARIES='Windows OCR TextAngle coordinate mapping; same-frame 2x contrast ROI; bounded local exact-label joining; market page diagnostics; original route replay and one-focus live batch.'
base.PHASE_NOTES=[
 'Original business order preserved: observe watchlist twice, return skin home, then open catalog filter.',
 'Manual test navigation only; no purchase, inventory movement, watchlist modification or filter selection.',
 'Pixels remain in memory. Debug output is allowlisted text geometry, not full OCR or images.',
 'Each complete live batch owns foreground once and restores IDE once; failed attempts remain in evidence.',
 'Recorded label replay is not live runtime integration or full S01-S39 completion; nonempty lists and business field extraction remain unvalidated.'
]
base.EXTRA_REQUIRED_RECORDS=['MARKET_CALIBRATION_TESTS','STARTUP_OBSERVER_TESTS','PACKAGED_STARTUP_SELF_TEST','FOREGROUND_POLICY_TESTS','FOREGROUND_BATCH_TESTS']

def verify():
 base.verify();env=base.h.environment(base.RELEASE);env['QT_PLUGIN_PATH']=str(base.RELEASE)
 for name,fixture in [('market_calibration_tests','market_live_20261007.json'),('startup_observer_tests','startup_pages.json'),('lobby_calibration_tests','lobby_live_20261007.json')]:
  base.h.run(name.upper(),[str(base.BUILD/(name+'.exe')),str(ROOT/'docs/business_rebuild/implementation/runtime/fixtures'/fixture)],
      'Frozen text geometry plus negative fixtures; no live capture.',name.upper()+'=PASS',env=env)
 base.h.run('PACKAGED_STARTUP_SELF_TEST',[str(base.RELEASE/'RelinkStudio.exe'),'--startup-observer-self-test'],
     'Embedded historical and market label projections and original-route replay; no real game capture.','STARTUP_OBSERVER_SELF_TEST=PASS')
 base.h.run('FOREGROUND_POLICY_TESTS',[str(base.BUILD/'foreground_policy_tests.exe')],
     'Focus callbacks only, no OS focus changes.','FOREGROUND_POLICY_TESTS=PASS',env=env)
 base.h.run('FOREGROUND_BATCH_TESTS',[sys.executable,'-X','utf8',str(ROOT/'tests/manual/test_foreground_batch_core.py')],
     'Pure batch sequencing and interruption tests.',env=os.environ.copy())

def finalize():
 base.h.run('MARKET_LIVE_EVIDENCE',[sys.executable,'-X','utf8',str(ROOT/'tests/manual/verify_market_evidence.py')],
     'Reopen saved live records and compare current EXE/helper hashes; no new focus or capture.','MARKET_LIVE_EVIDENCE=PASS',env=os.environ.copy())
 base.finalize();p=base.TX/'VERIFICATION.txt';text=p.read_text(encoding='utf-8')
 added=json.loads((base.TX/'added_release_files.json').read_text(encoding='utf-8'))
 text=text.replace('user sentinel preserved; new helper removed.',f'user sentinel preserved; actual added managed files removed={len(added)}.')
 text+='\nLIVE BATCH RECORDS: commands, bounded plan inputs, literal results, child exit statuses and final restore\n'
 for path in sorted(base.TX.glob('live_*.json')):text+='\n'+path.name+'\n'+path.read_text(encoding='utf-8')+'\n'
 text+='\nLegacy rollback stdout added_helper_removed is retained; actual additions count is stated above.\n'
 p.write_text(text,encoding='utf-8')
 for name in ['MODIFIED_FILE.exe','DIFF_FILE','VERIFICATION.txt','ROLLBACK.sh']:assert (base.TX/name).read_bytes()
 print('MARKET_TRANSACTION_REOPEN=PASS')

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('phase',choices=['build','verify','finalize']);phase=p.parse_args().phase
 base.build() if phase=='build' else globals()[phase]()
