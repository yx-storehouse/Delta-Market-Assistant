"""Session history caching: real temporary files, fake candidates, no game."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import collection_journal as module
from collection_journal import CollectionJournal, candidate_key
from test_collection_journal_history import candidate, complete


class JournalCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.journal = CollectionJournal(self.directory)

    def history(self, index=0, *, status='confirmed'):
        value = candidate('history-' + str(index), '0.' + str(index + 100000))
        record = dict(key=candidate_key(value), status=status, candidate=value,
                      evidence={'synthetic': True, 'nested': {'value': 1}})
        path = self.directory / (record['key'] + '.json')
        path.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
        return path, record

    def test_unchanged_confirmed_history_is_read_once_and_bytes_stay_unchanged(self):
        originals = {path: path.read_bytes() for path, unused in (self.history(i) for i in range(4))}
        with mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            first = self.journal.records()
            for unused in range(3):
                self.assertEqual(self.journal.records(), first)
            self.assertEqual(read.call_count, 4)
        self.assertEqual({path: path.read_bytes() for path in originals}, originals)
        self.assertEqual({p.name for p in self.directory.iterdir()},
                         {p.name for p in originals} | {'.collection-reservation.lock'})

    def test_new_file_only_reads_that_document_and_deleted_file_is_removed(self):
        old, unused = self.history()
        self.journal.records()
        new, added = self.history(1)
        with mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.assertEqual(len(self.journal.records()), 2)
            self.assertEqual([call.args[0] for call in read.call_args_list], [new])
            old.unlink()
            self.assertEqual(self.journal.records(), [added])
            self.assertEqual(read.call_count, 1)
        self.assertNotIn(old.name, self.journal._confirmed_cache)

    def test_changed_record_is_revalidated_even_when_size_and_mtime_are_restored(self):
        path, record = self.history()
        self.journal.records()
        before = path.stat()
        signature = module._record_signature(path)
        record['candidate']['price'] = '231'
        path.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertEqual(path.stat().st_size, before.st_size)
        self.assertEqual(path.stat().st_mtime_ns, before.st_mtime_ns)
        self.assertNotEqual(module._record_signature(path), signature)
        with mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.assertEqual(self.journal.records()[0]['candidate']['price'], '231')
            self.assertEqual(read.call_count, 1)

    def test_atomic_replacement_with_restored_size_and_time_is_not_old_identity(self):
        path, record = self.history()
        self.journal.records()
        before = path.stat()
        record['candidate']['price'] = '232'
        replacement = path.with_suffix('.tmp')
        replacement.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
        os.utime(replacement, ns=(before.st_atime_ns, before.st_mtime_ns))
        os.replace(replacement, path)
        with mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.assertEqual(self.journal.records()[0]['candidate']['price'], '232')
            self.assertEqual(read.call_count, 1)

    def test_each_pending_state_is_always_read_and_never_cached(self):
        for index, status in enumerate(('prepared', 'dispatched', 'input_uncertain')):
            self.history(index, status=status)
        with mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.journal.records()
            self.journal.records()
            self.assertEqual(read.call_count, 6)
        self.assertEqual(self.journal._confirmed_cache, {})
        with self.assertRaisesRegex(FileExistsError, 'PENDING_RECONCILIATION'):
            self.journal.prepare(candidate('fresh'), {}, allow_confirmed_history=True)

    def test_warm_reader_checks_competing_pending_again_inside_prepare(self):
        self.history()
        self.journal.records()
        other = CollectionJournal(self.directory)
        value = candidate('other-pending', '0.999999')
        key = other.prepare(value, {}, allow_confirmed_history=True)
        with self.assertRaisesRegex(FileExistsError, 'PENDING_RECONCILIATION'):
            self.journal.prepare(candidate('fresh'), {}, allow_confirmed_history=True)
        self.assertEqual(json.loads((self.directory / (key + '.json')).read_text('utf-8'))['status'], 'prepared')
        self.assertEqual(len(list(self.directory.glob('*.json'))), 2)

    def test_external_pending_state_replaces_previously_cached_confirmation(self):
        path, record = self.history()
        for status in ('prepared', 'dispatched', 'input_uncertain'):
            with self.subTest(status=status):
                record['status'] = 'confirmed'
                path.write_text(json.dumps(record), encoding='utf-8')
                self.journal.records()
                self.assertIn(path.name, self.journal._confirmed_cache)
                record['status'] = status
                path.write_text(json.dumps(record), encoding='utf-8')
                with self.assertRaisesRegex(FileExistsError, 'PENDING_RECONCILIATION'):
                    self.journal.prepare(candidate('fresh'), {}, allow_confirmed_history=True)
                self.assertNotIn(path.name, self.journal._confirmed_cache)

    def test_returned_nested_records_are_detached_from_cache(self):
        unused, original = self.history()
        first = self.journal.records()
        first[0]['candidate']['price'] = '0'
        first[0]['evidence']['nested']['value'] = 999
        first.append({'status': 'prepared'})
        self.assertEqual(self.journal.records(), [original])

    def test_missing_strong_metadata_means_reread_not_weak_cache_hit(self):
        self.history()
        self.journal.records()
        with mock.patch.object(module, '_record_signature', return_value=None), \
                mock.patch.object(module, '_directory_signatures', return_value=None), \
                mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.journal.records()
            self.journal.records()
            self.assertEqual(read.call_count, 2)
            self.assertEqual(self.journal._confirmed_cache, {})

    @unittest.skipUnless(os.name == 'nt', 'Windows ChangeTime API behavior')
    def test_windows_api_failure_returns_no_signature(self):
        path, unused = self.history()
        with mock.patch.object(module, '_windows_metadata_api', side_effect=OSError('metadata unavailable')):
            self.assertIsNone(module._record_signature(path))

    @unittest.skipUnless(os.name == 'nt', 'Windows ChangeTime API behavior')
    def test_change_time_query_failure_closes_handles_and_rereads_cached_history(self):
        self.history()
        self.journal.records()
        api = list(module._windows_metadata_api())
        original_close = api[-1]
        closed = []
        def close(handle):
            closed.append(handle)
            return original_close(handle)
        api[4] = lambda *unused: False  # GetFileInformationByHandleEx failed.
        api[-1] = close
        with mock.patch.object(module, '_windows_metadata_api', return_value=tuple(api)), \
                mock.patch.object(module, '_directory_signatures', return_value=None), \
                mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.journal.records()
            self.journal.records()
            self.assertEqual(read.call_count, 2)
            self.assertEqual(len(closed), 4)  # Before/after metadata for both reads.
            self.assertEqual(self.journal._confirmed_cache, {})

    @unittest.skipUnless(os.name == 'nt', 'Windows directory index metadata')
    def test_unchanged_directory_entries_skip_per_file_metadata(self):
        paths = [self.history(i)[0] for i in range(3)]
        self.journal.records()
        with mock.patch.object(module, '_record_signature', wraps=module._record_signature) as strong, \
                mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.journal.records()
            self.journal.records()
            self.assertEqual((strong.call_count, read.call_count), (0, 0))
        listed = module._directory_signatures(self.directory)
        self.assertEqual({name: value for name, value in listed.items() if name.endswith('.json')},
                         {path.name: module._record_signature(path) for path in paths})

    @unittest.skipUnless(os.name == 'nt', 'Windows directory index metadata')
    def test_directory_entry_change_time_detects_restored_size_and_mtime(self):
        path, record = self.history()
        self.journal.records()
        listed = module._directory_signatures(self.directory)[path.name]
        before = path.stat()
        record['candidate']['price'] = '233'
        path.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertNotEqual(module._directory_signatures(self.directory)[path.name], listed)
        with mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.assertEqual(self.journal.records()[0]['candidate']['price'], '233')
            self.assertEqual(read.call_count, 1)

    def test_changed_during_read_and_malformed_changed_document_stop(self):
        path, unused = self.history()
        with mock.patch.object(module, '_record_signature', side_effect=[('old',), ('new',)]):
            with self.assertRaisesRegex(ValueError, 'CHANGED_DURING_READ'):
                self.journal.records()
        self.assertEqual(self.journal._confirmed_cache, {})
        self.journal.records()
        path.write_text('{broken', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'JOURNAL_DOCUMENT'):
            self.journal.prepare(candidate('fresh'), {}, allow_confirmed_history=True)

    def test_identity_changes_in_cached_file_are_rejected(self):
        path, record = self.history()
        self.journal.records()
        record['candidate']['wear'] = '0.777777'
        path.write_text(json.dumps(record), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'JOURNAL_IDENTITY'):
            self.journal.records()

    def test_records_and_prepare_refresh_hold_the_existing_reservation_lock(self):
        self.history()
        original_lock, original_signature = module._reservation_lock, module._record_signature
        held = [0]
        @contextmanager
        def tracked_lock(directory):
            with original_lock(directory):
                held[0] += 1
                try:
                    yield
                finally:
                    held[0] -= 1
        def checked_signature(path):
            self.assertEqual(held[0], 1)
            return original_signature(path)
        with mock.patch.object(module, '_reservation_lock', tracked_lock), \
                mock.patch.object(module, '_record_signature', checked_signature):
            self.journal.records()
            self.journal.prepare(candidate('fresh'), {}, allow_confirmed_history=True)
        self.assertEqual(held[0], 0)

    def test_own_updates_cache_only_confirmed_readback_and_leave_history_bytes(self):
        path, unused = self.history()
        original = path.read_bytes()
        self.journal.records()
        key = self.journal.prepare(candidate('fresh'), {}, allow_confirmed_history=True)
        self.assertNotIn(key + '.json', self.journal._confirmed_cache)
        self.journal.update(key, 'dispatched', sent=2)
        self.assertNotIn(key + '.json', self.journal._confirmed_cache)
        with mock.patch.object(self.journal, '_read_record', wraps=self.journal._read_record) as read:
            self.journal.update(key, 'confirmed', receipt=candidate('receipt'))
            self.assertEqual(read.call_count, 1)
            self.assertEqual(len(self.journal.records()), 2)
            self.assertEqual(read.call_count, 1)
        self.assertEqual(path.read_bytes(), original)


class WarmCacheConcurrencyTests(unittest.TestCase):
    def test_two_pre_warmed_processes_reserve_only_one_attempt(self):
        script = (
            'import json,pathlib,sys,time;sys.path.insert(0,sys.argv[1]);'
            'from collection_journal import CollectionJournal;'
            'directory=pathlib.Path(sys.argv[2]); index=sys.argv[3]; value=json.loads(sys.argv[4]);'
            'journal=CollectionJournal(directory);journal.records();'
            '(directory/("ready-"+index)).write_text("ready");deadline=time.monotonic()+10\n'
            'while not (directory/"start.marker").exists():\n'
            ' if time.monotonic()>deadline:raise RuntimeError("barrier timeout")\n'
            ' time.sleep(.005)\n'
            'try:\n'
            ' key=journal.prepare(value,{},allow_confirmed_history=True)\n'
            ' print(json.dumps({"reserved":key}))\n'
            'except FileExistsError as error:\n'
            ' print(json.dumps({"blocked":str(error)}))\n')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            journal = CollectionJournal(root)
            old, unused = complete(journal, candidate('old'))
            original = (root / (old + '.json')).read_bytes()
            processes = []
            try:
                for index in range(2):
                    command = [sys.executable, '-B', '-X', 'utf8', '-c', script,
                               str(Path(__file__).resolve().parent), directory, str(index),
                               json.dumps(candidate('concurrent-' + str(index)), ensure_ascii=False)]
                    processes.append(subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True, encoding='utf-8', creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)))
                deadline = time.monotonic() + 10
                while not all((root / ('ready-' + str(i))).exists() for i in range(2)):
                    if time.monotonic() > deadline:
                        self.fail('Warm-cache barrier timeout')
                    time.sleep(.005)
                (root / 'start.marker').write_text('go', encoding='ascii')
                outcomes = []
                for process in processes:
                    out, error = process.communicate(timeout=20)
                    self.assertEqual(process.returncode, 0, error)
                    outcomes.append(json.loads(out))
                self.assertEqual(sum('reserved' in item for item in outcomes), 1)
                self.assertEqual(sum(item.get('blocked') == 'COLLECTION_PENDING_RECONCILIATION'
                                     for item in outcomes), 1)
                self.assertEqual((root / (old + '.json')).read_bytes(), original)
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()


if __name__ == '__main__':
    unittest.main()
