from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "m1_pr07_ui_transaction"
BASELINE = ROOT / "BASELINE_COPY.json"
MODIFIED = ROOT / "MODIFIED_FILE.json"
ROLLBACK_TEST = ROOT / "ROLLBACK_TEST_COPY.json"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(path: Path, expected: str) -> int:
    value = json.loads(path.read_text(encoding="utf-8"))
    ok = (
        value["value"] == expected
        and value["committable"] is False
        and value["actions_enabled"] is False
        and value["image_file_write_count"] == 0
        and value["source_hash_preserved"] is True
    )
    print(
        f"{expected.upper()} value={value['value']} committable={value['committable']} "
        f"actions_enabled={value['actions_enabled']} image_file_write_count={value['image_file_write_count']}"
    )
    return 0 if ok else 1


baseline_exit = check(BASELINE, "direct_load")
modified_exit = check(MODIFIED, "readonly_preview")
shutil.copy2(MODIFIED, ROLLBACK_TEST)
rollback = subprocess.run(
    [r"C:/Program Files/Git/bin/bash.exe", str(ROOT / "ROLLBACK.sh"), str(ROLLBACK_TEST), str(BASELINE)],
    cwd=ROOT,
    capture_output=True,
    text=True,
)
print(rollback.stdout.strip())
rollback_verify_exit = check(ROLLBACK_TEST, "direct_load")
restored = digest(ROLLBACK_TEST) == digest(BASELINE)
modified_changed = digest(MODIFIED) != digest(BASELINE)
print(f"BASELINE_EXIT={baseline_exit}")
print(f"MODIFIED_EXIT={modified_exit}")
print(f"ROLLBACK_EXIT={rollback.returncode}")
print(f"ROLLBACK_VERIFY_EXIT={rollback_verify_exit}")
print(f"RESTORED_BEHAVIOR={'PASS' if restored else 'FAIL'}")
print(f"MODIFIED_FILE_LEFT_CHANGED={'PASS' if modified_changed else 'FAIL'}")
raise SystemExit(
    0
    if baseline_exit == modified_exit == rollback.returncode == rollback_verify_exit == 0
    and restored
    and modified_changed
    else 1
)
