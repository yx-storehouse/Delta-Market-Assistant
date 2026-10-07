"""Export only already-recorded allowlisted label projections; no capture/images."""
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
TX=ROOT/'artifacts/m2_market_calibration'
specs=[('mandel','live_mandel_to_skin_home.json',0,'mandel'),
       ('skin_home','live_skin_home_to_watchlist.json',0,'skin_home'),
       ('empty_1','live_empty_to_filter_contrast.json',0,'empty_watchlist'),
       ('empty_2','live_empty_to_filter_contrast.json',1,'empty_watchlist'),
       ('home_after_empty','live_empty_to_filter_contrast.json',3,'skin_home'),
       ('catalog_filter','live_empty_to_filter_contrast.json',5,'catalog_filter')]
cases=[]
for identity,name,index,expected in specs:
    source=TX/name; record=json.loads(source.read_text(encoding='utf-8')); step=record['steps'][index]
    result=step['result']; projected=result['market_anchor_diagnostics']
    assert step['passed'] and result['startup_page']['page']==expected and not projected['truncated']
    assert result['image_file_writes']==0
    cases.append(dict(id=identity,source_record=source.relative_to(ROOT).as_posix(),
        source_record_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),source_step=index,
        source_binary_sha256=record['binary_sha256'],source_frame_sha256=result['frames'][0]['sha256'],
        expected_page=expected,observation=dict(width=projected['width'],height=projected['height'],
            coverage='full_client',words=projected['words'],protocol='windows-ocr-once-v1',language='zh-Hans-CN')))
data=dict(schema_version=1,source_kind='live_ocr_anchor_projection',pixels_included=False,
    note='Selected labels from full-client captures. Replay is not a new live test; excludes full OCR and account text.',cases=cases)
p=ROOT/'docs/business_rebuild/implementation/runtime/fixtures/market_live_20261007.json'
p.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
assert json.loads(p.read_text(encoding='utf-8'))==data
print('MARKET_FIXTURES=PASS; cases=6; image_files=0; new_capture=false')
