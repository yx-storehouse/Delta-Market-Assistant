"""Stationary full peers and a dehighlighted clipped card allow reads only."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession, selection_readiness_waiting
from collection_scroll import rebind_selected, selection_plan
from test_collection_live_session import Clock, FakeBackend, capture
from test_collection_selection_coherent_shift import fresh_packet

ROOT=Path(__file__).resolve().parents[2]


class DehighlightReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'artifacts/collection_scroll_speed/white_star_run_04/session.json'
        if not path.is_file():raise unittest.SkipTest('local recorded run04 absent')
        run=json.loads(path.read_text('utf-8'))
        cls.pending=run['pending_geometry']
        cls.before=run['steps'][-3]['result']
        cls.after=run['steps'][-1]['result']

    def test_actual_frame_is_readiness_only_and_original_selection_still_fails(self):
        frozen=copy.deepcopy((self.pending,self.after))
        result=selection_readiness_waiting(self.after,self.pending)
        self.assertEqual(result['reason'],'dehighlighted_partial_no_selected_card')
        self.assertEqual(result['measured_common_vertical_shift_px'],0)
        self.assertEqual(result['independent_non_target_peer_count'],3)
        self.assertFalse(result['selected_observed'])
        self.assertFalse(result['collection_allowed'])
        with self.assertRaisesRegex(ValueError,'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
            rebind_selected(self.after,self.pending['rule'],self.pending['lease'])
        self.assertEqual((self.pending,self.after),frozen)

    def test_nonselected_partial_or_full_peer_extra_motion_is_rejected(self):
        for index in (0,3,4,6,7):
            changed=copy.deepcopy(self.after)
            card=changed['collection_layout']['cards'][index]
            if card['fields_bounds'] is None:
                card['bounds'][0]+=3;card['bounds'][2]-=3
            else:card['bounds'][1]+=1;card['bounds'][3]-=1
            with self.subTest(index=index):
                self.assertIsNone(selection_readiness_waiting(changed,self.pending))

    def test_same_frame_or_another_selected_card_does_not_enable_retry(self):
        stale=copy.deepcopy(self.after)
        stale['collection_observation']['frame_id']=self.pending['wait_layout']['frame_id']
        self.assertIsNone(selection_readiness_waiting(stale,self.pending))
        other=copy.deepcopy(self.after);other['collection_layout']['cards'][3]['selected']=True
        self.assertIsNone(selection_readiness_waiting(other,self.pending))

    def test_bounded_new_reads_do_not_reclick_or_clear_pending_without_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            clock=Clock();backend=FakeBackend(clock,replies=[copy.deepcopy(self.before),
                copy.deepcopy(self.after),fresh_packet(self.after,'b'),fresh_packet(self.after,'c')])
            session=ForegroundSession('artifacts/readiness/session.json',root=Path(directory),
                backend=backend,now=clock.now,wait=clock.wait,fast_settle=True)
            with self.assertRaisesRegex(RuntimeError,'BATCH_STEP_FAILED'):
                with session:
                    session.perform(capture(collection_layout=True))
                    for step in selection_plan(session.previous,self.pending['rule'],self.pending['lease']['card']['id'])['steps']:
                        session.perform(step)
            self.assertEqual(session.steps[-1]['attempt_count'],3)
            self.assertEqual(session.report['manual_clicks'],1)
            self.assertIsNotNone(session.pending_geometry)
            self.assertIsNone(session.selected_lease)
            self.assertIsNone(session.pending)
            self.assertEqual(list(session.journal.directory.glob('*.json')),[])
            self.assertTrue(session.report['ide_restored'])


if __name__=='__main__':unittest.main()
