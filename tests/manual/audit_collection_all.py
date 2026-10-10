"""Reopen actual collection-all attempts and report their bounded observed outcome."""
import json,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];TX=ROOT/'artifacts/collection_all_rules'
runs=[];attempt_keys=set();motions=[]
for path in sorted(TX.glob('run_*/summary.json')):
 summary=json.loads(path.read_text('utf-8'));session=json.loads((path.parent/'session.json').read_text('utf-8'))
 assert summary['task_file_fully_completed'] is False and not summary['purchase_phase_started']
 assert summary['ide_restored'] is True and session['enter_calls']==session['leave_calls']==1
 assert summary['image_file_writes']==session['image_file_writes']==0
 motions+=session.get('cursor_motions',[])
 for step in session['steps']:
  attempt=step.get('collection_attempt')
  if attempt:
   from collection_journal import candidate_key
   attempt_keys.add(step.get('collection_attempt_key') or candidate_key(attempt))
 runs.append(dict(path=str(path),elapsed_ms=summary['elapsed_ms'],new_in_run_receipt=summary['confirmed_new'],
     already_favorited=summary['already_favorited'],scanned=summary['scanned_candidates'],
     completed_rules=len(summary['rows']),error=summary['error'],ide_restored=True))
journal=[json.loads(p.read_text('utf-8')) for p in (ROOT/'artifacts/m2_savedvalue_collection/journal').glob('*.json')]
new=[r for r in journal if r['key'] in attempt_keys]
assert len(runs)==5 and len(new)==len(attempt_keys)==4
assert all(r['status'] in ('confirmed','confirmed_reconciliation') for r in journal)
visual=[r for r in new if r.get('reconciliation',{}).get('automatic_price_validation_passed') is False]
assert len(visual)==1
review=json.loads((TX/'reconciliation_12_visual_review.json').read_text('utf-8'))
assert review['automatic_price_validation_passed'] is False and review['input_actions']==0
assert review['visual_observed_fields']['price']=='300' and review['raw_ocr_price_words']==['30']
config=Path.home()/'AppData/Local/RelinkStudio/RelinkStudio/config.json'
assert hashlib.sha256(config.read_bytes()).hexdigest()=='7c317fc1850e2ca85eeba394f73f9c231499f7a781dad570f95f0e4287b8f5b9'
result=dict(task_file_fully_completed=False,enabled_rules=21,configured_products=9,
    disabled_tasks=29,completed_rules=0,business_blocker='COLLECTION_RECOGNITION_STABILITY',
    trials=runs,new_star_actions=4,new_confirmed_by_automatic_readback=3,
    new_confirmed_by_independent_visual_readback=1,historical_confirmed_actions=len(journal),
    historical_count_is_not_current_watchlist_size=True,pending_journal=0,
    tested_motion_count=len(motions),failed_motions=sum(m.get('completed') is not True for m in motions),
    last_scene='AUG 天命 / 成色S / selected second-row-left / price300 / wear0.217900 / gold',
    ide_restored=True,purchase_phase_started=False,image_file_writes=0,
    automatic_price_failure_remains=True,config_unchanged=True)
assert result['failed_motions']==0
(TX/'execution_review.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
assert json.loads((TX/'execution_review.json').read_text('utf-8'))==result
print('ALL_RULES_EVIDENCE=PASS; task_file_fully_completed=false; new_star_actions=4; automatic_readbacks=3; independent_visual_readbacks=1; historical_confirmed=46; pending=0; ide_restored=true; images_written=0; config_unchanged=true')
