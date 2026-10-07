"""Reopen current market calibration evidence, without capturing or switching focus."""
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TX=ROOT/'artifacts/m2_market_calibration'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
records={}
for p in TX.glob('live_*.json'):
 record=json.loads(p.read_text(encoding='utf-8'))
 # The verifier also writes live_evidence_verification.json; it is a summary,
 # not an executable batch, and must not be fed back as a capture record.
 if 'steps' in record and 'plan' in record:records[p.name]=record
for name,r in records.items():
 assert r['enter_calls']==r['leave_calls']==1 and r['ide_restored'],name
 assert r['image_file_writes']==0,name
 for step in r['steps']:
  if step['kind']!='capture':continue
  for attempt in step.get('attempts',[step]):
   p=attempt['result'];assert p['focus_activation_requests']==p['focus_restore_requests']==0,name
   assert not p['game_input_sent'] and p['image_file_writes']==0 and p['resources_drained'],name
   assert 'preview_png_base64' not in p,name
fixture=json.loads((ROOT/'docs/business_rebuild/implementation/runtime/fixtures/market_live_20261007.json').read_text(encoding='utf-8'))
for case in fixture['cases']:
 source=ROOT/case['source_record'];assert sha(source)==case['source_record_sha256']
 r=json.loads(source.read_text(encoding='utf-8'));p=r['steps'][case['source_step']]['result']
 assert p['frames'][0]['sha256']==case['source_frame_sha256']
 assert p['market_anchor_diagnostics']['words']==case['observation']['words']
final=records['live_final_inverted_route.json']
assert final['passed'] and final['manual_clicks']==4
assert final['binary_sha256']==sha(ROOT/'dist/RelinkStudio/RelinkStudio.exe')
assert final['ocr_helper_sha256']==sha(ROOT/'dist/RelinkStudio/vision/windows_ocr_worker.ps1')
captures=[s for s in final['steps'] if s['kind']=='capture']
assert [s['result']['startup_page']['page'] for s in captures]==[
 'empty_watchlist','empty_watchlist','skin_home','empty_watchlist','empty_watchlist','skin_home','catalog_filter']
assert all(s['passed'] and s['result']['page_match_passed'] and s['exit_status']==0 for s in captures)
assert len({s['result']['frames'][0]['sha256'] for s in captures})==len(captures)
target=final['identities']['target_hwnd'];ide=final['identities']['return_hwnd']
sequence=[x['hwnd'] for x in final['foreground_transitions']]
assert sequence[sequence.index(target):]==[target,ide]
assert not records['live_empty_to_filter.json']['passed']
assert not records['live_final_roundtrip.json']['passed']
assert not records['live_final_stable_route.json']['passed']
actual_captures=sum(s.get('attempt_count',1) for s in captures)
result=dict(passed=True,batches_checked=len(records),fixture_projections=6,
 final_matched_checkpoints=len(captures),final_capture_attempts=actual_captures,final_navigation_clicks=4,
 entry_calls=1,restore_calls=1,child_focus_switches=0,images_written=0,new_capture=False,
 current_binary_sha256=final['binary_sha256'],current_helper_sha256=final['ocr_helper_sha256'],
 note='Page calibration and manual navigation, not full automatic S01-S39 or transaction execution. Failed attempts retained.')
p=TX/'live_evidence_verification.json';p.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
assert json.loads(p.read_text(encoding='utf-8'))==result
print(f'MARKET_LIVE_EVIDENCE=PASS; matched_checkpoints=7; capture_attempts={actual_captures}; navigation_clicks=4; enter=1; restore=1; new_capture=false')
