from pathlib import Path
import difflib
import json

root = Path("artifacts/m1_pr05_transaction")
modified = json.loads((root / "MODIFIED_FILE.json").read_text(encoding="utf-8"))
modified["value"] = "replay"
modified["mode"] = "replay"
(root / "MODIFIED_FILE.json").write_text(json.dumps(modified, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
before = (root / "BASELINE_COPY.json").read_text(encoding="utf-8").splitlines(keepends=True)
after = (root / "MODIFIED_FILE.json").read_text(encoding="utf-8").splitlines(keepends=True)
diff = difflib.unified_diff(before, after, fromfile="BASELINE_COPY.json", tofile="MODIFIED_FILE.json")
(root / "DIFF_FILE").write_text("".join(diff), encoding="utf-8")
print("TRANSACTION_MODIFIED=PASS")
