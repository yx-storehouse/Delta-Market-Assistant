from __future__ import annotations

import difflib
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "m1_pr07_transaction"
ROOT.mkdir(parents=True, exist_ok=True)
BASELINE = ROOT / "BASELINE_COPY.json"
MODIFIED = ROOT / "MODIFIED_FILE.json"
ROLLBACK_TEST = ROOT / "ROLLBACK_TEST_COPY.json"

baseline = {
    "branch": "m1_pr07",
    "field": "ui_import_preview_mode",
    "value": "hidden",
    "preview_only": False,
    "committable": False,
    "actions_enabled": False,
    "image_file_write_count": 0,
    "source_hash_preserved": True,
}
modified = dict(baseline)
modified.update({"value": "readonly_preview", "preview_only": True})

for path, value in ((BASELINE, baseline), (MODIFIED, modified)):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

diff = "".join(
    difflib.unified_diff(
        BASELINE.read_text(encoding="utf-8").splitlines(keepends=True),
        MODIFIED.read_text(encoding="utf-8").splitlines(keepends=True),
        fromfile=BASELINE.name,
        tofile=MODIFIED.name,
    )
)
(ROOT / "DIFF_FILE").write_text(diff, encoding="utf-8")

(ROOT / "ROLLBACK.sh").write_text(
    "#!/usr/bin/env bash\nset -euo pipefail\nTARGET=\"$1\"\nBASELINE=\"$2\"\ncp -- \"$BASELINE\" \"$TARGET\"\nprintf 'ROLLBACK_RESTORED=%s\\n' \"$TARGET\"\n",
    encoding="utf-8",
)
try:
    os.chmod(ROOT / "ROLLBACK.sh", 0o755)
except OSError:
    pass

print(f"BASELINE_COPY={BASELINE}")
print(f"MODIFIED_FILE={MODIFIED}")
print(f"ORIGINAL_HASH_SHA256={hashlib.sha256(BASELINE.read_bytes()).hexdigest()}")
print(f"MODIFIED_HASH_SHA256={hashlib.sha256(MODIFIED.read_bytes()).hexdigest()}")
