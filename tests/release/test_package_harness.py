"""Negative acceptance-harness regression using tiny non-executable fixtures.

No packaged application, Qt DLL, game, OCR model, or worker child is executed.
Windows rollback checks execute only the generated shell/PowerShell restoration
scripts against byte fixtures under a newly created temporary directory.
"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("verify_pr12b_package.py")


def load_harness():
    spec = importlib.util.spec_from_file_location("package_harness_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class PackageHarnessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="relink-package-harness-")
        self.root = Path(self.temporary.name).resolve()
        self.addCleanup(self.temporary.cleanup)
        self.h = load_harness()
        self.h.ROOT = self.root
        self.release = self.root / "dist/RelinkStudio"
        self.build = self.root / "build"
        self.release.mkdir(parents=True)
        self.build.mkdir()
        self.h.RELEASE = self.release
        self.h.TX = self.root / "transaction"
        self.h.TX.mkdir()
        self.h.BASELINE = self.h.TX / "baseline/release"
        (self.root / "docs/third_party").mkdir(parents=True)
        (self.root / "docs/third_party/LICENSE.txt").write_text("fixture license\n", encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text("project(RelinkStudio VERSION 0.6.0 LANGUAGES CXX)\n", encoding="utf-8")
        for relative in (
            "RelinkStudio.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Sql.dll",
            "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll",
            "platforms/qwindows.dll", "platforms/qoffscreen.dll", "sqldrivers/qsqlite.dll",
            "qt.conf", "third_party/LICENSE.txt",
        ):
            path = self.release / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("NON_EXECUTABLE_TEST_FIXTURE:" + relative).encode("ascii"))
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\n", encoding="utf-8")
        (self.build / "RelinkStudio.exe").write_bytes((self.release / "RelinkStudio.exe").read_bytes())
        self.rebuild_manifest()

    def rebuild_manifest(self):
        files = {}
        for path in self.release.rglob("*"):
            if path.is_file() and path.name != "file_manifest.json":
                files[path.relative_to(self.release).as_posix()] = {"bytes": path.stat().st_size, "sha256": self.h.sha(path)}
        self.manifest = {"version": "0.6.0", "files": files}
        self.save_manifest()

    def save_manifest(self):
        (self.release / "file_manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def verify(self):
        return self.h.verify_release_contents(self.release, self.build)

    def assert_rejected(self, pattern):
        with self.assertRaisesRegex(AssertionError, pattern):
            self.verify()

    def symlink(self, target, link, directory=False):
        try:
            os.symlink(target, link, target_is_directory=directory)
        except OSError as error:
            self.skipTest("test platform did not grant local symlink creation: " + str(error))

    def test_minimal_exact_manifest_accepts_files_and_skips_directories(self):
        result = self.verify()
        self.assertTrue(result["manifest_exact"])
        self.assertEqual(result["file_count"], 13)
        self.assertNotIn("platforms", result["files"])
        self.assertNotIn("file_manifest.json", result["files"])

    def test_changed_payload_rejected(self):
        (self.release / "Qt6Core.dll").write_bytes(b"changed")
        self.assert_rejected("manifest hash mismatch")

    def test_missing_file_rejected(self):
        (self.release / "Qt6Core.dll").unlink()
        self.assert_rejected("manifest file-set mismatch")

    def test_extra_unlisted_allowed_file_rejected(self):
        (self.release / "README.md").write_text("unrecorded", encoding="utf-8")
        self.assert_rejected("manifest file-set mismatch")

    def test_manifest_entry_without_file_rejected(self):
        self.manifest["files"]["README.md"] = {"bytes": 0, "sha256": "0" * 64}
        self.save_manifest()
        self.assert_rejected("manifest file-set mismatch")

    def test_manifest_entry_omission_rejected(self):
        del self.manifest["files"]["Qt6Core.dll"]
        self.save_manifest()
        self.assert_rejected("manifest file-set mismatch")

    def test_manifest_size_corruption_rejected(self):
        self.manifest["files"]["Qt6Core.dll"]["bytes"] += 1
        self.save_manifest()
        self.assert_rejected("manifest byte-size mismatch")

    def test_manifest_hash_corruption_rejected(self):
        self.manifest["files"]["Qt6Core.dll"]["sha256"] = "0" * 64
        self.save_manifest()
        self.assert_rejected("manifest hash mismatch")

    def test_manifest_empty_rejected(self):
        self.manifest["files"] = {}
        self.save_manifest()
        self.assert_rejected("empty or missing release manifest")

    def test_manifest_scalar_entry_rejected(self):
        self.manifest["files"]["Qt6Core.dll"] = "invalid"
        self.save_manifest()
        self.assert_rejected("manifest hash mismatch")

    def test_manifest_parent_escape_rejected_without_outside_mutation(self):
        outside = self.release.parent / "outside.txt"
        outside.write_bytes(b"unchanged")
        self.manifest["files"]["../outside.txt"] = {"bytes": 9, "sha256": self.h.sha(outside)}
        self.save_manifest()
        self.assert_rejected("manifest file-set mismatch")
        self.assertEqual(outside.read_bytes(), b"unchanged")

    def test_manifest_absolute_path_rejected(self):
        self.manifest["files"][str(self.root / "outside.txt")] = {"bytes": 0, "sha256": "0" * 64}
        self.save_manifest()
        self.assert_rejected("manifest file-set mismatch")

    def test_test_worker_executable_never_allowlisted(self):
        for name in ("synthetic_worker_child.exe", "vision_worker_fixture.exe"):
            with self.subTest(child=name):
                path = self.release / name
                path.write_bytes(b"NON_EXECUTABLE_TEST_FIXTURE")
                self.rebuild_manifest()
                self.assert_rejected("not allowlisted")
                path.unlink()

    def test_private_database_rejected_even_when_manifested(self):
        (self.release / "workspace.sqlite").write_bytes(b"fixture database")
        self.rebuild_manifest()
        self.assert_rejected("forbidden sample/database/key/SDK")

    def test_private_key_rejected_even_when_manifested(self):
        (self.release / "id_ed25519").write_text("NOT_A_KEY", encoding="ascii")
        self.rebuild_manifest()
        self.assert_rejected("forbidden sample/database/key/SDK")

    def test_forbidden_empty_cache_directory_rejected(self):
        (self.release / "cache").mkdir()
        self.assert_rejected("forbidden sample/database/key/SDK")

    def test_sqlite_sidecar_rejected(self):
        (self.release / "workspace.sqlite-wal").write_bytes(b"fixture")
        self.rebuild_manifest()
        self.assert_rejected("forbidden sample/database/key/SDK")

    def test_required_dependency_missing_even_with_rebuilt_manifest(self):
        (self.release / "platforms/qoffscreen.dll").unlink()
        self.rebuild_manifest()
        self.assert_rejected("missing required package dependencies")

    def test_plugin_root_not_isolated_rejected(self):
        (self.release / "qt.conf").write_text("[Paths]\nPlugins=other\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_plugin_parent_path_not_accepted_as_dot_prefix(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=..\\other\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_plugin_forward_slash_parent_path_rejected(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=../other\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_commented_plugin_dot_does_not_mask_external_path(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\n# Plugins=.\nPlugins=C:\\external\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_duplicate_plugin_keys_rejected(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\nPlugins=..\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("qt.conf")

    def test_default_inherited_plugin_paths_rejected(self):
        (self.release / "qt.conf").write_text("[DEFAULT]\nPrefix=.\nPlugins=.\n[Paths]\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_extra_qt_config_section_rejected(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\n[Other]\nkey=value\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_extra_qt_path_field_rejected(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\nLibraries=..\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_plugin_key_case_mismatch_rejected(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=.\nplugins=.\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_absolute_prefix_rejected_even_when_plugins_dot(self):
        (self.release / "qt.conf").write_text("[Paths]\nPrefix=C:\\external\nPlugins=.\n", encoding="utf-8")
        self.rebuild_manifest()
        self.assert_rejected("does not isolate")

    def test_build_binary_mismatch_rejected(self):
        (self.build / "RelinkStudio.exe").write_bytes(b"different build")
        self.assert_rejected("built executable hash differs")

    def test_build_binary_missing_rejected(self):
        (self.build / "RelinkStudio.exe").unlink()
        self.assert_rejected("built executable hash differs")

    def test_version_mismatch_rejected(self):
        self.manifest["version"] = "99.0"
        self.save_manifest()
        self.assert_rejected("manifest version differs")

    def test_file_symlink_rejected_before_hash_read(self):
        outside = self.root / "outside.txt"
        outside.write_bytes(b"fixture")
        self.symlink(outside, self.release / "README.md")
        self.assert_rejected("reparse package path")

    def test_directory_symlink_rejected_before_descent(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "secret.txt").write_bytes(b"fixture")
        self.symlink(outside, self.release / "docs", True)
        self.assert_rejected("reparse package path")

    def test_root_symlink_rejected(self):
        alias = self.root / "release-alias"
        self.symlink(self.release, alias, True)
        with self.assertRaisesRegex(AssertionError, "root must not be a reparse"):
            self.h.verify_release_contents(alias, self.build)

    def test_manifest_walk_includes_all_regular_files(self):
        path = self.root / "tree"
        (path / "nested").mkdir(parents=True)
        (path / "nested/baseline_release_hashes.json").write_bytes(b"included")
        result = self.h._manifest(path)
        self.assertEqual(set(result), {"nested/baseline_release_hashes.json"})

    def test_directory_digest_detects_names_empty_dirs_and_bytes(self):
        path = self.root / "digest"
        path.mkdir()
        first = self.h.directory_digest(path)
        (path / "empty").mkdir()
        second = self.h.directory_digest(path)
        self.assertNotEqual(first, second)
        (path / "value").write_bytes(b"one")
        third = self.h.directory_digest(path)
        (path / "value").write_bytes(b"two")
        self.assertNotEqual(second, third)
        self.assertNotEqual(third, self.h.directory_digest(path))

    def setup_rollback(self):
        if os.name != "nt":
            self.skipTest("generated rollback uses Windows PowerShell")
        self.bash = Path(r"C:\Program Files\Git\bin\bash.exe")
        if not self.bash.is_file():
            self.skipTest("Git Bash required for executable rollback regression")
        self.h.BASELINE.mkdir(parents=True)
        self.rollback = self.h.TX / "rollback_test"
        self.rollback.mkdir()
        for name in ("RelinkStudio.exe", "Qt6Core.dll"):
            (self.h.BASELINE / name).write_bytes(("baseline " + name).encode("ascii"))
            (self.rollback / name).write_bytes(("modified " + name).encode("ascii"))
        (self.rollback / "new-ledger.sqlite").write_bytes(b"preserve fixture ledger")
        self.rollback_manifest = {name: self.h.sha(self.h.BASELINE / name) for name in ("RelinkStudio.exe", "Qt6Core.dll")}
        self.save_rollback_manifest()
        self.h.write_rollback_scripts()

    def save_rollback_manifest(self):
        (self.h.TX / "baseline_release_hashes.json").write_text(json.dumps(self.rollback_manifest), encoding="utf-8")

    def execute_rollback(self, target):
        script = self.h.TX / "ROLLBACK.sh"
        return subprocess.run(
            [str(self.bash), "-c", 'script="$(cygpath -u "$1")"; chmod +x "$script" && "$script" "$2"',
             "rollback-negative-test", str(script), str(target)],
            cwd=self.root, env=os.environ.copy(), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=45,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

    def test_actual_rollback_restores_modified_fixture_and_preserves_data(self):
        self.setup_rollback()
        baseline_digest = self.h.directory_digest(self.h.BASELINE)
        script = self.h.TX / "ROLLBACK.sh"
        self.assertNotIn(b"\r", script.read_bytes())
        result = self.execute_rollback(self.rollback)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ROLLBACK_RESTORED=PASS", result.stdout)
        for name in self.rollback_manifest:
            self.assertEqual((self.rollback / name).read_bytes(), (self.h.BASELINE / name).read_bytes())
        self.assertEqual((self.rollback / "new-ledger.sqlite").read_bytes(), b"preserve fixture ledger")
        self.assertEqual(self.h.directory_digest(self.h.BASELINE), baseline_digest)

    def test_rollback_wrong_target_rejected_without_writes(self):
        self.setup_rollback()
        outside = self.root / "unrelated"
        outside.mkdir()
        (outside / "RelinkStudio.exe").write_bytes(b"untouched")
        before = self.h.directory_digest(self.root)
        result = self.execute_rollback(outside)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Target must be the recorded release", result.stderr)
        self.assertEqual(self.h.directory_digest(self.root), before)

    def test_rollback_corrupt_baseline_preflight_prevents_partial_restore(self):
        self.setup_rollback()
        (self.h.BASELINE / "Qt6Core.dll").write_bytes(b"corrupt second entry")
        before = self.h.directory_digest(self.rollback)
        result = self.execute_rollback(self.rollback)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Baseline hash mismatch", result.stderr)
        self.assertEqual(self.h.directory_digest(self.rollback), before)

    def test_rollback_manifest_escape_preflight_prevents_writes(self):
        self.setup_rollback()
        self.rollback_manifest["../outside.txt"] = "0" * 64
        self.save_rollback_manifest()
        before = self.h.directory_digest(self.rollback)
        result = self.execute_rollback(self.rollback)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Manifest path escaped", result.stderr)
        self.assertEqual(self.h.directory_digest(self.rollback), before)

    def test_rollback_destination_symlink_rejected(self):
        self.setup_rollback()
        outside = self.root / "outside-target.txt"
        outside.write_bytes(b"outside unchanged")
        (self.rollback / "Qt6Core.dll").unlink()
        self.symlink(outside, self.rollback / "Qt6Core.dll")
        before = self.h.directory_digest(self.rollback)
        result = self.execute_rollback(self.rollback)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Reparse point rejected", result.stderr)
        self.assertEqual(self.h.directory_digest(self.rollback), before)
        self.assertEqual(outside.read_bytes(), b"outside unchanged")


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PackageHarnessTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print("PACKAGE_HARNESS_TESTS=" + ("PASS" if result.wasSuccessful() else "FAIL")
          + f"; tests={result.testsRun}; failures={len(result.failures)}; errors={len(result.errors)}"
          + f"; skipped={len(result.skipped)}; application_processes=0; game_processes=0; fixture_only=true")
    raise SystemExit(0 if result.wasSuccessful() else 1)
