from __future__ import annotations
import difflib, hashlib, json, os
from pathlib import Path
root=Path(__file__).resolve().parent/'m1_pr08_transaction'
root.mkdir(parents=True,exist_ok=True)
base={
 'branch':'m1_pr08','field':'profile_commit_state','value':'review_only',
 'committable':False,'actions_enabled':False,'activation_required':True,
 'source_hash_preserved':True,'image_file_write_count':0,'target_mode':'demo',
 'config_schema':'LegacyImportPreview'
}
mod=dict(base); mod.update({'value':'atomic_config_v2','config_schema':'ConfigV2','revision_policy':'same_profile_id_increments'})
for p,v in [(root/'BASELINE_COPY.json',base),(root/'MODIFIED_FILE.json',mod)]:
 p.write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
diff=''.join(difflib.unified_diff((root/'BASELINE_COPY.json').read_text(encoding='utf-8').splitlines(True),(root/'MODIFIED_FILE.json').read_text(encoding='utf-8').splitlines(True),fromfile='BASELINE_COPY.json',tofile='MODIFIED_FILE.json'))
(root/'DIFF_FILE').write_text(diff,encoding='utf-8')
(root/'ROLLBACK.sh').write_text('''#!/usr/bin/env bash\nset -euo pipefail\nTARGET="$1"\nBASELINE="$2"\ncp -- "$BASELINE" "$TARGET"\nprintf 'ROLLBACK_RESTORED=%s\\n' "$TARGET"\n''',encoding='utf-8')
os.chmod(root/'ROLLBACK.sh',0o755)
print('BASELINE_COPY='+str(root/'BASELINE_COPY.json'))
print('MODIFIED_FILE='+str(root/'MODIFIED_FILE.json'))
print('ORIGINAL_HASH_SHA256='+hashlib.sha256((root/'BASELINE_COPY.json').read_bytes()).hexdigest())
print('MODIFIED_HASH_SHA256='+hashlib.sha256((root/'MODIFIED_FILE.json').read_bytes()).hexdigest())
