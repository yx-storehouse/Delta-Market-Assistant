"""Receipt-only card proofs: pure packets/backends, no native/game input."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_candidate import match_selected_first_card, receipt_reference
from collection_live_session import ForegroundSession
from collection_scroll import observe_layout, selection_plan
import test_collection_live_session as fixtures
from test_collection_dynamic import dynamic_packet, rule


def before_candidate():
    return match_selected_first_card(dynamic_packet(), rule())


def receipt_packet(digest='b', *, gold=True):
    before = before_candidate()
    result = dynamic_packet(digest, card_id='receipt-row', gold=gold)
    card = copy.deepcopy(result['collection_layout']['cards'][0])
    card['bounds_basis'] = 'observed'
    proof = dict(schema='collection-selected-card-proof-v1', scope='receipt_only', proven=True,
        actions_enabled=False, unique_selection_proven=False, local_selection_conflict_checked=True,
        local_selection_check_scope='reference_horizontal_band_and_peer_column_only',
        peer_geometry_resolved=False, unresolved_regions_may_contain_selection=True, same_frame=True,
        frame_id=result['collection_observation']['frame_id'],
        frame_sha256=result['collection_observation']['frame_sha256'],
        prior_card_reference=receipt_reference(before), card=card)
    result['collection_layout'].update(complete=False, cards=[], error='E_COLLECTION_PARTIAL_SPANS_ROWS',
                                       selected_card_proof=proof)
    result['collection_selected_card'].update(scope='receipt_only', actions_enabled=False,
                                               unique_selection_proven=False)
    result['collection_observation']['regions'][-1]['scope'] = 'receipt_only'
    return result


class ReceiptProofTests(unittest.TestCase):
    def test_receipt_proof_does_not_promote_incomplete_viewport_or_global_uniqueness(self):
        result = receipt_packet()
        original = copy.deepcopy(result)
        got = match_selected_first_card(result, rule(), receipt_before=before_candidate(), receipt_only=True)
        self.assertEqual(got['price'], '230')
        self.assertEqual(got['wear'], '0.187079')
        self.assertEqual(got['geometry_mode'], 'receipt_only_observed_dynamic')
        self.assertEqual(got['scope'], 'receipt_only')
        self.assertFalse(got['unique_selection_proven'])
        self.assertFalse(got['actions_enabled'])
        self.assertFalse(got['viewport_complete'])
        self.assertEqual(result, original)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_UNPROVEN'):
            observe_layout(result)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_UNPROVEN'):
            selection_plan(result, rule(), 'receipt-row')
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_UNPROVEN'):
            match_selected_first_card(result, rule())
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_UNPROVEN'):
            match_selected_first_card(result, rule(), receipt_before=before_candidate())

    def test_no_prior_reference_means_no_receipt_only_parse(self):
        with self.assertRaisesRegex(ValueError, 'COLLECTION_RECEIPT_REFERENCE'):
            match_selected_first_card(receipt_packet(), rule(), receipt_only=True)

    def test_scope_geometry_and_prior_reference_cannot_be_fabricated(self):
        for field, value in (('proven', False), ('actions_enabled', True), ('unique_selection_proven', True),
                             ('scope', 'all_cards'), ('same_frame', False), ('frame_id', 'old'),
                             ('local_selection_conflict_checked', False), ('local_selection_check_scope', 'global'),
                             ('unresolved_regions_may_contain_selection', False), ('error', 'E_CONFLICT'),
                             ('local_conflicting_card', {'id': 'another'})):
            result = receipt_packet()
            result['collection_layout']['selected_card_proof'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                match_selected_first_card(result, rule(), receipt_before=before_candidate(), receipt_only=True)
        result = receipt_packet()
        result['collection_layout']['selected_card_proof']['prior_card_reference']['card_id'] = 'different'
        with self.assertRaisesRegex(ValueError, 'REFERENCE_BINDING'):
            match_selected_first_card(result, rule(), receipt_before=before_candidate(), receipt_only=True)

    def test_partial_or_unobserved_card_is_not_a_receipt_proof(self):
        for mutation in ('partial', 'basis', 'geometry', 'subroi', 'scope', 'overlap', 'stale'):
            result = receipt_packet()
            card = result['collection_layout']['selected_card_proof']['card']
            if mutation == 'partial': card['edges']['right'] = False
            elif mutation == 'basis': card['bounds_basis'] = 'clipped_search_region'
            elif mutation == 'geometry': card['bounds'][0] -= 9; card['bounds'][2] += 9
            elif mutation == 'subroi': card['price_bounds'][0] = 0
            elif mutation == 'scope': result['collection_observation']['regions'][-1].pop('scope')
            elif mutation == 'overlap': card['price_bounds'] = copy.deepcopy(card['condition_bounds'])
            else:
                for section in (result['collection_observation'], result['collection_layout'],
                                result['collection_layout']['selected_card_proof'], result['collection_selected_card']):
                    section['frame_id'] = 'fake:a'
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                match_selected_first_card(result, rule(), receipt_before=before_candidate(), receipt_only=True)

    def test_new_card_roi_fields_not_old_or_legacy_fields_are_required(self):
        for mutation in ('wrong_card', 'wrong_bounds', 'outside', 'duplicate', 'missing'):
            result = receipt_packet()
            region = result['collection_observation']['regions'][-1]
            if mutation == 'wrong_card': region['card_id'] = 'observed-row'
            elif mutation == 'wrong_bounds': region['bounds'] = [120, 525, 865, 44]
            elif mutation == 'outside': region['words'][-1]['y'] += 275
            elif mutation == 'duplicate': result['collection_observation']['regions'].append(copy.deepcopy(region))
            else: result['collection_observation']['regions'].pop()
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                match_selected_first_card(result, rule(), receipt_before=before_candidate(), receipt_only=True)


class ReceiptSessionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.snapshot_path = self.root / 'artifacts/trial/input/snapshot.json'
        self.snapshot_path.parent.mkdir(parents=True)
        self.snapshot_path.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        self.clock = fixtures.Clock()
        self.backend = fixtures.FakeBackend(self.clock)

    def session(self, **options):
        return ForegroundSession('artifacts/trial/run.json', root=self.root, backend=self.backend,
                                 now=self.clock.now, wait=self.clock.wait, **options)

    def collect_and_receipt(self, session):
        session.perform(fixtures.capture(collection_layout=True))
        session.perform(fixtures.collect())
        return session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))

    def test_dispatched_star_receipt_only_success_leaves_layout_incomplete(self):
        self.backend.replies = [dynamic_packet(), receipt_packet()]
        session = self.session()
        with session:
            result = self.collect_and_receipt(session)
            self.assertIsNone(session.pending)
            self.assertFalse(session.previous['collection_layout']['complete'])
            self.assertFalse(session.previous['collection_layout_passed'])
            self.assertTrue(session.previous['collection_receipt_geometry_passed'])
            self.assertTrue(session.previous['collection_receipt_passed'])
            self.assertEqual(session.previous['collection_geometry_scope'], 'receipt_only')
        self.assertTrue(session.report['passed'])
        self.assertEqual(session.report['manual_clicks'], 1)
        command = result['command']
        reference = json.loads(command[command.index('--collection-receipt-reference') + 1])
        self.assertEqual(reference, receipt_reference(before_candidate()))
        first_command = next(e[1] for e in self.backend.events if isinstance(e, tuple) and e[0] == 'capture')
        self.assertNotIn('--collection-receipt-reference', first_command)
        journal = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(journal['status'], 'confirmed')
        self.assertEqual(journal['receipt']['scope'], 'receipt_only')
        self.assertFalse(journal['receipt']['unique_selection_proven'])

    def test_receipt_only_frame_cannot_be_reused_for_new_input(self):
        self.backend.replies = [dynamic_packet(), receipt_packet()]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'FULL_LAYOUT_REOBSERVATION_REQUIRED'):
            with session:
                self.collect_and_receipt(session)
                session.perform(dict(kind='click', expected_before='skin_listings', point=[168, 1403]))
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertIsNone(session.pending)

    def test_new_complete_layout_is_required_and_preserves_confirmed_gold(self):
        self.backend.replies = [dynamic_packet(), receipt_packet(), dynamic_packet('c', gold=True)]
        session = self.session()
        with session:
            self.collect_and_receipt(session)
            session.perform(fixtures.capture(collection_layout=True))
            result = session.perform(fixtures.collect())
            self.assertEqual(result['reason'], 'already_favorited')
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertTrue(session.report['passed'])

    def test_no_pending_journal_does_not_authorize_receipt_only(self):
        self.backend.replies = [receipt_packet()]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True, attempts=1))
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertNotIn('--collection-receipt-reference', session.steps[-1]['command'])

    def test_external_reference_is_rejected_before_capture(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'RECEIPT_REFERENCE_INTERNAL'):
            with session:
                session.perform(fixtures.capture(collection_layout=True, receipt_reference=receipt_reference(before_candidate())))
        self.assertFalse(any(isinstance(e, tuple) and e[0] == 'capture' for e in self.backend.events))

    def test_prepared_or_uncertain_journal_does_not_authorize_receipt_only(self):
        for status in ('prepared', 'input_uncertain'):
            # Each case receives its own exclusive report/journal directory.
            self.root = self.root / status
            snapshot = self.root / 'artifacts/trial/input/snapshot.json'
            snapshot.parent.mkdir(parents=True)
            snapshot.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
            self.backend.replies = [dynamic_packet(), receipt_packet()]
            session = self.session()
            with self.subTest(status=status), self.assertRaisesRegex(RuntimeError, 'DISPATCH_UNPROVEN'):
                with session:
                    session.perform(fixtures.capture(collection_layout=True))
                    session.perform(fixtures.collect())
                    path = next(session.journal.directory.glob('*.json'))
                    doc = json.loads(path.read_text('utf-8'))
                    doc['status'] = status
                    path.write_text(json.dumps(doc), encoding='utf-8')
                    session.perform(fixtures.capture(expect_collection_added=True))
            self.assertEqual(len(self.backend.replies), 1)

    def test_receipt_only_keeps_price_wear_and_gold_constraints(self):
        for mutation in ('price', 'wear', 'white'):
            self.root = self.root / mutation
            snapshot = self.root / 'artifacts/trial/input/snapshot.json'
            snapshot.parent.mkdir(parents=True)
            snapshot.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
            result = receipt_packet(gold=mutation != 'white')
            if mutation == 'price': result['collection_observation']['regions'][-1]['words'][-1]['text'] = '231'
            elif mutation == 'wear':
                next(r for r in result['collection_observation']['regions'] if r['kind'] == 'selected_detail')['words'][0]['text'] = 'S(0.187080)'
            self.backend.replies = [dynamic_packet(), result]
            session = self.session()
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
                with session: self.collect_and_receipt(session)
            self.assertIsNotNone(session.pending)
            self.assertEqual(session.steps[-1]['attempt_count'], 1)
            self.assertEqual(session.report['manual_clicks'], 1)

    def test_price_image_and_preview_removed_before_every_durable_report(self):
        result = dynamic_packet()
        envelope = dict(base64='PRICE_IMAGE_BYTES_MUST_NOT_REACH_DISK', frame_id='fake:a', mime_type='image/png')
        result.update(collection_price_image=envelope, preview_png_base64='FULL_PREVIEW_BYTES_MUST_NOT_REACH_DISK')
        self.backend.replies = [result, dynamic_packet('b')]
        session = self.session()
        with session:
            first = session.perform(fixtures.capture(collection_layout=True, collection_numeric_price=True,
                                                      collection_price_image=True))
            self.assertEqual(session.last_price_image, envelope)
            self.assertNotIn('collection_price_image', session.previous)
            self.assertIn('--collection-price-image', first['command'])
            self.assertIn('--collection-numeric-price', first['command'])
            second = session.perform(fixtures.capture(collection_layout=True))
            self.assertIsNone(session.last_price_image)
            self.assertNotIn('--collection-price-image', second['command'])
            self.assertNotIn('--collection-numeric-price', second['command'])
        for path in (self.root / 'artifacts').rglob('*.json'):
            raw = path.read_text('utf-8')
            self.assertNotIn('PRICE_IMAGE_BYTES_MUST_NOT_REACH_DISK', raw)
            self.assertNotIn('FULL_PREVIEW_BYTES_MUST_NOT_REACH_DISK', raw)

    def test_numeric_price_session_option_is_explicit_and_dynamic_only(self):
        self.backend.replies = [fixtures.packet(), dynamic_packet(), receipt_packet()]
        session = self.session(numeric_price=True)
        with session:
            plain = session.perform(fixtures.capture())
            dynamic = session.perform(fixtures.capture(collection_layout=True))
            session.perform(fixtures.collect())
            receipt = session.perform(fixtures.capture(expect_collection_added=True))
        self.assertNotIn('--collection-numeric-price', plain['command'])
        self.assertIn('--collection-numeric-price', dynamic['command'])
        self.assertIn('--collection-numeric-price', receipt['command'])
        self.assertNotIn('--collection-price-image', dynamic['command'])
        self.assertTrue(session.report['numeric_price_enabled'])

    def test_numeric_option_rejects_truthy_non_bool_before_backend_entry(self):
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_NUMERIC_PRICE_OPTION'):
            self.session(numeric_price='true')
        self.assertEqual(self.backend.events, [])


if __name__ == '__main__':
    unittest.main()
