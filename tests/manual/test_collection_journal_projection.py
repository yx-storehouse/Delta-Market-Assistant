"""Small decision projection retains current strong metadata and pending checks."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from collection_journal import CollectionJournal,candidate_key
from collection_scroll import collection_disposition
from test_collection_journal_history import candidate,complete


class JournalProjectionTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.directory=Path(self.tmp.name);self.journal=CollectionJournal(self.directory)

    def test_same_disposition_without_nested_old_evidence(self):
        old=candidate('old');complete(self.journal,old)
        full=self.journal.records();brief=self.journal.decision_records()
        self.assertNotIn('evidence',brief[0]);self.assertNotIn('receipt',brief[0])
        self.assertEqual(set(brief[0]['candidate']),{'product','condition','wear'})
        fresh=candidate('fresh')
        self.assertEqual(collection_disposition(fresh,fresh['favorite_before'],full),
                         collection_disposition(fresh,fresh['favorite_before'],brief))
        brief[0]['candidate']['product']='changed'
        self.assertEqual(self.journal.records(),full)

    def test_competing_pending_is_seen_and_atomic_prepare_still_blocks(self):
        complete(self.journal,candidate('old'));self.journal.decision_records()
        other=CollectionJournal(self.directory)
        other.prepare(candidate('new-pending','0.999999'),{},allow_confirmed_history=True)
        records=self.journal.decision_records()
        self.assertTrue(any(r['status']=='prepared' for r in records))
        fresh=candidate('fresh')
        with self.assertRaisesRegex(ValueError,'PENDING_RECONCILIATION'):
            collection_disposition(fresh,fresh['favorite_before'],records)
        with self.assertRaisesRegex(FileExistsError,'PENDING_RECONCILIATION'):
            self.journal.prepare(fresh,{},allow_confirmed_history=True)

    def test_v2_history_keys_and_current_white_semantics_are_preserved(self):
        complete(self.journal,candidate('first'))
        key=self.journal.prepare(candidate('second'),{'synthetic':True},allow_confirmed_history=True)
        self.journal.update(key,'dispatched',sent=2)
        self.journal.update(key,'confirmed',receipt=candidate('second-receipt'))
        fresh=candidate('third');short=self.journal.decision_records();full=self.journal.records()
        self.assertEqual(len(short),2)
        self.assertEqual(collection_disposition(fresh,fresh['favorite_before'],short),
                         collection_disposition(fresh,fresh['favorite_before'],full))
        self.assertTrue(any(r.get('schema')=='collection-attempt-v2' for r in short))

    def test_deleted_or_identity_corrupt_history_is_not_hidden_by_projection(self):
        key,_=complete(self.journal,candidate('first'));self.journal.decision_records()
        path=self.directory/(key+'.json');record=json.loads(path.read_text('utf8'))
        record['candidate']['wear']='0.999998';path.write_text(json.dumps(record),'utf8')
        with self.assertRaisesRegex(ValueError,'JOURNAL_IDENTITY'):self.journal.decision_records()
        path.unlink();self.assertEqual(self.journal.decision_records(),[])


if __name__=='__main__':unittest.main()
