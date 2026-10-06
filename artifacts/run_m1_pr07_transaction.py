from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "m1_pr07_transaction"
BASELINE = ROOT / "BASELINE_COPY.json"
MODIFIED = ROOT / "MODIFIED_FILE.json"
ROLLBACK_TEST = ROOT / "ROLLBACK_TEST_COPY.json"
VERIFICATION = ROOT / "VERIFICATION.txt"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(path: Path, expected_value: str, expected_preview_only: bool) -> tuple[int, str]:
    value = json.loads(path.read_text(encoding="utf-8"))
    ok = (
        value["branch"] == "m1_pr07"
        and value["field"] == "ui_import_preview_mode"
        and value["value"] == expected_value
        and value["preview_only"] is expected_preview_only
        and value["committable"] is False
        and value["actions_enabled"] is False
        and value["image_file_write_count"] == 0
        and value["source_hash_preserved"] is True
    )
    line = (
        f"value={value['value']} preview_only={value['preview_only']} "
        f"committable={value['committable']} actions_enabled={value['actions_enabled']} "
        f"image_file_write_count={value['image_file_write_count']}"
    )
    return (0 if ok else 1), line


if "--baseline" in sys.argv:
    code, output = verify(BASELINE, "hidden", False)
    print(f"HIDDEN {output}")
    raise SystemExit(code)

if "--modified" in sys.argv:
    code, output = verify(MODIFIED, "readonly_preview", True)
    print(f"READONLY_PREVIEW {output}")
    raise SystemExit(code)


baseline_exit, baseline_output = verify(BASELINE, "hidden", False)
modified_exit, modified_output = verify(MODIFIED, "readonly_preview", True)
shutil.copy2(MODIFIED, ROLLBACK_TEST)
rollback_command = [
    r"C:/Program Files/Git/bin/bash.exe",
    str(ROOT / "ROLLBACK.sh"),
    str(ROLLBACK_TEST),
    str(BASELINE),
]
rollback = subprocess.run(rollback_command, cwd=ROOT, capture_output=True, text=True)
rollback_verify_exit, rollback_output = verify(ROLLBACK_TEST, "hidden", False)
restored = digest(ROLLBACK_TEST) == digest(BASELINE)
modified_changed = digest(MODIFIED) != digest(BASELINE)

lines = [
    "M1 PR07 TOOL TRANSACTION VERIFICATION",
    "DATE=2026-10-06",
    "TARGET=13-column import preview UI projection fixture",
    "CHANGED_BRANCH=m1_pr07",
    "CHANGED_FIELD=ui_import_preview_mode (value/mode)",
    "SAFEGUARD=committable=false; actions_enabled=false; image_file_write_count=0",
    "",
    "ARTIFACTS",
    f"MODIFIED_FILE={MODIFIED}",
    f"DIFF_FILE={ROOT / 'DIFF_FILE'}",
    f"VERIFICATION={VERIFICATION}",
    f"ROLLBACK_SCRIPT={ROOT / 'ROLLBACK.sh'}",
    f"BASELINE_COPY={BASELINE}",
    "",
    f"ORIGINAL_HASH_SHA256={digest(BASELINE)}",
    f"MODIFIED_HASH_SHA256={digest(MODIFIED)}",
    f"ROLLBACK_TEST_COPY={ROLLBACK_TEST}",
    "",
    "BASELINE_COMMAND",
    "python artifacts/run_m1_pr07_transaction.py --baseline",
    "BASELINE_INPUT",
    str(BASELINE),
    "BASELINE_LITERAL_OUTPUT",
    f"HIDDEN {baseline_output}",
    "BASELINE_RESULT=PASS" if baseline_exit == 0 else "BASELINE_RESULT=FAIL",
    f"BASELINE_EXIT_STATUS={baseline_exit}",
    "",
    "MODIFIED_COMMAND",
    "python artifacts/run_m1_pr07_transaction.py --modified",
    "MODIFIED_INPUT",
    str(MODIFIED),
    "MODIFIED_LITERAL_OUTPUT",
    f"READONLY_PREVIEW {modified_output}",
    "MODIFIED_RESULT=PASS" if modified_exit == 0 else "MODIFIED_RESULT=FAIL",
    f"MODIFIED_EXIT_STATUS={modified_exit}",
    "",
    "ROLLBACK_COMMAND",
    "C:/Program Files/Git/bin/bash.exe " + " ".join(f'\"{part}\"' for part in rollback_command[1:]),
    "ROLLBACK_INPUT",
    f"Copied MODIFIED_FILE to {ROLLBACK_TEST} before running ROLLBACK.sh.",
    "ROLLBACK_LITERAL_OUTPUT",
    rollback.stdout.strip(),
    f"ROLLBACK_RESULT={'PASS' if rollback.returncode == 0 else 'FAIL'}",
    f"ROLLBACK_EXIT_STATUS={rollback.returncode}",
    f"ROLLBACK_VERIFY_LITERAL_OUTPUT=HIDDEN {rollback_output}",
    f"ROLLBACK_VERIFY_EXIT_STATUS={rollback_verify_exit}",
    "",
    f"RESTORED_BEHAVIOR={'PASS' if restored else 'FAIL'}",
    f"MODIFIED_FILE_LEFT_CHANGED={'PASS' if modified_changed else 'FAIL'}",
    "RESTORED_STATUS=value=hidden; preview_only=false; committable=false; actions_enabled=false",
    "MODIFIED_STATUS=value=readonly_preview; preview_only=true; committable=false; actions_enabled=false",
    "",
    "DIFF_SUMMARY",
    "The changed branch is m1_pr07. The changed field is ui_import_preview_mode, represented by value/mode hidden -> readonly_preview.",
    "The modified branch remains read-only and non-actionable; source hash preservation and zero image file writes remain enforced.",
]
VERIFICATION.write_text("\n".join(lines) + "\n", encoding="utf-8")

print("BASELINE", baseline_output)
print("MODIFIED", modified_output)
print(rollback.stdout.strip())
print("ROLLBACK_VERIFY", rollback_output)
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
