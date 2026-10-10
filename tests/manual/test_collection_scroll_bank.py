"""Offline measured-bank loading and CLI selection; no game backend starts."""
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import MagicMock, patch

import run_collection_trial as runner
from test_collection_scroll_evidence import measured_fixture


class ScrollBankTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.artifacts = self.root / 'artifacts'
        self.artifacts.mkdir()
        self.default = self.artifacts / 'collection_scroll_speed' / 'calibration_bank.json'
        self.default.parent.mkdir()
        root_patch = patch.object(runner, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)

    def profile(self, name='a', delta=-480):
        profile, evidence = measured_fixture()
        profile['delta'] = delta
        evidence['delta'] = delta
        evidence['profile_parameters']['delta'] = delta
        evidence_path = self.artifacts / f'{name}_measurement.json'
        evidence_path.write_text(json.dumps(evidence), encoding='utf-8')
        profile['source'].update(record_path=str(evidence_path),
            record_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest())
        path = self.artifacts / f'{name}_profile.json'
        path.write_text(json.dumps(profile), encoding='utf-8')
        entry = dict(path=path.relative_to(self.root).as_posix(),
                     sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        return path, entry, profile

    def bank(self, entries, path=None):
        path = path or self.default
        path.write_text(json.dumps(dict(schema='collection-scroll-calibration-bank-v1', profiles=entries)),
                        encoding='utf-8')
        return path

    def test_each_profile_is_independently_loaded_and_declared_hash_bound(self):
        first, second = self.profile('a'), self.profile('b', -600)
        bank = self.bank([first[1], second[1]])
        with patch.object(runner, 'load_scroll_profile', wraps=runner.load_scroll_profile) as load:
            profiles, source = runner.load_scroll_profile_bank(bank)
        self.assertEqual(profiles, [first[2], second[2]])
        self.assertEqual([call.args[0] for call in load.call_args_list], [first[0], second[0]])
        self.assertEqual(source['profile_count'], 2)
        self.assertEqual(source['sha256'], hashlib.sha256(bank.read_bytes()).hexdigest())
        self.assertTrue(source['profile_hashes_verified'])
        self.assertTrue(source['evidence_semantics_verified'])
        self.assertTrue(all(value['declared_sha256'] == value['sha256'] for value in source['profiles']))

    def test_valid_profile_with_changed_bytes_fails_bank_hash(self):
        path, entry, unused = self.profile()
        bank = self.bank([entry])
        path.write_bytes(path.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'BANK_PROFILE_HASH'):
            runner.load_scroll_profile_bank(bank)

    def test_bad_evidence_is_not_accepted_because_profile_hash_matches(self):
        path, entry, profile = self.profile()
        Path(profile['source']['record_path']).write_text('{}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'PROFILE_EVIDENCE_HASH'):
            runner.load_scroll_profile_bank(self.bank([entry]))

    def test_absolute_escape_traversal_and_missing_member_paths_fail(self):
        path, entry, unused = self.profile()
        for value in (str(path), '../outside.json', 'artifacts/../outside.json',
                      'artifacts/missing.json', 'not_artifacts/a_profile.json'):
            bank = self.bank([dict(entry, path=value)])
            with self.subTest(path=value), self.assertRaisesRegex(ValueError, 'BANK_PROFILE_PATH'):
                runner.load_scroll_profile_bank(bank)

    def test_bank_itself_must_exist_under_artifacts_as_json(self):
        for value in (self.root / 'outside.json', self.artifacts / 'missing.json',
                      self.artifacts / 'bank.txt'):
            with self.subTest(path=value), self.assertRaisesRegex(ValueError, 'BANK_PATH'):
                runner.load_scroll_profile_bank(value)

    def test_duplicate_member_is_rejected_not_ignored(self):
        unused, entry, unused_profile = self.profile()
        with self.assertRaisesRegex(ValueError, 'BANK_DUPLICATE'):
            runner.load_scroll_profile_bank(self.bank([entry, copy.deepcopy(entry)]))

    def test_empty_oversized_bad_json_and_invalid_schemas_fail(self):
        for value, error in [(b'', 'SIZE'), (b'x' * (512 * 1024 + 1), 'SIZE'),
                             (b'{', 'JSON'), (b'[]', 'SCHEMA'),
                             (b'{"schema":"collection-scroll-calibration-bank-v1","profiles":[]}', 'SCHEMA')]:
            self.default.write_bytes(value)
            with self.subTest(error=error, size=len(value)), self.assertRaisesRegex(ValueError, 'BANK_' + error):
                runner.load_scroll_profile_bank(self.default)

    def test_malformed_entry_or_hash_is_rejected(self):
        for entry in (None, {}, {'path': 'artifacts/a.json', 'sha256': 'wrong'}):
            with self.subTest(entry=entry), self.assertRaisesRegex(ValueError, 'BANK_ENTRY'):
                runner.load_scroll_profile_bank(self.bank([entry]))

    def test_project_default_bank_is_selected_when_present(self):
        unused, entry, profile = self.profile()
        self.bank([entry])
        single, profiles, source = runner.select_scroll_configuration()
        self.assertIsNone(single)
        self.assertEqual(profiles, [profile])
        self.assertEqual(source['selection'], 'default_bank')

    def test_absent_default_keeps_existing_legacy_behavior(self):
        self.assertEqual(runner.select_scroll_configuration(), (None, None, None))

    def test_explicit_legacy_does_not_read_even_invalid_default_bank(self):
        self.default.write_text('not-json', encoding='utf-8')
        with patch.object(runner, 'load_scroll_profile_bank') as load:
            single, profiles, source = runner.select_scroll_configuration(legacy=True)
        load.assert_not_called()
        self.assertIsNone(single)
        self.assertIsNone(profiles)
        self.assertEqual(source['selection'], 'explicit_legacy')

    def test_explicit_single_overrides_invalid_default_bank(self):
        path, unused, profile = self.profile()
        self.default.write_text('not-json', encoding='utf-8')
        single, profiles, source = runner.select_scroll_configuration(profile_path=path)
        self.assertEqual(single, profile)
        self.assertIsNone(profiles)
        self.assertEqual(source['selection'], 'explicit_profile')

    def test_explicit_bank_overrides_default_and_missing_does_not_fall_back(self):
        unused, entry, profile = self.profile()
        self.default.write_text('not-json', encoding='utf-8')
        chosen = self.bank([entry], self.artifacts / 'explicit_bank.json')
        single, profiles, source = runner.select_scroll_configuration(bank_path=chosen)
        self.assertIsNone(single)
        self.assertEqual(profiles, [profile])
        self.assertEqual(source['selection'], 'explicit_bank')
        with self.assertRaisesRegex(ValueError, 'BANK_PATH'):
            runner.select_scroll_configuration(bank_path=self.artifacts / 'missing.json')

    def test_invalid_default_does_not_silently_fall_back_to_legacy(self):
        self.default.write_text('not-json', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'BANK_JSON'):
            runner.select_scroll_configuration()

    def test_cli_options_and_helper_both_enforce_mutual_exclusion(self):
        required = ['--snapshot', 'a.json', '--output', 'b']
        choices = [['--scroll-profile', 'p.json'], ['--scroll-bank', 'b.json'], ['--legacy-scroll']]
        for index, first in enumerate(choices):
            for second in choices[index + 1:]:
                with self.subTest(first=first, second=second), contextlib.redirect_stderr(io.StringIO()), \
                        self.assertRaises(SystemExit) as error:
                    runner.build_argument_parser().parse_args(required + first + second)
                self.assertEqual(error.exception.code, 2)
        for options in (dict(profile_path='a', bank_path='b'), dict(profile_path='a', legacy=True),
                        dict(bank_path='b', legacy=True)):
            with self.assertRaisesRegex(ValueError, 'CONFIGURATION_AMBIGUOUS'):
                runner.select_scroll_configuration(**options)

    def test_main_passes_default_bank_into_trial_constructor_without_starting_real_backend(self):
        unused, entry, profile = self.profile()
        self.bank([entry])
        snapshot = self.artifacts / 'snapshot.json'
        snapshot.write_text('{}', encoding='utf-8')
        fake_session = MagicMock()
        fake_session.report = dict(ide_restored=True)
        fake_trial = MagicMock()
        fake_trial.summary = dict(task_file_fully_completed=True, status='completed')
        fake_trial.started = time.monotonic()
        with patch('sys.argv', ['runner', '--snapshot', str(snapshot), '--output', str(self.artifacts / 'output')]), \
                patch('collection_live_session.ForegroundSession', return_value=fake_session), \
                patch.object(runner, 'CollectionTrial', return_value=fake_trial) as trial, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runner.main(), 0)
        self.assertIsNone(trial.call_args.kwargs['scroll_profile'])
        self.assertEqual(trial.call_args.kwargs['scroll_profile_bank'], [profile])
        self.assertEqual(trial.call_args.kwargs['scroll_profile_source']['selection'], 'default_bank')


if __name__ == '__main__':
    unittest.main()
