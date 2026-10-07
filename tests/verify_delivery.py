"""Parameterized extracted-package acceptance. All outputs stay in a fresh directory."""
from __future__ import annotations
import argparse
from datetime import datetime
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_package_harness():
    path = ROOT / "tests/release/verify_pr12b_package.py"
    spec = importlib.util.spec_from_file_location("package_acceptance", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build_relocated")
    parser.add_argument("--release-dir", type=Path, default=ROOT / "dist/RelinkStudio")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    build, release = args.build_dir.resolve(), args.release_dir.resolve()
    out = (args.output_dir or ROOT / "artifacts" / ("delivery_" + datetime.now().strftime("%Y%m%d_%H%M%S"))).resolve()
    if out.is_relative_to(release) or out.is_relative_to(build):
        raise RuntimeError("output directory must not be inside release/build directories")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise RuntimeError(f"refusing to overwrite non-empty evidence directory: {out}")
    out.mkdir(parents=True, exist_ok=True)
    harness = load_package_harness()
    records = []
    exe = release / "RelinkStudio.exe"
    built = build / "RelinkStudio.exe"
    if not exe.is_file() or not built.is_file() or sha(exe) != sha(built):
        raise RuntimeError("current built EXE and released EXE must have identical hashes")
    release_hash = sha(exe)
    package_evidence = harness.verify_release_contents(release, build)
    release_digest = harness.directory_digest(release)
    (out / "package_manifest.json").write_text(json.dumps(package_evidence, indent=2) + "\n", encoding="utf-8")

    def run(label, command, input_text, required=(), env=None):
        command = [str(x) for x in command]
        result = subprocess.run(command, cwd=ROOT, env=env or harness.qenv(release),
                                capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=900,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        record = dict(label=label, command=subprocess.list2cmdline(command), input=input_text,
                      stdout=result.stdout, stderr=result.stderr, exit_status=result.returncode)
        records.append(record)
        (out / "delivery_test_results.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        missing = [x for x in required if x not in result.stdout and x not in result.stderr]
        if result.returncode or missing:
            raise RuntimeError(f"{label}: exit={result.returncode}; missing={missing}\n{result.stdout}\n{result.stderr}")
        print(f"{label}_EXIT={result.returncode}", flush=True)
        for line in (result.stdout + result.stderr).splitlines():
            if "PASS" in line or any(x in line for x in required):
                print(line, flush=True)
        return record

    run("DOMAIN", [build / "domain_tests.exe"], "Current domain test; package-only PATH; no GUI/game.", ["RESULT PASS"])
    observation = harness._test_executable(build, "observation_adapter_tests")
    worker = harness._test_executable(build, "worker_protocol_tests")
    vision = harness._test_executable(build, "vision_pipeline_tests")
    shared_memory = harness._test_executable(build, "shared_frame_memory_tests")
    worker_process = harness._test_executable(build, "worker_process_tests")
    fixture_child = harness._test_executable(build, "vision_worker_fixture")
    if not all((observation, worker, vision, shared_memory, worker_process, fixture_child)):
        raise RuntimeError("required runtime focused test target is missing")
    fixture_child = fixture_child.resolve()
    if fixture_child.is_relative_to(release) or (release / fixture_child.name).exists():
        raise RuntimeError("synthetic worker fixture must not be shipped in the desktop release")
    run("OBSERVATION_ADAPTER", [observation],
        "Synthetic memory-frame lifecycle; no image capture.", ["OBSERVATION_ADAPTER_TESTS=PASS", "image_file_writes=0"])
    fixtures = ROOT / "docs/business_rebuild/implementation/runtime/fixtures/messages"
    run("WORKER_PROTOCOL", [worker, fixtures],
        "Frozen synthetic worker fixtures; actual protocol and lease state machine; no worker process or OCR.",
        ["WORKER_PROTOCOL_TESTS=PASS", "external_processes=0", "image_file_writes=0"])
    run("VISION_PIPELINE", [vision, fixtures],
        "In-process synthetic observation/protocol/lease integration; no external worker or game.",
        ["VISION_PIPELINE_TESTS=PASS"])
    run("SHARED_FRAME_MEMORY", [shared_memory],
        "Isolated real paging-file-backed memory ownership/read-only view regression; no image files or game.",
        ["SHARED_FRAME_MEMORY_TESTS=PASS", "image_file_writes=0"])
    run("WORKER_PROCESS", [worker_process.resolve(), fixture_child, fixtures],
        "Isolated relink_vision_transport tests launch a hidden synthetic fixture child with shared-memory bytes; "
        "not a real OCR worker, not shipped in the desktop package, no game or capture.",
        ["WORKER_PROCESS_TESTS=PASS", "failures=0"])
    run("PACKAGE_HARNESS", [sys.executable, "-I", "-X", "utf8", ROOT / "tests/release/test_package_harness.py"],
        "Tiny non-executable package fixtures and generated rollback script negative regressions; no app/game process.",
        ["PACKAGE_HARNESS_TESTS=PASS", "failures=0", "errors=0", "fixture_only=true"], os.environ.copy())
    run("PACKAGE_STORAGE", [exe, "--storage-self-test"], "Packaged SQLite plugin; temporary database.", ["STORAGE_SELF_TEST=PASS"])
    cli_evidence = harness.verify_cli_workspace(release, out / "cli", run)
    (out / "cli_checks.json").write_text(json.dumps(cli_evidence, indent=2) + "\n", encoding="utf-8")

    baseline = out / "BASELINE.json"
    run("DEMO_FIXTURE", [exe, "--write-demo-config", baseline], "Fresh synthetic config under isolated output.", ["DEMO_CONFIG=PASS"])
    baseline_bytes = baseline.read_bytes()
    data = json.loads(baseline_bytes)
    tasks = data.get("tasks")
    if not tasks:
        raise RuntimeError("synthetic config contains no tasks")
    original_price, original_quantity = tasks[0]["maxPrice"], tasks[0]["quantity"]
    tasks[0]["maxPrice"] = original_price + 50
    tasks[0]["quantity"] = original_quantity + 1
    modified = out / "MODIFIED_FILE.json"
    modified.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    diff = out / "DIFF_FILE"
    diff.write_text("".join(difflib.unified_diff(baseline_bytes.decode("utf-8").splitlines(True),
                    modified.read_text(encoding="utf-8").splitlines(True),
                    fromfile="BASELINE.json", tofile="MODIFIED_FILE.json")), encoding="utf-8")
    run("BASELINE_CONFIG", [exe, "--validate-config", baseline], "Baseline synthetic fixture.", ["CONFIG_VALIDATION=PASS"])
    run("MODIFIED_CONFIG", [exe, "--validate-config", modified], "Modified local config copy.", [
        "CONFIG_VALIDATION=PASS", f"first_max_price={tasks[0]['maxPrice']:.2f}", f"first_quantity={tasks[0]['quantity']}"])
    run("EQUALS_HEADLESS", [exe, "--validate-config=" + str(baseline)], "Equals-style CLI; explicit offscreen config.", ["CONFIG_VALIDATION=PASS"])

    restore = out / "restore_config.py"
    restore.write_text(
        "from pathlib import Path\nimport shutil,sys\n"
        "root=Path(__file__).resolve().parent\n"
        "target=Path(sys.argv[1]).resolve()\n"
        "assert target == root/'ROLLBACK_TEST.json', 'isolated target required'\n"
        "shutil.copy2(root/'BASELINE.json', target)\n"
        "assert target.read_bytes()==(root/'BASELINE.json').read_bytes()\n"
        "print('CONFIG_ROLLBACK=PASS')\n", encoding="utf-8")
    rollback_script = out / "ROLLBACK.sh"
    rollback_script.write_text(
        "#!/usr/bin/env bash\nset -euo pipefail\n"
        "here=\"$(cd -- \"$(dirname -- \"${BASH_SOURCE[0]}\")\" && pwd)\"\n"
        "if [[ $# -ne 2 ]]; then echo 'Usage: ROLLBACK.sh <python-exe> <isolated-target>' >&2; exit 2; fi\n"
        "\"$1\" \"$(cygpath -w \"$here/restore_config.py\")\" \"$2\"\n", encoding="utf-8", newline="\n")
    rollback_copy = out / "ROLLBACK_TEST.json"
    shutil.copy2(modified, rollback_copy)
    bash = Path(r"C:\Program Files\Git\bin\bash.exe")
    command = 'script="$(cygpath -u "$1")"; chmod +x "$script" && "$script" "$(cygpath -u "$2")" "$3"'
    run("ROLLBACK_CONFIG", [bash, "-c", command, "rollback", rollback_script, sys.executable, rollback_copy],
        "Modified config copy restored using executable ROLLBACK.sh; no user config.", ["CONFIG_ROLLBACK=PASS"], os.environ.copy())
    run("RESTORED_CONFIG", [exe, "--validate-config", rollback_copy], "Restored baseline config copy.", [
        "CONFIG_VALIDATION=PASS", f"first_max_price={original_price:.2f}", f"first_quantity={original_quantity}"])
    if rollback_copy.read_bytes() != baseline_bytes or modified.read_bytes() == baseline_bytes or sha(exe) != release_hash:
        raise RuntimeError("baseline/modified/rollback state mismatch")
    if harness.directory_digest(release) != release_digest:
        raise RuntimeError("delivery checks changed the extracted release directory")
    hashes = dict(baseline=sha(baseline), modified=sha(modified), restored=sha(rollback_copy), application=release_hash)
    (out / "fixture_hashes.json").write_text(json.dumps(hashes, indent=2) + "\n", encoding="utf-8")
    lines = ["Delta Market Assistant - parameterized delivery verification",
        f"BUILD_DIR: {build}", f"RELEASE_DIR: {release}", f"OUTPUT_DIR: {out}",
        f"BUILT_EXE_SHA256: {sha(built)}", f"RELEASED_EXE_SHA256: {release_hash}",
        f"Changed fields: tasks[0].maxPrice {original_price} -> {tasks[0]['maxPrice']}; tasks[0].quantity {original_quantity} -> {tasks[0]['quantity']}; isolated fixture only.",
        f"BASELINE_SHA256: {hashes['baseline']}", f"MODIFIED_SHA256: {hashes['modified']}", f"RESTORED_SHA256: {hashes['restored']}",
        f"RELEASE_DIRECTORY_DIGEST_UNCHANGED: {release_digest}",
        "M2_BOUNDARY: no live game capture, OCR, input injection, purchase or transaction.",
        "TRANSPORT_BOUNDARY: real shared memory/hidden synthetic fixture child tested separately; fixture is not an OCR worker and is not shipped; product remains replay-only.",
        f"MODIFIED_FILE: {modified}", f"DIFF_FILE: {diff}", f"VERIFICATION: {out/'VERIFICATION.txt'}",
        f"ROLLBACK: {rollback_script}"]
    for rec in records:
        lines += ["", rec["label"], "COMMAND: " + rec["command"], "INPUT: " + rec["input"],
                  "STDOUT:", rec["stdout"].rstrip(), "STDERR:", rec["stderr"].rstrip() or "(empty)",
                  "EXIT_STATUS: " + str(rec["exit_status"])]
    lines += ["", "RESTORED: isolated config copy equals baseline; validator accepted original values.",
              "LEFT_CHANGED: MODIFIED_FILE.json remains changed; fixed release remains unchanged by tests."]
    (out / "VERIFICATION.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    for artifact in (modified, diff, out/"VERIFICATION.txt", rollback_script):
        assert artifact.read_bytes()
        print("REOPEN=PASS; path=" + str(artifact), flush=True)
    print("DELIVERY_VERIFICATION=PASS; manifest_exact=true; cli_isolated=true", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
