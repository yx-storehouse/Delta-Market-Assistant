"""Negative-only native gate: bounded fresh reads, no business-field shortcut."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession, selection_preflight_waiting
from collection_scroll import selection_plan, observe_layout
from collection_candidate import match_selected_first_card
import test_collection_live_session as fake
from test_collection_clipped_tail_readiness import FIXTURE


def deferred(packet):
    packet=copy.deepcopy(packet)
    layout=packet['collection_layout']
    packet.update(ocr_passed=False,recognition_performed=False,page_match_passed=False,
        startup_page=dict(page='unobserved',overlay='unobserved',valid_input=False,actions_enabled=False),
        ocr=dict(provider='not_invoked',coverage='none',frame_sha256=layout['frame_sha256'],frame_age_at_result_ms=95),
        ocr_session=dict(request_count=0))
    packet['collection_observation']['regions']=[]
    packet.pop('collection_selected_card',None)
    packet['collection_selection_preflight']=dict(schema='collection-selection-preflight-v1',
        scope='negative_readiness_gate_only',frame_id=layout['frame_id'],frame_sha256=layout['frame_sha256'],
        same_frame=True,layout_complete=layout['complete'],selected_count=0,ready_for_full_ocr=False,
        ocr_deferred=True,actions_enabled=False,page_classified=False,identity_proven=False,
        requires_original_full_validation=True)
    return packet


class SelectionPreflightTests(unittest.TestCase):
    def setUp(self):
        self.data=json.loads(FIXTURE.read_text('utf8'))
        self.packet=deferred(self.data['after'])
        self.pending=self.data['pending']

    def test_deferred_frame_is_not_a_page_layout_or_candidate_proof(self):
        self.assertTrue(selection_preflight_waiting(self.packet,self.pending))
        with self.assertRaisesRegex(ValueError,'COLLECTION_PAGE'):
            observe_layout(self.packet)
        with self.assertRaisesRegex(ValueError,'COLLECTION_PAGE'):
            match_selected_first_card(self.packet,self.pending['rule'])

    def test_no_pending_selection_no_reforged_scope_no_stale_frame(self):
        self.assertFalse(selection_preflight_waiting(self.packet,None))
        for field,value in (('scope','full_validation'),('ready_for_full_ocr',True),('ocr_deferred',False),
            ('same_frame',False),('frame_id','old'),('frame_sha256','e'*64),('actions_enabled',True),
            ('page_classified',True),('identity_proven',True),('selected_count',1),
            ('requires_original_full_validation',False)):
            packet=copy.deepcopy(self.packet)
            packet['collection_selection_preflight'][field]=value
            with self.subTest(field=field):self.assertFalse(selection_preflight_waiting(packet,self.pending))
        for section,field,value in (('ocr_session','request_count',1),('ocr','provider','fake'),
                ('startup_page','page','skin_listings')):
            packet=copy.deepcopy(self.packet);packet[section][field]=value
            self.assertFalse(selection_preflight_waiting(packet,self.pending))

    def test_only_pending_selection_enables_flag_and_retry_never_reclicks(self):
        with tempfile.TemporaryDirectory() as directory:
            clock=fake.Clock();backend=fake.FakeBackend(clock,[self.data['before'],self.packet,self.packet])
            backend.reuse_capture=True
            session=ForegroundSession('artifacts/preflight/session.json',root=Path(directory),
                backend=backend,now=clock.now,wait=clock.wait,fast_settle=True)
            with self.assertRaisesRegex(RuntimeError,'BATCH_STEP_FAILED'):
                with session:
                    before=session.perform(fake.capture(collection_layout=True))
                    self.assertNotIn('--collection-selection-preflight',before['command'])
                    plan=selection_plan(session.previous,self.pending['rule'],self.pending['lease']['card']['id'])
                    session.perform(plan['steps'][0])
                    session.perform(dict(plan['steps'][1],attempts=2))
            step=session.steps[-1]
            self.assertIn('--collection-selection-preflight',step['command'])
            self.assertEqual(json.loads(step['command'][step['command'].index('--collection-selection-point')+1]),
                plan['steps'][0]['lease']['point'])
            self.assertEqual(step['attempt_count'],2)
            self.assertTrue(step['capture_attempts'][0]['collection_selection_preflight_retryable'])
            self.assertFalse(step['capture_attempts'][1]['collection_selection_preflight_retryable']) # same frame
            self.assertEqual(session.report['manual_clicks'],1)
            self.assertIsNotNone(session.pending_geometry)
            self.assertIsNone(session.selected_lease)
            self.assertEqual(list(session.journal.directory.glob('*.json')),[])
            self.assertTrue(session.report['ide_restored'])

    def test_following_frame_must_run_original_page_and_selected_validation(self):
        for bad_page in (False,True):
            final=copy.deepcopy(self.data['before'])
            target_id=self.pending['lease']['card']['id']
            for card in final['collection_layout']['cards']:card['selected']=card['id']==target_id
            target=next(c for c in final['collection_layout']['cards'] if c['selected'])
            for key in ('collection_layout','collection_observation','collection_selected_card'):
                final[key].update(frame_id='fresh:after-preflight',frame_sha256='f'*64)
            final['frames'][-1]['sha256']='f'*64
            final['collection_selected_card'].update(selected=True,card_id=target_id,
                bounds=target['bounds'],fields_bounds=target['fields_bounds'])
            if bad_page:final['startup_page']['page']='warehouse'
            with tempfile.TemporaryDirectory() as directory:
                clock=fake.Clock();backend=fake.FakeBackend(clock,[self.data['before'],self.packet,final])
                backend.reuse_capture=True
                session=ForegroundSession('artifacts/preflight/session.json',root=Path(directory),
                    backend=backend,now=clock.now,wait=clock.wait,fast_settle=True)
                def run():
                    with session:
                        session.perform(fake.capture(collection_layout=True))
                        plan=selection_plan(session.previous,self.pending['rule'],target_id)
                        for step in plan['steps']:session.perform(dict(step,attempts=2) if step['kind']=='capture' else step)
                if bad_page:
                    # Fake native returns the same expected-page failure code.
                    final['_exit']=1;final['page_error']='E_DIAGNOSTIC_PAGE_MISMATCH'
                    with self.assertRaisesRegex(RuntimeError,'BATCH_STEP_FAILED'):run()
                    self.assertIsNotNone(session.pending_geometry)
                else:
                    run()
                    self.assertIsNone(session.pending_geometry)
                    self.assertEqual(session.selected_lease['frame_id'],'fresh:after-preflight')
                self.assertEqual(session.report['manual_clicks'],1)
                self.assertEqual(list(session.journal.directory.glob('*.json')),[])

    def test_public_capture_step_cannot_supply_internal_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            clock=fake.Clock();backend=fake.FakeBackend(clock)
            session=ForegroundSession('artifacts/preflight/session.json',root=Path(directory),
                backend=backend,now=clock.now,wait=clock.wait,fast_settle=True)
            with self.assertRaisesRegex(RuntimeError,'PREFLIGHT_INTERNAL'):
                with session:session.perform(fake.capture(collection_layout=True,selection_preflight=True))
            self.assertFalse(any(isinstance(e,tuple) and e[0]=='capture' for e in backend.events))


if __name__=='__main__':unittest.main()
