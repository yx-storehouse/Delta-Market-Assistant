from pathlib import Path
import hashlib,json,shutil,subprocess
root=Path(__file__).resolve().parent;base=root/'BASELINE_COPY.json';mod=root/'MODIFIED_FILE.json';rb=root/'ROLLBACK_TEST_COPY.json'
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def verify(p,value):
 d=json.loads(p.read_text(encoding='utf-8')); ok=(d['branch']=='git_project' and d['field']=='repository_state' and d['value']==value and d['branch_name']=='main' and d['secret_files_staged']==0 and d['dist_preserved_local'] is True)
 return 0 if ok else 1, f"value={d['value']} branch_name={d['branch_name']} bbzps_ignored={d['bbzps_ignored']} large_artifacts_ignored={d['large_artifacts_ignored']} dist_preserved_local={d['dist_preserved_local']} secret_files_staged={d['secret_files_staged']}"
be,bo=verify(base,'uninitialized');me,mo=verify(mod,'initialized_main');shutil.copy2(mod,rb);cmd=['C:/Program Files/Git/bin/bash.exe',str((root/'ROLLBACK.sh').resolve()),str(rb.resolve()),str(base)];proc=subprocess.run(cmd,capture_output=True,text=True,cwd=root);re,ro=verify(rb,'uninitialized');restored=digest(rb)==digest(base);changed=digest(mod)!=digest(base)
print('BASELINE',bo);print('MODIFIED',mo);print(proc.stdout.strip());print('ROLLBACK_VERIFY',ro);print(f'BASELINE_EXIT={be}');print(f'MODIFIED_EXIT={me}');print(f'ROLLBACK_EXIT={proc.returncode}');print(f'ROLLBACK_VERIFY_EXIT={re}');print(f'RESTORED_BEHAVIOR={"PASS" if restored and re==0 else "FAIL"}');print(f'MODIFIED_FILE_LEFT_CHANGED={"PASS" if changed else "FAIL"}')
raise SystemExit(0 if be==me==proc.returncode==re==0 and restored and changed else 1)


