from pathlib import Path
import hashlib
import json

root = Path(".").resolve()
old_path = root / "artifacts/business_rebuild_docs/m1_pr01_pr04_baseline.json"
old = json.loads(old_path.read_text(encoding="utf-8"))
files = dict(old["files"])
files["src/main.cpp"] = hashlib.sha256((root / "src/main.cpp").read_bytes()).hexdigest()
for release_path in ("dist/RelinkStudio/RelinkStudio.exe", "dist/RelinkStudio/file_manifest.json"):
    release_file = root / release_path
    if release_file.is_file():
        files[release_path] = hashlib.sha256(release_file.read_bytes()).hexdigest()
for path in list(files):
    files[path] = hashlib.sha256((root / path).read_bytes()).hexdigest()
payload = {
    "baseline_id": "m1_pr05",
    "created_at": "2026-10-06",
    "parent_baseline": "artifacts/business_rebuild_docs/m1_pr01_pr04_baseline.json",
    "scope": "PR01-PR05 domain, replay runtime, replay UI projection, and offscreen UI acceptance",
    "follow_on_changes": [
        "PR05 replay source/state/reason projection is wired into the existing overview without OCR, capture, input, purchase, or BBZPS execution",
        "offscreen self-test covers Replay start/pause/resume/stop transitions and compact Win11 layout"
    ],
    "files": dict(sorted(files.items())),
    "expected_project_baseline_drift": [
        "CMakeLists.txt", "src/mainwindow.cpp", "src/mainwindow.h",
        "dist/RelinkStudio/RelinkStudio.exe", "dist/RelinkStudio/file_manifest.json"
    ]
}
out = root / "artifacts/business_rebuild_docs/m1_pr05_baseline.json"
out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"M1_PR05_BASELINE={out}")
print(f"M1_PR05_FILES={len(files)}")
