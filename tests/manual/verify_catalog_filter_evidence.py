"""Reopen recorded calibration evidence; never capture or drive the game."""
import hashlib
import json
from pathlib import Path
from foreground_batch_core import filter_expectation_matches
ROOT=Path(__file__).resolve().parents[2]
TX=ROOT/'artifacts/m2_catalog_filter'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 records=[];clicks=captures=failures=0
 for path in sorted(TX.glob('live_*.json')):
  data=json.loads(path.read_text(encoding='utf-8'))
  if 'steps' not in data or 'plan' not in data:continue
  assert data['enter_calls']==data['leave_calls']==1 and data['ide_restored'],path.name
  assert data['image_file_writes']==0
  assert data['manual_clicks']==sum(s['kind']=='click' for s in data['steps'])
  for step in data['steps']:
   if step['kind']!='capture':continue
   for attempt in step.get('attempts',[step]):
    result=attempt['result'];assert result['focus_activation_requests']==result['focus_restore_requests']==0
    assert result['image_file_writes']==0 and not result['game_input_sent']
    assert 'preview_png_base64' not in result
    captures+=1
  failures+=not data['passed'];clicks+=data['manual_clicks'];records.append(path.name)
 final=json.loads((TX/'live_final_filter_states.json').read_text(encoding='utf-8'))
 assert final['passed'] and final['binary_sha256']==sha(ROOT/'dist/RelinkStudio/RelinkStudio.exe')
 assert final['ocr_helper_sha256']==sha(ROOT/'dist/RelinkStudio/vision/windows_ocr_worker.ps1')
 for planned,step in zip(final['plan']['steps'],final['steps']):
  if step['kind']=='capture':assert filter_expectation_matches(step['result'],planned['expected_filter'])
 first=final['steps'][0]['result']['catalog_filter_state'];last=final['steps'][-1]['result']['catalog_filter_state']
 assert first['season_label']==last['season_label']
 assert {k:v['state'] for k,v in first['checkboxes'].items()}=={k:v['state'] for k,v in last['checkboxes'].items()}
 fixture=json.loads((ROOT/'docs/business_rebuild/implementation/runtime/fixtures/catalog_filter_signals_20261007.json').read_text(encoding='utf-8'))
 for source in fixture['sources']:assert sha(ROOT/source['path'])==source['sha256']
 assert {c['checkbox'] for c in fixture['cases'] if c['expected_state']=='checked'}=={'owned','unowned','legendary','epic','rare','common'}
 assert not fixture['pixels_included'] and not fixture['independent_accuracy_dataset']
 result=dict(passed=True,records=records,actual_captures=captures,manual_checkbox_clicks=clicks,
  retained_failed_batches=failures,final_season_and_checkbox_values_restored=True,final_binary_verified=True,
  six_checkbox_selected_signals_covered=True,signal_fixture_cases=len(fixture['cases']),image_file_writes=0)
 (TX/'live_evidence_verification.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
 print('CATALOG_FILTER_LIVE_EVIDENCE=PASS; '+json.dumps(result))
if __name__=='__main__':main()
