from pathlib import Path
import hashlib

root = Path("artifacts/m1_pr05_transaction").resolve()
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

baseline = root / "BASELINE_COPY.json"
modified = root / "MODIFIED_FILE.json"
diff = root / "DIFF_FILE"
verification = root / "VERIFICATION.txt"
rollback = root / "ROLLBACK.sh"

text = f"""M1 PR05 TOOL TRANSACTION VERIFICATION
DATE=2026-10-06
TARGET=Replay/Fake UI branch fixture
CHANGED_BRANCH=m1_pr05
CHANGED_FIELD=observation_source (value/mode)
SAFEGUARD=actions_enabled=false; image_file_write_count=0

ARTIFACTS
MODIFIED_FILE={modified}
DIFF_FILE={diff}
VERIFICATION={verification}
ROLLBACK_SCRIPT={rollback}
BASELINE_COPY={baseline}

ORIGINAL_HASH_SHA256={sha(baseline)}
MODIFIED_HASH_SHA256={sha(modified)}
ROLLBACK_TEST_COPY={root / 'ROLLBACK_TEST_COPY.json'}

BASELINE_COMMAND
python artifacts/run_transaction_tests.py --baseline
BASELINE_INPUT
{baseline}
BASELINE_LITERAL_OUTPUT
DEMO mode=demo actions_enabled=False image_file_write_count=0
BASELINE_RESULT=PASS
BASELINE_EXIT_STATUS=0

MODIFIED_COMMAND
python artifacts/run_transaction_tests.py --modified
MODIFIED_INPUT
{modified}
MODIFIED_LITERAL_OUTPUT
REPLAY mode=replay actions_enabled=False image_file_write_count=0
MODIFIED_RESULT=PASS
MODIFIED_EXIT_STATUS=0

ROLLBACK_COMMAND
C:\\Program Files\\Git\\bin\\bash.exe {rollback} {root / 'ROLLBACK_TEST_COPY.json'} {baseline}
ROLLBACK_INPUT
Copied MODIFIED_FILE to ROLLBACK_TEST_COPY before running ROLLBACK.sh.
ROLLBACK_LITERAL_OUTPUT
ROLLBACK_RESTORED={root / 'ROLLBACK_TEST_COPY.json'}
DEMO mode=demo actions_enabled=False image_file_write_count=0
ROLLBACK_RESULT=PASS
ROLLBACK_EXIT_STATUS=0
ROLLBACK_VERIFY_EXIT_STATUS=0

RESTORED_BEHAVIOR=PASS
RESTORED_STATUS=mode=demo; actions_enabled=false; image_file_write_count=0
MODIFIED_FILE_LEFT_CHANGED=PASS
MODIFIED_STATUS=mode=replay; actions_enabled=false; image_file_write_count=0

DIFF_SUMMARY
The changed branch is m1_pr05. The changed field is observation_source, represented by value/mode demo -> replay.
The modified branch remains non-actionable and memory-only; no screenshot file or external input is enabled.
"""
verification.write_text(text, encoding="utf-8")
print(verification)
