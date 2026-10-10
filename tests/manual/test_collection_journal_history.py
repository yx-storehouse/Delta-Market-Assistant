"""Current-white attempt reservations; pure fixtures, no desktop/game access."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from collection_journal import CollectionJournal, candidate_key, receipt_matches, validate_record_identity


def candidate(frame='current', wear='0.187079'):
    return dict(product='AUG-天命', condition='成色S', price='230', wear=wear,
                row_index=0, source_frame_id=frame,
                source_frame_sha256=hashlib.sha256(frame.encode()).hexdigest(),
                eligible=True, age_at_action_ms=12,
                favorite_before=dict(selected=True, favorite_warm_fraction=0,
                                     favorite_bright_fraction=.13))


def complete(journal, before, *, reconciliation=False):
    key = journal.prepare(before, {'synthetic': True})
    after = candidate(before['source_frame_id'] + '-receipt', before['wear'])
    if reconciliation:
        journal.update(key, 'confirmed_reconciliation', reconciliation={'candidate': after})
    else:
        journal.update(key, 'dispatched', sent=2)
        journal.update(key, 'confirmed', receipt=after)
    return key, after


class CurrentWhiteHistoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.journal = CollectionJournal(self.directory)

    def records(self):
        return {path.name: path.read_bytes() for path in self.directory.glob('*.json')}

    def test_first_current_white_attempt_keeps_legacy_key(self):
        value = candidate()
        key = self.journal.prepare(value, {}, allow_confirmed_history=True)
        self.assertEqual(key, candidate_key(value))
        record = json.loads(self.records()[key + '.json'])
        self.assertNotIn('schema', record)
        self.assertEqual(validate_record_identity(record), key)

    def test_confirmed_white_creates_v2_and_keeps_original_bytes(self):
        old, _ = complete(self.journal, candidate('old'))
        before = self.records()
        current = candidate('new')
        key = self.journal.prepare(current, {'frame': 'new'}, allow_confirmed_history=True)
        self.assertNotEqual(key, old)
        record = json.loads(self.records()[key + '.json'])
        self.assertEqual(record['schema'], 'collection-attempt-v2')
        self.assertEqual(record['identity_key'], candidate_key(current))
        self.assertRegex(record['attempt_id'], r'^[0-9a-f]{32}$')
        self.assertEqual(validate_record_identity(record), key)
        self.assertEqual(before[old + '.json'], self.records()[old + '.json'])
        self.journal.update(key, 'dispatched', sent=2)
        self.journal.update(key, 'confirmed', receipt=candidate('new-receipt'))
        self.assertEqual(before[old + '.json'], self.records()[old + '.json'])
        self.assertEqual(json.loads(self.records()[key + '.json'])['status'], 'confirmed')

    def test_default_still_blocks_confirmed_exact_identity(self):
        complete(self.journal, candidate('old'))
        with self.assertRaises(FileExistsError):
            self.journal.prepare(candidate('new'), {})

    def test_reconciliation_allows_new_white_but_never_rewrites_old_record(self):
        old, _ = complete(self.journal, candidate('old'), reconciliation=True)
        before = self.records()[old + '.json']
        key = self.journal.prepare(candidate('new'), {}, allow_confirmed_history=True)
        self.assertNotEqual(old, key)
        self.assertEqual(before, self.records()[old + '.json'])

    def test_wear_decimal_normalization_finds_history_without_rekeying_it(self):
        old, _ = complete(self.journal, candidate('old', '0.187079'))
        before = self.records()[old + '.json']
        value = candidate('new', '0.1870790')
        self.assertNotEqual(old, candidate_key(value))
        key = self.journal.prepare(value, {}, allow_confirmed_history=True)
        record = json.loads(self.records()[key + '.json'])
        self.assertEqual(record['schema'], 'collection-attempt-v2')
        self.assertEqual(record['identity_key'], candidate_key(value))
        self.assertEqual(before, self.records()[old + '.json'])

    def test_price_position_and_rule_row_changes_do_not_hide_history(self):
        complete(self.journal, candidate('old'))
        value = candidate('new')
        value.update(price='231', row_index=4, bounds=[10, 20, 30, 40])
        key = self.journal.prepare(value, {}, allow_confirmed_history=True)
        self.assertIn('.', key)

    def test_different_item_with_only_completed_history_keeps_first_key(self):
        complete(self.journal, candidate('old'))
        value = candidate('new')
        value['product'] = 'P90-天命'
        key = self.journal.prepare(value, {}, allow_confirmed_history=True)
        self.assertEqual(key, candidate_key(value))

    def test_all_unfinished_states_block_same_and_unrelated_items(self):
        for state in ('prepared', 'dispatched', 'input_uncertain'):
            for same in (True, False):
                with self.subTest(state=state, same=same), tempfile.TemporaryDirectory() as directory:
                    journal = CollectionJournal(directory)
                    key = journal.prepare(candidate('pending'), {})
                    if state != 'prepared':
                        journal.update(key, state, sent=2 if state == 'dispatched' else 1)
                    before = {p.name: p.read_bytes() for p in Path(directory).glob('*.json')}
                    value = candidate('fresh', '0.1870790')
                    if not same:
                        value['product'] = 'P90-天命'
                    with self.assertRaisesRegex(FileExistsError, 'COLLECTION_PENDING_RECONCILIATION'):
                        journal.prepare(value, {}, allow_confirmed_history=True)
                    self.assertEqual(before, {p.name: p.read_bytes() for p in Path(directory).glob('*.json')})

    def test_second_unfinished_v2_reservation_blocks_another_uuid(self):
        complete(self.journal, candidate('old'))
        first = self.journal.prepare(candidate('new'), {}, allow_confirmed_history=True)
        with self.assertRaisesRegex(FileExistsError, 'COLLECTION_PENDING_RECONCILIATION'):
            self.journal.prepare(candidate('newer'), {}, allow_confirmed_history=True)
        self.assertEqual(len(self.records()), 2)
        self.assertEqual(json.loads(self.records()[first + '.json'])['status'], 'prepared')

    def test_candidate_and_receipt_frame_id_cannot_be_reused_even_with_new_hash(self):
        _, receipt = complete(self.journal, candidate('old'))
        before = self.records()
        for origin in (candidate('old'), receipt):
            with self.subTest(origin=origin['source_frame_id']):
                value = candidate('fresh', '0.1870790')
                value['source_frame_id'] = origin['source_frame_id']
                self.assertNotEqual(value['source_frame_sha256'], origin['source_frame_sha256'])
                with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_STALE_FRAME'):
                    self.journal.prepare(value, {}, allow_confirmed_history=True)
                self.assertEqual(before, self.records())

    def test_reconciliation_frame_cannot_be_reused(self):
        _, receipt = complete(self.journal, candidate('old'), reconciliation=True)
        value = candidate('fresh')
        value['source_frame_id'] = receipt['source_frame_id']
        self.assertNotEqual(value['source_frame_sha256'], receipt['source_frame_sha256'])
        with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_STALE_FRAME'):
            self.journal.prepare(value, {}, allow_confirmed_history=True)

    def test_independently_captured_static_pixels_do_not_block_new_white_attempt(self):
        for reconciliation in (False, True):
            for previous_observation in ('candidate', 'receipt'):
                with self.subTest(reconciliation=reconciliation, previous_observation=previous_observation), \
                        tempfile.TemporaryDirectory() as directory:
                    journal = CollectionJournal(directory)
                    old, receipt = complete(journal, candidate('old'), reconciliation=reconciliation)
                    path = Path(directory) / (old + '.json')
                    original = path.read_bytes()
                    origin = candidate('old') if previous_observation == 'candidate' else receipt
                    value = candidate('fresh', '0.1870790')
                    value['source_frame_sha256'] = origin['source_frame_sha256']
                    self.assertNotEqual(value['source_frame_id'], origin['source_frame_id'])
                    key = journal.prepare(value, {}, allow_confirmed_history=True)
                    self.assertNotEqual(key, old)
                    self.assertEqual(path.read_bytes(), original)
                    self.assertEqual(json.loads((Path(directory) / (key + '.json')).read_text(encoding='utf-8'))['schema'],
                                     'collection-attempt-v2')

    def test_static_pixels_exception_does_not_bypass_age_white_or_eligibility(self):
        complete(self.journal, candidate('old'))
        for field in ('age_at_action_ms', 'favorite_before', 'eligible'):
            with self.subTest(field=field):
                value = candidate('fresh')
                value['source_frame_sha256'] = candidate('old')['source_frame_sha256']
                value[field] = {'age_at_action_ms': 5000.01, 'favorite_before': dict(selected=True,
                    favorite_warm_fraction=.13, favorite_bright_fraction=0), 'eligible': False}[field]
                with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_CURRENT_WHITE_REQUIRED'):
                    self.journal.prepare(value, {}, allow_confirmed_history=True)
        self.assertEqual(len(self.records()), 1)

    def test_white_to_gold_receipt_still_requires_changed_image_hash(self):
        before = candidate('before')
        after = candidate('after')
        packet = dict(startup_page=dict(anchor_checks={'collection.added': True}),
                      collection_selected_card=dict(favorite_warm_fraction=.13, favorite_bright_fraction=0))
        self.assertTrue(receipt_matches(before, after, packet))
        after['source_frame_sha256'] = before['source_frame_sha256']
        self.assertNotEqual(before['source_frame_id'], after['source_frame_id'])
        self.assertFalse(receipt_matches(before, after, packet))

    def test_all_prior_v2_candidate_and_receipt_frames_remain_checked(self):
        complete(self.journal, candidate('old'))
        key = self.journal.prepare(candidate('second'), {}, allow_confirmed_history=True)
        self.journal.update(key, 'dispatched', sent=2)
        self.journal.update(key, 'confirmed', receipt=candidate('second-receipt'))
        before = self.records()
        for frame in ('old', 'old-receipt', 'second', 'second-receipt'):
            with self.subTest(frame=frame), self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_STALE_FRAME'):
                self.journal.prepare(candidate(frame), {}, allow_confirmed_history=True)
        self.assertEqual(before, self.records())

    def test_current_white_fields_are_all_required_and_strict(self):
        changes = [
            ('favorite_before', None), ('favorite_before', {}),
            ('favorite_before.selected', False), ('favorite_before.selected', 1),
            ('favorite_before.favorite_warm_fraction', .01),
            ('favorite_before.favorite_warm_fraction', False),
            ('favorite_before.favorite_warm_fraction', float('nan')),
            ('favorite_before.favorite_bright_fraction', .039),
            ('favorite_before.favorite_bright_fraction', 1.1),
            ('favorite_before.favorite_bright_fraction', float('inf')),
            ('eligible', False), ('eligible', 1), ('source_frame_id', ''),
            ('source_frame_id', ' '), ('source_frame_id', None),
            ('source_frame_sha256', 'g' * 64), ('source_frame_sha256', 'a' * 63),
            ('source_frame_sha256', None), ('age_at_action_ms', -1),
            ('age_at_action_ms', 5000.01), ('age_at_action_ms', True),
            ('age_at_action_ms', float('nan')), ('age_at_action_ms', float('inf')),
        ]
        for field, replacement in changes:
            with self.subTest(field=field, replacement=replacement):
                value = candidate()
                if '.' in field:
                    group, name = field.split('.')
                    value[group][name] = replacement
                else:
                    value[field] = replacement
                with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_CURRENT_WHITE_REQUIRED'):
                    self.journal.prepare(value, {}, allow_confirmed_history=True)
                self.assertEqual(self.records(), {})

    def test_age_and_brightness_inclusive_boundaries(self):
        for age in (0, 5000):
            with self.subTest(age=age), tempfile.TemporaryDirectory() as directory:
                value = candidate()
                value['age_at_action_ms'] = age
                value['favorite_before']['favorite_bright_fraction'] = .04
                CollectionJournal(directory).prepare(value, {}, allow_confirmed_history=True)

    def test_invalid_or_nonfinite_identity_stops_without_new_record(self):
        for wear in ('NaN', 'Infinity', '-0.1', '', 'not-decimal'):
            with self.subTest(wear=wear), self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_IDENTITY'):
                self.journal.prepare(candidate(wear=wear), {}, allow_confirmed_history=True)
        self.assertEqual(self.records(), {})

    def test_malformed_or_renamed_record_stops_without_overwrite(self):
        old, _ = complete(self.journal, candidate('old'))
        path = self.directory / (old + '.json')
        renamed = self.directory / ('b' * 64 + '.json')
        path.rename(renamed)
        before = self.records()
        with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_IDENTITY'):
            self.journal.prepare(candidate('new'), {}, allow_confirmed_history=True)
        self.assertEqual(before, self.records())
        renamed.write_text('{broken', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_DOCUMENT'):
            self.journal.prepare(candidate('new'), {}, allow_confirmed_history=True)
        self.assertEqual(renamed.read_text(), '{broken')

    def test_reserved_identity_fields_and_completed_records_cannot_be_rewritten(self):
        old, _ = complete(self.journal, candidate('old'))
        before = self.records()
        with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_TRANSITION'):
            self.journal.update(old, 'confirmed', receipt=candidate('other'))
        for field in ('candidate', 'evidence', 'schema', 'identity_key', 'attempt_id'):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_RESERVED_FIELD'):
                self.journal.update(old, 'dispatched', **{field: 'changed'})
        self.assertEqual(before, self.records())


class RecordIdentityTests(unittest.TestCase):
    def test_legacy_and_v2_return_their_actual_attempt_keys(self):
        value = candidate()
        identity = candidate_key(value)
        self.assertEqual(validate_record_identity(dict(key=identity, candidate=value)), identity)
        attempt = 'a' * 32
        key = identity + '.' + attempt
        self.assertEqual(validate_record_identity(dict(schema='collection-attempt-v2', key=key,
                         identity_key=identity, attempt_id=attempt, candidate=value)), key)

    def test_tampered_schema_key_candidate_and_v2_fields_are_rejected(self):
        value = candidate()
        identity = candidate_key(value)
        record = dict(schema='collection-attempt-v2', key=identity + '.' + 'a' * 32,
                      identity_key=identity, attempt_id='a' * 32, candidate=value)
        for field, replacement in [('schema', 'unknown'), ('key', identity),
                                   ('identity_key', 'b' * 64), ('attempt_id', 'a' * 31),
                                   ('attempt_id', 'g' * 32), ('attempt_id', None),
                                   ('candidate', {}), ('candidate', candidate(wear='0.1'))]:
            with self.subTest(field=field, replacement=replacement):
                changed = copy.deepcopy(record)
                changed[field] = replacement
                with self.assertRaisesRegex(ValueError, '^COLLECTION_JOURNAL_IDENTITY$'):
                    validate_record_identity(changed)
        for malformed in (None, [], {}, dict(key=identity, candidate=value, attempt_id='a' * 32)):
            with self.subTest(malformed=malformed), self.assertRaisesRegex(ValueError, '^COLLECTION_JOURNAL_IDENTITY$'):
                validate_record_identity(malformed)


class MultiprocessReservationTests(unittest.TestCase):
    def command(self, script, *args):
        return [sys.executable, '-B', '-X', 'utf8', '-c', script, str(Path(__file__).resolve().parent), *args]

    def test_parallel_current_white_prepares_reserve_only_one_uuid(self):
        script = (
            'import json,pathlib,sys,time;sys.path.insert(0,sys.argv[1]);'
            'from collection_journal import CollectionJournal;'
            'directory=pathlib.Path(sys.argv[2]);value=json.loads(sys.argv[3]);'
            'deadline=time.monotonic()+10\n'
            'while not (directory/"start.marker").exists():\n'
            ' if time.monotonic()>deadline:raise RuntimeError("barrier timeout")\n'
            ' time.sleep(.005)\n'
            'try:\n'
            ' key=CollectionJournal(directory).prepare(value,{},allow_confirmed_history=True)\n'
            ' print(json.dumps({"reserved":key}))\n'
            'except FileExistsError as error:\n'
            ' print(json.dumps({"blocked":str(error)}))\n')
        with tempfile.TemporaryDirectory() as directory:
            journal = CollectionJournal(directory)
            old, _ = complete(journal, candidate('old'))
            original = (Path(directory) / (old + '.json')).read_bytes()
            processes = []
            try:
                for index in range(4):
                    command = self.command(script, directory, json.dumps(candidate('parallel-' + str(index)), ensure_ascii=False))
                    processes.append(subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True, encoding='utf-8', creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)))
                (Path(directory) / 'start.marker').write_text('go', encoding='ascii')
                outcomes = []
                for process in processes:
                    out, error = process.communicate(timeout=20)
                    self.assertEqual(process.returncode, 0, error)
                    outcomes.append(json.loads(out))
                self.assertEqual(sum('reserved' in item for item in outcomes), 1)
                self.assertEqual(sum(item.get('blocked') == 'COLLECTION_PENDING_RECONCILIATION' for item in outcomes), 3)
                records = list(Path(directory).glob('*.json'))
                self.assertEqual(len(records), 2)
                self.assertEqual((Path(directory) / (old + '.json')).read_bytes(), original)
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()

    def test_process_crash_releases_lock_even_though_lock_file_remains(self):
        script = ('import os,pathlib,sys;sys.path.insert(0,sys.argv[1]);'
                  'from collection_journal import _reservation_lock\n'
                  'with _reservation_lock(pathlib.Path(sys.argv[2])):\n'
                  ' print("LOCK_HELD",flush=True)\n'
                  ' os._exit(7)\n')
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(self.command(script, directory), capture_output=True,
                text=True, encoding='utf-8', timeout=15,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertEqual(result.returncode, 7)
            self.assertIn('LOCK_HELD', result.stdout)
            self.assertTrue((Path(directory) / '.collection-reservation.lock').is_file())
            key = CollectionJournal(directory).prepare(candidate(), {}, allow_confirmed_history=True)
            self.assertEqual(key, candidate_key(candidate()))


if __name__ == '__main__':
    unittest.main(verbosity=2)
