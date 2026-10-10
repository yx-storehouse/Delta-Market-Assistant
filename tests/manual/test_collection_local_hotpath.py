"""Local listing continuation protocol tests; no Win32, model, or capture runs."""
import json
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from collection_live_session import WindowsBackend
from run_collection_observed import MemoryReviewBackend


class LocalHotpathProtocolTests(unittest.TestCase):
    @staticmethod
    def init_stub(backend, root, *, persistent_capture=False):
        backend._metadata = {}
        backend.persistent_stub = persistent_capture

    def create(self, **options):
        with patch.object(WindowsBackend, '__init__', self.init_stub):
            return MemoryReviewBackend(Path('synthetic-project'), **options)

    def capture(self, command, *, local_hotpath=True, fast_capture=True,
                reuse_capture=True, packet=None):
        backend = self.create(local_hotpath=local_hotpath, fast_capture=fast_capture,
                              reuse_capture=reuse_capture)
        if packet is None:
            packet = dict(capture_passed=False, error='SYNTHETIC_STOP', image_file_writes=0)
        reply = subprocess.CompletedProcess(command, 1, json.dumps(packet).encode(), b'')
        with patch.object(WindowsBackend, 'capture', return_value=reply) as base:
            result = backend.capture(command, 10)
        return result, base.call_args.args[0], backend

    @staticmethod
    def command(page='skin_listings', action='selection'):
        command=['app','--live-capture-check','--ocr','--collection-observation',
                 '--collection-layout','--expected-page',page]
        if action=='selection':command.append('--collection-selection-preflight')
        elif action=='receipt':command += ['--collection-receipt-reference','{"frame_id":"synthetic:old"}']
        return command

    def test_opt_in_requires_persistent_capture_and_capture_resource_reuse(self):
        for fast in (False,True):
            for reuse in (False,True):
                for local in (False,True):
                    with self.subTest(fast=fast,reuse=reuse,local=local):
                        backend=self.create(fast_capture=fast,reuse_capture=reuse,local_hotpath=local)
                        self.assertEqual(backend.local_hotpath,fast and reuse and local)
                        self.assertEqual(backend._metadata['calibrated_listing_hotpath_enabled'],fast and reuse and local)

    def test_local_hotpath_option_requires_actual_bool(self):
        for value in (None,0,1,'true',[],{}):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError,'BOOLEAN_OPTION'):
                    self.create(fast_capture=True,local_hotpath=value)

    def test_continuous_motion_selected_only_for_explicit_fast_actions(self):
        for fast in (False,True):
            for actions in (False,True):
                backend=self.create(fast_capture=fast,fast_actions=actions)
                expected='collection_continuous' if fast and actions else 'standard'
                self.assertEqual(backend.collection_motion_profile,expected)
                self.assertEqual(backend._metadata['collection_action_motion_profile'],expected)

    def test_only_selection_or_pending_receipt_on_listing_gets_option(self):
        for action in ('selection','receipt'):
            command=self.command(action=action);before=list(command)
            result,sent,_=self.capture(command)
            self.assertEqual(sent.count('--collection-local-hotpath'),1)
            self.assertEqual(command,before)
            self.assertEqual(json.loads(result.stdout)['diagnostic_actual_command'],sent)
            self.assertEqual(result.returncode,1)
        _,sent,_=self.capture(self.command(action='ordinary'))
        self.assertNotIn('--collection-local-hotpath',sent)

    def test_other_pages_and_missing_expected_page_do_not_get_listing_option(self):
        for page in ('skin_home','watchlist_listings','catalog_filter','lobby',''):
            for action in ('selection','receipt'):
                with self.subTest(page=page,action=action):
                    _,sent,_=self.capture(self.command(page=page,action=action))
                    self.assertNotIn('--collection-local-hotpath',sent)
        command=self.command();i=command.index('--expected-page');del command[i:i+2]
        _,sent,_=self.capture(command)
        self.assertNotIn('--collection-local-hotpath',sent)

    def test_explicit_opt_out_retains_existing_capture_and_recognition_options(self):
        for action in ('selection','receipt'):
            _,sent,backend=self.capture(self.command(action=action),local_hotpath=False)
            self.assertNotIn('--collection-local-hotpath',sent)
            self.assertIn('--reuse-capture-resources',sent)
            self.assertIn('--collection-title-image',sent)
            self.assertIn('--collection-price-image',sent)
            self.assertFalse(backend._metadata['calibrated_listing_hotpath_enabled'])

    def test_local_option_never_duplicated_and_full_preview_stays_disabled(self):
        command=self.command()+['--collection-local-hotpath']
        _,sent,_=self.capture(command)
        self.assertEqual(sent.count('--collection-local-hotpath'),1)
        self.assertNotIn('--preview-stdout',sent)

    def test_wrapper_does_not_upgrade_context_packet_to_fake_full_ocr(self):
        context=dict(capture_passed=True,full_frame_ocr_performed=False,
            ocr=dict(coverage='calibrated_listing_fields',full_frame_ocr_performed=False),
            collection_page_context=dict(used=True,old_candidate_fields_reused=False,actions_enabled=False),
            image_file_writes=0)
        result,_,_=self.capture(self.command(),packet=context)
        actual=json.loads(result.stdout)
        self.assertFalse(actual['full_frame_ocr_performed'])
        self.assertEqual(actual['ocr'],context['ocr'])
        self.assertEqual(actual['collection_page_context'],context['collection_page_context'])
        for name in ('collection_selected_card','collection_observation','collection_layout'):
            self.assertNotIn(name,actual)

    def test_miss_and_full_frame_fallback_metadata_are_not_relabelled(self):
        packet=dict(capture_passed=True,full_frame_ocr_performed=True,
            ocr=dict(coverage='full_client',full_frame_ocr_performed=True),
            collection_page_context=dict(used=False,reason='guard_pixels_changed'),image_file_writes=0)
        result,_,_=self.capture(self.command(),packet=packet)
        actual=json.loads(result.stdout)
        self.assertTrue(actual['full_frame_ocr_performed'])
        self.assertEqual(actual['ocr'],packet['ocr'])
        self.assertEqual(actual['collection_page_context'],packet['collection_page_context'])


if __name__=='__main__':unittest.main()
