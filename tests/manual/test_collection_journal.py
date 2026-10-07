import copy
import json
from pathlib import Path
import tempfile
import unittest
from collection_journal import CollectionJournal, receipt_matches


class JournalTests(unittest.TestCase):
    def setUp(self):
        self.candidate = dict(product='AUG-天命',condition='成色S',price='230',wear='0.187079',row_index=0,source_frame_sha256='a'*64)

    def test_prepare_survives_restart_and_blocks_reissue(self):
        with tempfile.TemporaryDirectory() as directory:
            first = CollectionJournal(directory)
            key = first.prepare(self.candidate, dict(record='before'))
            reopened = CollectionJournal(directory)
            self.assertEqual(json.loads((Path(directory)/(key+'.json')).read_text(encoding='utf-8'))['status'],'prepared')
            with self.assertRaises(FileExistsError): reopened.prepare(self.candidate, {})
            repriced=dict(self.candidate,price='231')
            with self.assertRaises(FileExistsError): reopened.prepare(repriced,{})
            reopened.update(key,'dispatched',sent=2)
            reopened.update(key,'confirmed',record='after')
            with self.assertRaises(ValueError):reopened.update(key,'dispatched')

    def test_partial_input_stays_uncertain(self):
        with tempfile.TemporaryDirectory() as directory:
            journal=CollectionJournal(directory);key=journal.prepare(self.candidate,{})
            journal.update(key,'input_uncertain',sent=1)
            with self.assertRaises(FileExistsError):journal.prepare(self.candidate,{})
            with self.assertRaises(ValueError):journal.update(key,'confirmed')

    def test_receipt_identity_color_and_fresh_frame(self):
        after=dict(self.candidate,source_frame_sha256='b'*64)
        packet=dict(startup_page=dict(anchor_checks={'collection.added':True}),collection_selected_card=dict(favorite_warm_fraction=.13,favorite_bright_fraction=0))
        self.assertTrue(receipt_matches(self.candidate,after,packet))
        for field,value in [('product','P90'),('condition','成色A'),('wear','0.1'),('price','231'),('source_frame_sha256','a'*64)]:
            changed=dict(after);changed[field]=value
            self.assertFalse(receipt_matches(self.candidate,changed,packet))
        for field,value in [('favorite_warm_fraction',0),('favorite_bright_fraction',.1)]:
            changed=copy.deepcopy(packet);changed['collection_selected_card'][field]=value
            self.assertFalse(receipt_matches(self.candidate,after,changed))
        packet['startup_page']['anchor_checks']['collection.added']=False
        self.assertFalse(receipt_matches(self.candidate,after,packet))
        self.candidate['favorite_before']=dict(favorite_warm_fraction=0,favorite_bright_fraction=.13)
        self.assertTrue(receipt_matches(self.candidate,after,packet))
        self.candidate['favorite_before']['favorite_warm_fraction']=.13
        self.assertFalse(receipt_matches(self.candidate,after,packet))


if __name__=='__main__':unittest.main(verbosity=2)
