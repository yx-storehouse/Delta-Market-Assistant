"""Export bounded numeric checkbox signals, never pixels, from recorded batches.

Expected labels come from the predeclared manual test plan, not its classifier.
This is signal replay evidence, not an independent accuracy/holdout dataset.
"""
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
TX=ROOT/'artifacts/m2_catalog_filter'
RECORDS=['live_verified_states_v2.json','live_final_filter_states.json']
def main():
 cases=[];sources=[]
 for name in RECORDS:
  path=TX/name
  if not path.exists():continue
  data=json.loads(path.read_text(encoding='utf-8'))
  assert data['passed'] and data['ide_restored'] and data['enter_calls']==data['leave_calls']==1
  source=path.relative_to(ROOT).as_posix();digest=hashlib.sha256(path.read_bytes()).hexdigest()
  sources.append({'path':source,'sha256':digest,'binary_sha256':data['binary_sha256']})
  for index,(step,planned) in enumerate(zip(data['steps'],data['plan']['steps'])):
   if step['kind']!='capture':continue
   result=step['result'];state=result['catalog_filter_state']
   assert result['filter_expectation_passed'] and state['complete']
   assert state['frame_sha256']==result['frames'][0]['sha256']
   for key,box in state['checkboxes'].items():
    expected=planned['expected_filter'][key]
    assert expected==box['state']
    cases.append(dict(source_record=source,source_step_index=index,frame_sha256=state['frame_sha256'],
        checkbox=key,expected_state=expected,measurement=box['measurement']))
 assert cases
 out=ROOT/'docs/business_rebuild/implementation/runtime/fixtures/catalog_filter_signals_20261007.json'
 out.write_text(json.dumps(dict(schema='catalog-checkbox-signals-v1',pixels_included=False,
     independent_accuracy_dataset=False,sources=sources,cases=cases),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 assert json.loads(out.read_text(encoding='utf-8'))['cases']==cases
 print(f'CATALOG_SIGNAL_EXPORT=PASS; cases={len(cases)}; sources={len(sources)}; pixels_included=false')
if __name__=='__main__':main()
