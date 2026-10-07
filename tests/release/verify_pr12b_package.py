"""PR12B service-boundary and package acceptance harness.

This harness is intentionally evidence-first.  It never reports a passing
business or capture result from documentation/schema checks.  The ``baseline``
phase freezes the already-built release, ``build`` runs the real CTest/package
commands, ``modified`` exercises the packaged executable and service tests,
and ``finalize`` writes the four transaction artifacts and a complete command
record.  No visible window is opened; all application checks use Qt offscreen.

The harness stops at the M2 boundary: it verifies replay/history/configuration
behaviour and proves that no game process, input injection, or runtime image
file writes occurred.  It deliberately does not claim live game capture/OCR.
"""
from __future__ import annotations

import argparse
import configparser
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / "artifacts/m1_pr12b_transaction"
RELEASE = ROOT / "dist/RelinkStudio"
BASELINE = TX / "baseline/release"
BUILD_DEFAULT = ROOT / "build_relocated"
SERVICE_FILES = {
    "profile_catalog": ("src/application/workspace/profile_catalog.h", "ProfileCatalog"),
    "replay_scenario": ("src/application/workspace/replay_scenario.h", "ReplayScenario"),
    "history_query_service": (
        "src/application/workspace/history_query_service.h",
        "HistoryQueryService",
    ),
    "records_exporter": (
        "src/application/workspace/records_exporter.h",
        "RecordsExporter",
    ),
}


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def qenv(runtime: Path = RELEASE) -> dict[str, str]:
    env = os.environ.copy()
    for key in (
        "QT_PLUGIN_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QML_IMPORT_PATH",
        "QML2_IMPORT_PATH",
    ):
        env.pop(key, None)
    system_root = Path(env.get("SystemRoot", r"C:\Windows"))
    env["PATH"] = ";".join(
        str(item) for item in (runtime, system_root / "System32", system_root)
    )
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_SCALE_FACTOR"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def directory_digest(root: Path) -> str:
    """Hash relative names/types/bytes without relying on directory mtimes."""
    digest = hashlib.sha256()
    if not root.exists():
        return '<missing>'
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root).as_posix().encode('utf-8')
        if path.is_symlink():
            digest.update(b'L\0' + relative + b'\0' + os.readlink(path).encode('utf-8') + b'\n')
        elif path.is_file():
            digest.update(b'F\0' + relative + b'\0' + str(path.stat().st_size).encode('ascii') + b'\0')
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
            digest.update(b'\n')
        elif path.is_dir():
            digest.update(b'D\0' + relative + b'\n')
    return digest.hexdigest()


def regular_tree(root: Path) -> Iterable[Path]:
    """Walk without following reparse points, including directory junctions."""
    def check(path: Path) -> os.stat_result:
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise AssertionError("reparse package path: " + str(path))
        return info

    if not stat.S_ISDIR(check(root).st_mode):
        raise AssertionError("package root must be a regular directory: " + str(root))
    pending = [root]
    while pending:
        directory = pending.pop()
        for path in sorted(directory.iterdir()):
            info = check(path)
            if stat.S_ISDIR(info.st_mode):
                yield path
                pending.append(path)
            elif stat.S_ISREG(info.st_mode):
                yield path
            else:
                raise AssertionError("non-regular package path: " + str(path))


def _record_path() -> Path:
    TX.mkdir(parents=True, exist_ok=True)
    return TX / "commands.json"


def _append_record(record: dict) -> None:
    path = _record_path()
    records = json.loads(path.read_text(encoding="utf-8")) if path.exists() else []
    records.append(record)
    path.write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run(
    label: str,
    args: list[str],
    input_description: str,
    required: Iterable[str] = (),
    *,
    cwd: Path = ROOT,
    env: dict[str, str] | None = None,
    timeout: int = 1200,
) -> dict:
    """Run one real command and persist its literal evidence before checking it."""
    result = subprocess.run(
        args,
        cwd=cwd,
        env=env or qenv(),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    record = {
        "label": label,
        "command": subprocess.list2cmdline([str(a) for a in args]),
        "input": input_description,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "exit_status": result.returncode,
    }
    _append_record(record)
    print(f"{label}_EXIT={result.returncode}", flush=True)
    for line in (result.stdout + result.stderr).splitlines():
        if any(token in line for token in required) or "PASS" in line or "tests passed" in line:
            print(line, flush=True)
    missing = [token for token in required if token not in result.stdout and token not in result.stderr]
    if result.returncode != 0 or missing:
        detail = f"{label}: exit={result.returncode}, missing={missing}\n{result.stdout}\n{result.stderr}"
        raise RuntimeError(detail)
    return record


def _manifest(directory: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in regular_tree(directory):
        if path.is_file():
            result[path.relative_to(directory).as_posix()] = sha(path)
    return result


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _verify_manifest(directory: Path, expected: dict[str, str], *, allow_extra: bool = False) -> None:
    actual = _manifest(directory)
    if (actual != expected) and not allow_extra:
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        changed = sorted(k for k in set(expected) & set(actual) if expected[k] != actual[k])
        raise AssertionError(f"release manifest mismatch missing={missing} extra={extra} changed={changed}")
    missing = sorted(set(expected) - set(actual))
    changed = sorted(k for k in set(expected) & set(actual) if expected[k] != actual[k])
    if missing or changed:
        raise AssertionError(f"release manifest mismatch missing={missing} changed={changed}")


def _source_revision() -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()


def source_boundaries() -> dict[str, object]:
    """Check real PR12B service seams without requiring a particular line count."""
    found: dict[str, str] = {}
    for role, (relative, class_name) in SERVICE_FILES.items():
        path = ROOT / relative
        if not path.is_file():
            raise AssertionError(f"missing PR12B service file: {relative}")
        text = path.read_text(encoding="utf-8")
        if not re.search(r"\bclass\s+" + re.escape(class_name) + r"\b", text):
            raise AssertionError(f"missing {class_name} declaration in {relative}")
        found[role] = relative

    controller = ROOT / "src/application/workspace/workspace_controller.cpp"
    controller_header = ROOT / "src/application/workspace/workspace_controller.h"
    if not controller.is_file() or not controller_header.is_file():
        raise AssertionError("WorkspaceController sources are missing")
    controller_text = controller.read_text(encoding="utf-8")
    for relative, _ in SERVICE_FILES.values():
        stem = Path(relative).stem
        if stem not in controller_text:
            raise AssertionError(f"WorkspaceController does not include/use {stem}")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    for relative, _ in SERVICE_FILES.values():
        if Path(relative).name not in cmake:
            raise AssertionError(f"CMake does not register {Path(relative).name}")
    return {"services": found, "controller": str(controller.relative_to(ROOT)), "cmake_registered": True}


def baseline() -> None:
    if not RELEASE.is_dir() or not (RELEASE / "RelinkStudio.exe").is_file():
        raise RuntimeError(f"fixed delivery directory is missing: {RELEASE}")
    TX.mkdir(parents=True, exist_ok=True)
    if BASELINE.exists():
        hashes_path = TX / "baseline_release_hashes.json"
        if not hashes_path.is_file():
            raise RuntimeError("baseline release exists without baseline_release_hashes.json")
        expected = json.loads(hashes_path.read_text(encoding="utf-8"))
        _verify_manifest(BASELINE, expected)
    else:
        shutil.copytree(RELEASE, BASELINE)
        _write_json(TX / "baseline_release_hashes.json", _manifest(BASELINE))
    revision_path = TX / "source_revision.txt"
    # A frozen transaction may be reused after the implementation has moved
    # ahead of the baseline commit.  Never silently replace that provenance
    # with the current HEAD; the modified phase records the new source state.
    if not revision_path.exists():
        revision_path.write_text(_source_revision() + "\n", encoding="ascii")
    if not (TX / "baseline_source_hashes.json").exists():
        _write_json(TX / "baseline_source_hashes.json", {
            str(p.relative_to(ROOT)): sha(p)
            for p in sorted((ROOT / "src").rglob("*"))
            if p.is_file()
        })
    # Baseline intentionally predates PR12B service files.  Service-boundary
    # evidence is collected in modified(), after the new source is present.
    ui("BASELINE", BASELINE, TX / "baseline/ui_snapshots")
    run(
        "BASELINE_STORAGE",
        [str(BASELINE / "RelinkStudio.exe"), "--storage-self-test"],
        "Frozen pre-PR12B package; temporary SQLite data and recovery checks; Qt offscreen.",
        ("STORAGE_SELF_TEST=PASS",),
        env=qenv(BASELINE),
    )
    print("PR12B_BASELINE=FROZEN; service_boundaries=DEFERRED_TO_MODIFIED", flush=True)


def build() -> None:
    run(
        "MODIFIED_BUILD",
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(ROOT / "build.ps1"),
            "-Test",
            "-Package",
        ],
        "Current source; real CMake build, CTest, and fixed extracted package.",
        ("PACKAGE=PASS",),
        env=os.environ.copy(),
        timeout=1800,
    )


def ui(label: str, runtime: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    config = TX / f"{label.lower()}-config.json"
    run(
        label,
        [
            str(runtime / "RelinkStudio.exe"),
            "--self-test",
            "--snapshot-dir",
            str(output),
            "--config",
            str(config),
        ],
        "Synthetic UI/config and temporary workspace; Qt offscreen; package-only PATH; no game process.",
        ("UI_SELF_TEST=PASS", "WORKSPACE_UI_SELF_TEST=PASS"),
        env=qenv(runtime),
    )
    for relative in (Path("ui_results.json"), Path("workspace/ui_results.json")):
        path = output / relative
        if not path.is_file():
            raise AssertionError(f"missing UI evidence: {path}")
        data = json.loads(path.read_text(encoding="utf-8"))
        if not data.get("passed", False):
            raise AssertionError(f"UI evidence did not pass: {path}")
        if data.get("game_connected") is not False or data.get("system_input_sent") is not False:
            raise AssertionError(f"live game/input activity was reported: {path}")
        if "image_file_write_count" in data and data["image_file_write_count"] != 0:
            raise AssertionError(f"runtime image writes were reported: {path}")


def _test_executable(build_dir: Path, name: str) -> Path | None:
    candidates = [build_dir / f"{name}.exe", build_dir / "tests" / f"{name}.exe"]
    return next((p for p in candidates if p.is_file()), None)


def write_rollback_scripts() -> None:
    """Materialize the actual restoration path; baseline bytes are never changed."""
    (TX / "ROLLBACK.sh").write_text(
        '#!/usr/bin/env bash\nset -euo pipefail\n'
        'here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"\n'
        'if [[ $# -ne 1 ]]; then echo "Usage: ROLLBACK.sh <absolute-target-directory>" >&2; exit 2; fi\n'
        'powershell="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}\\System32\\WindowsPowerShell\\v1.0\\powershell.exe")"\n'
        '"$powershell" -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/restore_release.ps1")" -Target "$1"\n',
        encoding="utf-8", newline="\n")
    script = r'''param([Parameter(Mandatory=$true)][string]$Target)
$ErrorActionPreference = 'Stop'
$Source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'baseline\release')).TrimEnd('\')
$Destination = [IO.Path]::GetFullPath($Target).TrimEnd('\')
$Allowed = @('__RELEASE__', [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'rollback_test')))
if ($Allowed -notcontains $Destination) { throw 'Target must be the recorded release or isolated rollback_test directory.' }
if (!(Test-Path -LiteralPath $Destination -PathType Container)) { throw 'Target directory must already exist.' }
function Assert-NoReparse([string]$Path) {
    $cursor = $Path
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse point rejected: $cursor" }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
Assert-NoReparse $Source
Assert-NoReparse $Destination
$Manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'baseline_release_hashes.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Entries = @()
foreach ($entry in $Manifest.PSObject.Properties) {
    $relative = $entry.Name.Replace('/', '\')
    $sourcePath = [IO.Path]::GetFullPath((Join-Path $Source $relative))
    $destPath = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
    if (!$sourcePath.StartsWith($Source + '\', [StringComparison]::OrdinalIgnoreCase) -or !$destPath.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escaped release root.' }
    Assert-NoReparse $sourcePath
    Assert-NoReparse $destPath
    if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw "Baseline hash mismatch: $relative" }
    $Entries += [PSCustomObject]@{Source=$sourcePath; Destination=$destPath; Hash=$entry.Value}
}
foreach ($entry in $Entries) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($entry.Destination)) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Source -Destination $entry.Destination -Force
    if ((Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Hash) { throw "Restored hash mismatch: $($entry.Destination)" }
}
Write-Output 'ROLLBACK_RESTORED=PASS; new_databases_preserved=true'
'''
    (TX / "restore_release.ps1").write_text(script.replace("__RELEASE__", str(RELEASE).replace("'", "''")), encoding="utf-8")


def verify_release_contents(release: Path, build_dir: Path) -> dict:
    """Check every shipped file, including files not named by the manifest."""
    if release.is_symlink() or getattr(release.lstat(), 'st_file_attributes', 0) & 0x400:
        raise AssertionError('release root must not be a reparse point')
    release, build_dir = release.resolve(), build_dir.resolve()
    paths = list(regular_tree(release))
    manifest_path = release / 'file_manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    entries = manifest.get('files')
    if not isinstance(entries, dict) or not entries:
        raise AssertionError('empty or missing release manifest files object')
    runtime_names = {
        'RelinkStudio.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Sql.dll', 'Qt6Network.dll',
        'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'qt.conf',
        'platforms/qwindows.dll', 'platforms/qoffscreen.dll',
        'sqldrivers/qsqlite.dll', 'sqldrivers/qsqlmimer.dll', 'sqldrivers/qsqlodbc.dll', 'sqldrivers/qsqlpsql.dll',
        'imageformats/qgif.dll', 'imageformats/qico.dll', 'imageformats/qjpeg.dll',
        'styles/qmodernwindowsstyle.dll', 'generic/qtuiotouchplugin.dll',
        'networkinformation/qnetworklistmanager.dll', 'tls/qcertonlybackend.dll', 'tls/qschannelbackend.dll',
        'README.md', 'FRONTEND_DELIVERY.md', 'docs/STORE_UI.md', 'docs/WIN11_DARK_UI.md',
        'docs/FLUENT_UI.md', 'docs/TERMINAL_UI.md',
    }
    license_root = ROOT / 'docs/third_party'
    license_names = {'third_party/' + p.relative_to(license_root).as_posix()
                     for p in regular_tree(license_root) if p.is_file()}
    allowed = runtime_names | license_names
    forbidden_parts = {'bbzps', 'sample', 'samples', 'sdk', 'sdks', '.tools', '.git', '__pycache__',
                       'node_modules', 'cache', 'keys', 'secrets', 'profiles', 'business'}
    forbidden_suffixes = {'.db', '.sqlite', '.sqlite3', '.key', '.pem', '.pfx', '.p12', '.ppk', '.pyc'}
    actual = {}
    for path in paths:
        relative = path.relative_to(release).as_posix()
        parts = {part.lower() for part in Path(relative).parts}
        lower = relative.lower()
        if (parts & forbidden_parts or 'bbzps' in lower or path.suffix.lower() in forbidden_suffixes
                or lower.endswith(('.sqlite-wal', '.sqlite-shm', '.sqlite-journal', '.db-wal', '.db-shm'))
                or path.name.lower() in {'id_rsa', 'id_ed25519', '.env'}):
            raise AssertionError('forbidden sample/database/key/SDK package path: ' + relative)
        if not path.resolve().is_relative_to(release):
            raise AssertionError('package path escaped root: ' + relative)
        if path.is_dir():
            continue
        if not path.is_file():
            raise AssertionError('non-regular package file: ' + relative)
        if path == manifest_path:
            continue
        if relative not in allowed:
            raise AssertionError('package file is not allowlisted: ' + relative)
        actual[relative] = sha(path)
    if set(actual) != set(entries):
        raise AssertionError('manifest file-set mismatch: missing=' + str(sorted(set(entries) - set(actual)))
                             + '; extra=' + str(sorted(set(actual) - set(entries))))
    for relative, entry in entries.items():
        if not isinstance(entry, dict) or actual[relative] != entry.get('sha256'):
            raise AssertionError('manifest hash mismatch: ' + relative)
        if (release / relative).stat().st_size != entry.get('bytes'):
            raise AssertionError('manifest byte-size mismatch: ' + relative)
    required = {'RelinkStudio.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Sql.dll',
                'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
                'platforms/qwindows.dll', 'platforms/qoffscreen.dll', 'sqldrivers/qsqlite.dll', 'qt.conf'}
    if not required <= actual.keys():
        raise AssertionError('missing required package dependencies: ' + str(sorted(required - actual.keys())))
    plugin_config = configparser.ConfigParser(interpolation=None, strict=True)
    plugin_config.optionxform = str
    try:
        plugin_config.read_string((release / 'qt.conf').read_text(encoding='utf-8'))
        isolated = (set(plugin_config.sections()) == {'Paths'}
                    and not plugin_config.defaults()
                    and dict(plugin_config['Paths']) == {'Prefix': '.', 'Plugins': '.'})
    except configparser.Error:
        isolated = False
    if not isolated:
        raise AssertionError('qt.conf does not isolate the package plugin root')
    built = build_dir / 'RelinkStudio.exe'
    if not built.is_file() or sha(built) != sha(release / 'RelinkStudio.exe'):
        raise AssertionError('current built executable hash differs from released executable')
    version_match = re.search(r'project\(RelinkStudio VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))
    if version_match is None or manifest.get('version') != version_match.group(1):
        raise AssertionError('manifest version differs from current CMake project version')
    return {'manifest_exact': True, 'allowlist_passed': True, 'file_count': len(actual),
            'application_version': manifest['version'], 'build_dir': str(build_dir), 'release_dir': str(release),
            'built_exe_sha256': sha(built), 'released_exe_sha256': actual['RelinkStudio.exe'],
            'forbidden_paths': [], 'files': actual}


def verify_cli_workspace(release: Path, output: Path, execute) -> dict:
    """Exercise actual CLI with explicit isolated files; never normal user state."""
    output = output.resolve()
    if output.exists() and any(output.iterdir()):
        raise AssertionError('CLI output must be fresh: ' + str(output))
    output.mkdir(parents=True, exist_ok=True)
    exe = release / 'RelinkStudio.exe'
    workspace = output / 'workspace'

    def call(label, args, inputs, required, env=None):
        return execute(label, [str(x) for x in args], inputs, required, env=env)

    def verify_ui(directory, full=False):
        data = json.loads((directory / 'ui_results.json').read_text(encoding='utf-8'))
        assert data['passed'] and data['offscreen'] and data['game_connected'] is False and data['system_input_sent'] is False
        if full:
            data = json.loads((directory / 'workspace/ui_results.json').read_text(encoding='utf-8'))
            assert data['passed'] and data['game_connected'] is False and data['system_input_sent'] is False
            assert data['image_file_write_count'] == 0

    call('SNAPSHOT_SEED', [exe, '--self-test', '--snapshot-dir', output/'seed', '--config', output/'seed-config.json',
                           '--workspace-dir', workspace],
         'Synthetic seed through packaged workflow; fresh explicit config/workspace; no user state.',
         ['UI_SELF_TEST=PASS', 'WORKSPACE_UI_SELF_TEST=PASS'])
    verify_ui(output/'seed', True)
    assert (workspace/'workspace.sqlite').is_file(), 'CLI did not use the explicit workspace directory'
    (workspace/'byte-sentinel.txt').write_text('preserve explicit workspace bytes\n', encoding='utf-8')
    original = directory_digest(workspace)
    config = output/'seed-config.json'
    config_hash = sha(config)
    for label, suffix, read_only in [('SNAPSHOT_WRITABLE', 'writable', False), ('SNAPSHOT_READONLY', 'readonly', True)]:
        arguments = [exe, '--snapshot-dir', output/suffix, '--config', config, '--workspace-dir', workspace]
        if read_only:
            arguments += ['--workspace-read-only']
        call(label, arguments,
             'Snapshot-only explicit ' + suffix + ' workspace; no --self-test; verify all directory names/bytes and config hash.',
             ['UI_SELF_TEST=PASS'])
        verify_ui(output/suffix)
        assert directory_digest(workspace) == original, label + ' changed workspace files'
        assert sha(config) == config_hash, label + ' changed configuration'
    isolated_temp = output/'isolated_temp'
    isolated_temp.mkdir()
    env = qenv(release)
    env.update(TEMP=str(isolated_temp), TMP=str(isolated_temp), TMPDIR=str(isolated_temp))
    call('STARTUP_DEFAULT_CONFIG', [exe, '--self-test', '--snapshot-dir', output/'default_startup'],
         'No --config or --workspace-dir; TEMP/TMP redirected; default headless workspace must be temporary and removed.',
         ['UI_SELF_TEST=PASS', 'WORKSPACE_UI_SELF_TEST=PASS'], env)
    verify_ui(output/'default_startup', True)
    default_config = isolated_temp/'RelinkStudioOffscreen/config.json'
    assert default_config.is_file(), 'default headless config did not use isolated TEMP'
    assert not list(isolated_temp.rglob('workspace.sqlite')), 'default temporary workspace persisted after process exit'
    source = (ROOT/'src/main.cpp').read_text(encoding='utf-8')
    default_business_source = ('QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)' in source
                               and 'QStringLiteral("/business")' in source and 'QTemporaryDir temporaryWorkspace' in source)
    assert default_business_source, 'normal/headless workspace path branches missing from startup source'
    return {'workspace_before': original, 'workspace_after': directory_digest(workspace),
            'config_before': config_hash, 'config_after': sha(config), 'snapshot_only_no_write': True,
            'headless_default_config': str(default_config), 'temporary_workspace_removed': True,
            'normal_app_local_data_business_branch': 'source_checked_not_launched',
            'game_connected': False, 'system_input_sent': False}


def modified(build_dir: Path) -> None:
    source_info = source_boundaries()
    _write_json(TX / "service_boundaries.json", source_info)
    manifest_evidence = verify_release_contents(RELEASE, build_dir)
    release_before = directory_digest(RELEASE)
    _write_json(TX / "release_manifest_checks.json", manifest_evidence)
    modified_file = TX / "MODIFIED_FILE.exe"
    shutil.copy2(RELEASE / "RelinkStudio.exe", modified_file)
    if sha(modified_file) == sha(BASELINE / "RelinkStudio.exe"):
        raise AssertionError("modified executable is byte-identical to the frozen baseline")

    ui("MODIFIED", RELEASE, TX / "snapshots")
    run(
        "MODIFIED_STORAGE",
        [str(RELEASE / "RelinkStudio.exe"), "--storage-self-test"],
        "Updated extracted package; QSQLITE plugin; temporary transactions/recovery; Qt offscreen.",
        ("STORAGE_SELF_TEST=PASS",),
        env=qenv(RELEASE),
    )
    workspace = _test_executable(build_dir, "workspace_tests")
    if workspace is None:
        raise RuntimeError(f"workspace_tests.exe not found under {build_dir}")
    run(
        "WORKSPACE_TESTS",
        [str(workspace)],
        "PR12B end-to-end synthetic workspace workflow: preview/review/save/select/replay/history/reopen/CSV; no game process.",
        ("WORKSPACE_TESTS=PASS",),
        env={**qenv(RELEASE), "QT_PLUGIN_PATH": str(RELEASE)},
    )
    ui_tests = _test_executable(build_dir, "ui_module_tests")
    if ui_tests is not None:
        run(
            "UI_MODULE_TESTS",
            [str(ui_tests)],
            "Independent UI module regression; offscreen; no game process or input injection.",
            ("UI_MODULE_TESTS=PASS",),
            env={**qenv(RELEASE), "QT_PLUGIN_PATH": str(RELEASE)},
        )

    cli_output = Path(tempfile.mkdtemp(prefix="cli_acceptance_", dir=TX))
    def cli_execute(label, command, inputs, required=(), env=None):
        return run(label, command, inputs, required, env=env or qenv(RELEASE))
    cli_evidence = verify_cli_workspace(RELEASE, cli_output, cli_execute)
    _write_json(TX / "cli_checks.json", cli_evidence)

    # Observation/worker are release gates, not silently optional targets.
    for name, required in (
        ("observation_adapter_tests", ("OBSERVATION_ADAPTER_TESTS", "failures=0")),
        ("worker_protocol_tests", ("WORKER_PROTOCOL_TESTS=PASS", "external_processes=0", "image_file_writes=0")),
        ("vision_pipeline_tests", ("VISION_PIPELINE_TESTS=PASS",)),
        ("shared_frame_memory_tests", ("SHARED_FRAME_MEMORY_TESTS=PASS", "image_file_writes=0")),
    ):
        executable = _test_executable(build_dir, name)
        if executable is None:
            raise RuntimeError(f"required focused target missing: {name}")
        arguments = [str(executable)]
        if name in {"worker_protocol_tests", "vision_pipeline_tests"}:
            arguments.append(str(ROOT / "docs/business_rebuild/implementation/runtime/fixtures/messages"))
        run(name.upper(), arguments,
            f"Focused synthetic {name}; explicit frozen fixtures for worker; no external process/capture/OCR.",
            required, env={**qenv(RELEASE), "QT_PLUGIN_PATH": str(RELEASE)})

    worker_process = _test_executable(build_dir, "worker_process_tests")
    fixture_child = _test_executable(build_dir, "vision_worker_fixture")
    if worker_process is None or fixture_child is None:
        raise RuntimeError("required isolated worker-process test or synthetic fixture child is missing")
    fixture_child = fixture_child.resolve()
    if fixture_child.is_relative_to(RELEASE.resolve()) or (RELEASE / fixture_child.name).exists():
        raise AssertionError("synthetic worker fixture must remain outside the desktop release package")
    run(
        "WORKER_PROCESS_TESTS",
        [str(worker_process.resolve()), str(fixture_child),
         str(ROOT / "docs/business_rebuild/implementation/runtime/fixtures/messages")],
        "Isolated relink_vision_transport test: actual paging-file-backed shared memory and hidden QProcess "
        "synthetic fixture child; not an OCR worker; no game process, window capture, model, input or transaction.",
        ("WORKER_PROCESS_TESTS=PASS", "failures=0"),
        env={**qenv(RELEASE), "QT_PLUGIN_PATH": str(RELEASE)},
    )
    _write_json(TX / "transport_test_targets.json", {
        "library": "relink_vision_transport",
        "worker_process_test": str(worker_process.resolve()),
        "fixture_child": str(fixture_child),
        "fixture_child_sha256": sha(fixture_child),
        "fixture_is_ocr_worker": False,
        "fixture_shipped_in_desktop_release": False,
        "desktop_product_behavior": "synthetic_replay_only",
    })
    run(
        "PACKAGE_HARNESS_TESTS",
        [sys.executable, "-I", "-X", "utf8", str(ROOT / "tests/release/test_package_harness.py")],
        "Negative acceptance-harness regression on tiny non-executable temporary files; "
        "actual rollback shell scripts only; no application or game process.",
        ("PACKAGE_HARNESS_TESTS=PASS", "failures=0", "errors=0", "fixture_only=true"),
        env=os.environ.copy(), timeout=300,
    )

    # Separate service targets remain optional; workspace_tests covers each seam.
    for name in (
        "profile_catalog_tests",
        "history_query_tests",
        "replay_scenario_tests",
        "records_exporter_tests",
    ):
        executable = _test_executable(build_dir, name)
        if executable is not None:
            run(
                name.upper(),
                [str(executable)],
                f"Focused PR12B service test target {name}; no game process or image capture.",
                ("PASS",),
                env={**qenv(RELEASE), "QT_PLUGIN_PATH": str(RELEASE)},
            )

    rollback = TX / "rollback_test"
    rollback.mkdir(parents=True, exist_ok=True)
    # Validate every existing path before copytree can traverse it; reruns may
    # keep their isolated sentinel files but cannot redirect a write via links.
    list(regular_tree(rollback))
    shutil.copytree(RELEASE, rollback, dirs_exist_ok=True)
    if sha(rollback / "RelinkStudio.exe") != sha(modified_file):
        raise AssertionError("rollback test must start from the modified executable")
    database = rollback / "preserve-new-ledger.sqlite"
    with sqlite3.connect(database) as connection:
        connection.execute("CREATE TABLE IF NOT EXISTS sentinel (id INTEGER PRIMARY KEY, value TEXT)")
        connection.execute("INSERT OR REPLACE INTO sentinel VALUES (1, 'preserve new ledger')")
    profile = rollback / "profiles" / "preserve-reviewed-profile.json"
    profile.parent.mkdir(parents=True, exist_ok=True)
    profile.write_text('{"enabled":false,"activation_required":true}\n', encoding="utf-8")
    saved = {str(path): sha(path) for path in (database, profile)}
    bash = Path(r"C:\Program Files\Git\bin\bash.exe")
    if not bash.is_file():
        raise RuntimeError(f"Git bash is required for rollback artifact: {bash}")
    write_rollback_scripts()
    run(
        "ROLLBACK",
        [
            str(bash),
            "-c",
            'script="$(cygpath -u "$1")"; chmod +x "$script" && "$script" "$2"',
            "rollback",
            str(TX / "ROLLBACK.sh"),
            str(rollback),
        ],
        "Separate updated package copy; restore frozen program files; retain new DB/profile sentinels.",
        ("ROLLBACK_RESTORED=PASS",),
        cwd=ROOT,
        env=os.environ.copy(),
    )
    expected = json.loads((TX / "baseline_release_hashes.json").read_text(encoding="utf-8"))
    _verify_manifest(rollback, expected, allow_extra=True)
    for path, digest in saved.items():
        if sha(Path(path)) != digest:
            raise AssertionError(f"rollback destroyed new data: {path}")
    ui("RESTORED", rollback, rollback / "ui_snapshots")
    run(
        "RESTORED_STORAGE",
        [str(rollback / "RelinkStudio.exe"), "--storage-self-test"],
        "Restored frozen package on isolated copy; temporary storage self-test; Qt offscreen.",
        ("STORAGE_SELF_TEST=PASS",),
        env=qenv(rollback),
    )
    if sha(RELEASE / "RelinkStudio.exe") != sha(modified_file):
        raise AssertionError("fixed release changed during isolated rollback test")
    if directory_digest(RELEASE) != release_before:
        raise AssertionError("isolated verification changed the fixed release directory")
    print("PR12B_PACKAGE_VERIFICATION=PASS; modified_file_retained=true; new_data_preserved=true", flush=True)


def finalize() -> None:
    commands_path = TX / "commands.json"
    if not commands_path.is_file():
        raise RuntimeError("commands.json is missing; run baseline/build/modified first")
    records = json.loads(commands_path.read_text(encoding="utf-8"))
    latest = {record["label"]: record for record in records}
    required_labels = {
        "BASELINE",
        "BASELINE_STORAGE",
        "MODIFIED_BUILD",
        "MODIFIED",
        "MODIFIED_STORAGE",
        "WORKSPACE_TESTS",
        "OBSERVATION_ADAPTER_TESTS",
        "WORKER_PROTOCOL_TESTS",
        "VISION_PIPELINE_TESTS",
        "SHARED_FRAME_MEMORY_TESTS",
        "WORKER_PROCESS_TESTS",
        "PACKAGE_HARNESS_TESTS",
        "SNAPSHOT_WRITABLE",
        "SNAPSHOT_READONLY",
        "STARTUP_DEFAULT_CONFIG",
        "ROLLBACK",
        "RESTORED",
        "RESTORED_STORAGE",
    }
    missing = sorted(required_labels - set(latest))
    if missing:
        raise RuntimeError(f"missing required evidence labels: {missing}")
    for label in required_labels:
        if latest[label]["exit_status"] != 0:
            raise RuntimeError(f"evidence command failed: {label}")
    revision = (TX / "source_revision.txt").read_text(encoding="ascii").strip()
    changed = set(
        subprocess.check_output(["git", "diff", "--name-only", revision], cwd=ROOT, text=True).splitlines()
    )
    changed.update(
        subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], cwd=ROOT, text=True).splitlines()
    )
    patch: list[str] = []
    for name in sorted(changed):
        if not (name.startswith(("src/", "tests/", "docs/")) or name in {".gitattributes", ".gitignore", "CMakeLists.txt", "build.ps1", "README.md", "SESSION_START.md"}):
            continue
        old_result = subprocess.run(["git", "show", f"{revision}:{name}"], cwd=ROOT, capture_output=True)
        old = old_result.stdout.decode("utf-8", errors="replace").splitlines(True) if old_result.returncode == 0 else []
        current = ROOT / name
        new = current.read_text(encoding="utf-8").splitlines(True) if current.is_file() else []
        patch.extend(difflib.unified_diff(old, new, fromfile="a/" + name, tofile="b/" + name))
    (TX / "DIFF_FILE").write_text("".join(patch), encoding="utf-8")
    modified_file = TX / "MODIFIED_FILE.exe"
    if not modified_file.is_file() or sha(modified_file) != sha(RELEASE / "RelinkStudio.exe"):
        raise RuntimeError("MODIFIED_FILE.exe is not the final fixed release executable")
    baseline_hash = sha(BASELINE / "RelinkStudio.exe")
    modified_hash = sha(modified_file)
    if baseline_hash == modified_hash:
        raise RuntimeError("final release is byte-identical to baseline")
    boundaries = json.loads((TX / "service_boundaries.json").read_text(encoding="utf-8"))
    lines = [
        "Delta Market Assistant - PR12B service/package verification",
        "Date: 2026-10-07 (Asia/Shanghai)",
        "Branch: main",
        "Changed branch/fields: WorkspaceController service seams; metadata-first history; in-memory observation; worker protocol and lease pool; isolated shared-memory/hidden synthetic process transport; snapshot-only no-write branch; parameterized package acceptance.",
        "Service boundary evidence: " + json.dumps(boundaries, ensure_ascii=False, sort_keys=True),
        "Compatibility retained: schema v2; ConfigV2/QSaveFile; objectNames; CLI; Win11 light UI; saved profile flags; synthetic replay semantics.",
        "M2 boundary: no live game capture, OCR, input injection, market connection, purchase, or runtime screenshot/temp-image writes are claimed or executed.",
        "Transport boundary: real paging-file-backed shared memory and hidden synthetic fixture child are tested outside the desktop package; the fixture is not an OCR worker; the desktop product remains replay-only.",
        f"DELIVERY: {RELEASE / 'RelinkStudio.exe'}",
        f"MODIFIED_FILE: {modified_file}",
        f"DIFF_FILE: {TX / 'DIFF_FILE'}",
        f"VERIFICATION: {TX / 'VERIFICATION.txt'}",
        f"ROLLBACK: {TX / 'ROLLBACK.sh'}",
        "BASELINE_SHA256: " + baseline_hash,
        "MODIFIED_SHA256: " + modified_hash,
        "Restored behavior/status: isolated rollback copy passed UI and storage self-tests with exit 0; new DB/profile sentinels were preserved; fixed release remained modified.",
        "Runtime image-file writes: 0 in workspace UI evidence; PNG snapshots are explicit offscreen test artifacts only.",
        "",
        "Exact commands, inputs, literal output, and exit status:",
    ]
    for record in records:
        lines.extend([
            "",
            record["label"],
            "COMMAND: " + record["command"],
            "INPUT: " + record["input"],
            "STDOUT:",
            record["stdout"].rstrip(),
            "STDERR:",
            record["stderr"].rstrip() or "(empty)",
            "EXIT_STATUS: " + str(record["exit_status"]),
        ])
    (TX / "VERIFICATION.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    for path in (modified_file, TX / "DIFF_FILE", TX / "VERIFICATION.txt", TX / "ROLLBACK.sh"):
        if not path.is_file() or not path.read_bytes():
            raise RuntimeError(f"required artifact missing/empty: {path}")
        print("REOPEN=PASS; path=" + str(path), flush=True)
    print("PR12B_TRANSACTION_VERIFICATION=PASS", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("baseline", "build", "modified", "finalize"))
    parser.add_argument("--build-dir", type=Path, default=BUILD_DEFAULT)
    args = parser.parse_args()
    if args.phase == "modified":
        modified(args.build_dir.resolve())
    else:
        globals()[args.phase]()
