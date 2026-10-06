from pathlib import Path
import hashlib
import json
import shutil
import subprocess

root = Path("artifacts/m1_pr05_transaction").resolve()
baseline = root / "BASELINE_COPY.json"
modified = root / "MODIFIED_FILE.json"
rollback_test = root / "ROLLBACK_TEST_COPY.json"

def verify(path: Path, expected_mode: str):
    data = json.loads(path.read_text(encoding="utf-8"))
    ok = (data["mode"] == expected_mode and data["actions_enabled"] is False
          and data["image_file_write_count"] == 0)
    print(f"{expected_mode.upper()} mode={data['mode']} actions_enabled={data['actions_enabled']} image_file_write_count={data['image_file_write_count']}")
    return 0 if ok else 1

print("COMMAND_BASELINE=python artifacts/run_transaction_tests.py --baseline")
baseline_exit = verify(baseline, "demo")
print(f"BASELINE_EXIT={baseline_exit}")
print("COMMAND_MODIFIED=python artifacts/run_transaction_tests.py --modified")
modified_exit = verify(modified, "replay")
print(f"MODIFIED_EXIT={modified_exit}")

shutil.copy2(modified, rollback_test)
bash = Path(r"C:/Program Files/Git/bin/bash.exe")
rollback_command = [str(bash), str(root / "ROLLBACK.sh"), str(rollback_test), str(baseline)]
result = subprocess.run(rollback_command, cwd=root, capture_output=True, text=True)
print("COMMAND_ROLLBACK=" + " ".join(rollback_command))
print(result.stdout.strip())
print(f"ROLLBACK_EXIT={result.returncode}")
rollback_exit = verify(rollback_test, "demo")
print(f"ROLLBACK_VERIFY_EXIT={rollback_exit}")

def sha(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

same_after_rollback = sha(rollback_test) == sha(baseline)
modified_still_changed = sha(modified) != sha(baseline)
print(f"RESTORED_BEHAVIOR={'PASS' if same_after_rollback else 'FAIL'}")
print(f"MODIFIED_FILE_LEFT_CHANGED={'PASS' if modified_still_changed else 'FAIL'}")
raise SystemExit(0 if baseline_exit == modified_exit == result.returncode == rollback_exit == 0
                  and same_after_rollback and modified_still_changed else 1)
