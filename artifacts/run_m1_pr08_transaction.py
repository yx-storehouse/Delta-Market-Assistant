from __future__ import annotations
import hashlib,json,shutil,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent/'m1_pr08_transaction'
base=root/'BASELINE_COPY.json'; mod=root/'MODIFIED_FILE.json'; rb=root/'ROLLBACK_TEST_COPY.json'; ver=root/'VERIFICATION.txt'
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def verify(p, value, schema):
 d=json.loads(p.read_text(encoding='utf-8'))
 ok=(d['branch']=='m1_pr08' and d['field']=='profile_commit_state' and d['value']==value and d['config_schema']==schema and d['committable'] is False and d['actions_enabled'] is False and d['activation_required'] is True and d['source_hash_preserved'] is True and d['image_file_write_count']==0)
 out=f"value={d['value']} config_schema={d['config_schema']} committable={d['committable']} actions_enabled={d['actions_enabled']} activation_required={d['activation_required']} source_hash_preserved={d['source_hash_preserved']} image_file_write_count={d['image_file_write_count']}"
 return (0 if ok else 1),out
be,bo=verify(base,'review_only','LegacyImportPreview'); me,mo=verify(mod,'atomic_config_v2','ConfigV2')
shutil.copy2(mod,rb)
cmd=[r'C:/Program Files/Git/bin/bash.exe',str(root/'ROLLBACK.sh'),str(rb),str(base)]
proc=subprocess.run(cmd,cwd=root,capture_output=True,text=True)
re,ro=verify(rb,'review_only','LegacyImportPreview')
restored=digest(rb)==digest(base); changed=digest(mod)!=digest(base)
lines=['M1 PR08 TOOL TRANSACTION VERIFICATION','DATE=2026-10-06','TARGET=ProfileStore reviewed ConfigV2 atomic commit fixture','CHANGED_BRANCH=m1_pr08','CHANGED_FIELD=profile_commit_state (value/config_schema)','SAFEGUARD=committable=false; actions_enabled=false; activation_required=true; source_hash_preserved=true; image_file_write_count=0','', 'ARTIFACTS',f'MODIFIED_FILE={mod}',f'DIFF_FILE={root/"DIFF_FILE"}',f'VERIFICATION={ver}',f'ROLLBACK_SCRIPT={root/"ROLLBACK.sh"}',f'BASELINE_COPY={base}',f'ROLLBACK_TEST_COPY={rb}','',f'ORIGINAL_HASH_SHA256={digest(base)}',f'MODIFIED_HASH_SHA256={digest(mod)}','', 'BASELINE_COMMAND','python artifacts/run_m1_pr08_transaction.py --baseline','BASELINE_INPUT',str(base),'BASELINE_LITERAL_OUTPUT',f'REVIEW_ONLY {bo}',f'BASELINE_RESULT={"PASS" if be==0 else "FAIL"}',f'BASELINE_EXIT_STATUS={be}','', 'MODIFIED_COMMAND','python artifacts/run_m1_pr08_transaction.py --modified','MODIFIED_INPUT',str(mod),'MODIFIED_LITERAL_OUTPUT',f'ATOMIC_CONFIG_V2 {mo}',f'MODIFIED_RESULT={"PASS" if me==0 else "FAIL"}',f'MODIFIED_EXIT_STATUS={me}','', 'ROLLBACK_COMMAND','C:/Program Files/Git/bin/bash.exe '+ ' '.join('"'+x+'"' for x in cmd[1:]),'ROLLBACK_INPUT',f'Copied MODIFIED_FILE to {rb} before running ROLLBACK.sh.', 'ROLLBACK_LITERAL_OUTPUT',proc.stdout.strip(),f'ROLLBACK_RESULT={"PASS" if proc.returncode==0 else "FAIL"}',f'ROLLBACK_EXIT_STATUS={proc.returncode}',f'ROLLBACK_VERIFY_LITERAL_OUTPUT=REVIEW_ONLY {ro}',f'ROLLBACK_VERIFY_EXIT_STATUS={re}','',f'RESTORED_BEHAVIOR={"PASS" if restored and re==0 else "FAIL"}',f'MODIFIED_FILE_LEFT_CHANGED={"PASS" if changed else "FAIL"}', 'RESTORED_STATUS=value=review_only; config_schema=LegacyImportPreview; committable=false; actions_enabled=false; activation_required=true','MODIFIED_STATUS=value=atomic_config_v2; config_schema=ConfigV2; committable=false; actions_enabled=false; activation_required=true','', 'DIFF_SUMMARY','The changed branch is m1_pr08. The changed field is profile_commit_state, represented by value/config_schema review_only/LegacyImportPreview -> atomic_config_v2/ConfigV2.','The modified branch remains non-actionable, requires explicit activation, preserves source hash metadata, and records zero image file writes.']
ver.write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('BASELINE',bo); print('MODIFIED',mo); print(proc.stdout.strip()); print('ROLLBACK_VERIFY',ro); print(f'BASELINE_EXIT={be}'); print(f'MODIFIED_EXIT={me}'); print(f'ROLLBACK_EXIT={proc.returncode}'); print(f'ROLLBACK_VERIFY_EXIT={re}'); print(f'RESTORED_BEHAVIOR={"PASS" if restored and re==0 else "FAIL"}'); print(f'MODIFIED_FILE_LEFT_CHANGED={"PASS" if changed else "FAIL"}')
raise SystemExit(0 if be==me==proc.returncode==re==0 and restored and changed else 1)
