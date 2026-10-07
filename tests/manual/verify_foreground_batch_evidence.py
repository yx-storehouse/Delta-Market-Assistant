"""Validate recorded live batch focus evidence without another foreground switch."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
TX=ROOT/'artifacts/m2_lobby_calibration'
r=json.loads((TX/'foreground_batch_live.json').read_text(encoding='utf-8'))
assert r['binary_sha256']==hashlib.sha256((ROOT/'dist/RelinkStudio/RelinkStudio.exe').read_bytes()).hexdigest()
assert r['passed'] and r['ide_restored'] and r['enter_calls']==r['leave_calls']==1
assert r['manual_clicks']==r['image_file_writes']==0 and len(r['steps'])==2
for step in r['steps']:
    p=step['result']
    assert step['passed'] and step['exit_status']==0 and p['capture_passed'] and p['ocr_passed']
    assert p['focus_policy']=='caller-owned' and p['foreground_cleanup_owner']=='batch_caller'
    assert p['focus_activation_requests']==p['focus_restore_requests']==0
    assert p['target_foreground_retained'] and not p['ide_foreground_restored']
    assert not p['game_input_sent'] and p['image_file_writes']==0 and p['resources_drained']
sequence=[item['hwnd'] for item in r['foreground_transitions']]
target=r['identities']['target_hwnd']; ide=r['identities']['return_hwnd']
assert sequence[sequence.index(target):]==[target,ide]
summary=dict(passed=True,enter_calls=1,leave_calls=1,child_focus_switch_requests=0,captures=2,
    observed_sequence_after_game_entry=[target,ide],sampling_interval_ms=r['foreground_sampling_interval_ms'],
    business_page_validation_passed=False,new_capture=False,
    note='The current Mandel page remains unknown; this test validates batch focus/capture only, not that page classifier.')
p=TX/'foreground_batch_verification.json';p.write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
assert json.loads(p.read_text(encoding='utf-8'))==summary
print('FOREGROUND_BATCH_LIVE=PASS; captures=2; enter=1; restore=1; child_focus_switch_requests=0; new_capture=false')
