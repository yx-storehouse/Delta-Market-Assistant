"""Pixel receipts, pipelined next selection and async evidence; fake backend only."""
import copy
import json
from pathlib import Path
import tempfile
import time
import unittest

import test_collection_live_session as fixtures
from test_collection_dynamic import dynamic_packet, rule
from collection_candidate import receipt_reference, validate_pixel_receipt
from collection_live_session import AsyncJsonWriter, ForegroundSession
from collection_scroll import layouts_equivalent, observe_layout, selection_plan


def two_card_packet(digest, *, selected='A', gold=False, shift_b=0):
    """Card A (left) and card B (right); fields words belong to the selected card."""
    name = selected or 'A'
    x = 119 if name == 'A' else 997
    value = dynamic_packet(digest, x=x, card_id='card-' + name, selected=selected is not None, gold=gold)
    layout = value['collection_layout']
    first = layout['cards'][0]
    other_x = 997 if name == 'A' else 119
    other = copy.deepcopy(first)
    other.update(id='card-' + ('B' if name == 'A' else 'A'), selected=False,
                 bounds=[other_x, first['bounds'][1] + (shift_b if name == 'A' else 0), 865, 266])
    other['fields_bounds'] = [other_x, other['bounds'][1] + 222, 865, 44]
    other['condition_bounds'] = [other_x, other['bounds'][1] + 222, 100, 44]
    other['price_bounds'] = [other_x + 730, other['bounds'][1] + 222, 135, 44]
    layout['cards'].append(other)
    layout['listing_viewport'] = [110, 303, 1760, 947]
    return value


def pixel_reply(basis, digest, *, gold=True, ready=None, shift_b=0, move_a=0):
    """Native pixel receipt packet derived from the selection packet's layout."""
    def build(command):
        reference = json.loads(command[command.index('--collection-receipt-reference') + 1])
        frame_id, sha = 'fake:' + digest, digest * 64
        layout = copy.deepcopy(basis['collection_layout'])
        layout.update(frame_id=frame_id, frame_sha256=sha)
        for card in layout['cards']:
            delta = move_a if card['selected'] else shift_b
            for key in ('bounds', 'fields_bounds', 'condition_bounds', 'price_bounds'):
                card[key][1] += delta
        card = next(c for c in layout['cards'] if c['selected'])
        warm, bright = (.3, .005) if gold else (0, .13)
        return dict(capture_passed=True, ocr_passed=False, recognition_performed=False,
            focus_activation_requests=0, focus_restore_requests=0, target_foreground_retained=True,
            frames=[dict(width=2560, height=1440, sha256=sha, source_age_ms=10, source_uncertainty_ms=1)],
            collection_layout=layout,
            collection_observation=dict(frame_id=frame_id, frame_sha256=sha, same_frame=True, regions=[]),
            collection_selected_card=dict(selected=True, same_frame=True, frame_id=frame_id, frame_sha256=sha,
                card_id=card['id'], bounds=card['bounds'], fields_bounds=card['fields_bounds'],
                favorite_warm_fraction=warm, favorite_bright_fraction=bright, actions_enabled=False),
            collection_pixel_receipt=dict(schema='collection-pixel-receipt-v1', ready=gold if ready is None else ready,
                layout_complete=True, frame_id=frame_id, frame_sha256=sha, reference=reference,
                ocr_performed=False, actions_enabled=False),
            startup_page=dict(page='unobserved', overlay='unobserved'), ocr=dict(provider='not_invoked'))
    return build


class CallableBackend(fixtures.FakeBackend):
    pixel_receipt = True
    fast_collection_motion = False
    def capture(self, command, timeout):
        if self.replies and callable(self.replies[0]):
            self.replies[0] = self.replies[0](list(command))
        return super().capture(command, timeout)


class PixelReceiptValidationTests(unittest.TestCase):
    def setUp(self):
        self.basis = two_card_packet('b')
        from collection_candidate import match_selected_first_card
        self.before = match_selected_first_card(self.basis, rule())
        self.before['favorite_before'] = dict(favorite_warm_fraction=0, favorite_bright_fraction=.13, selected=True)

    def reply(self, **options):
        command = ['x', '--collection-receipt-reference', json.dumps(receipt_reference(self.before))]
        return pixel_reply(self.basis, 'c', **options)(command)

    def test_gold_same_card_and_unchanged_list(self):
        validated = validate_pixel_receipt(self.reply(), self.before, self.basis)
        self.assertTrue(validated['layout_unchanged'])
        self.assertEqual(validated['scope'], 'visible_layout')
        self.assertFalse(validated['text_reread'])

    def test_other_card_moved_confirms_star_but_not_list(self):
        validated = validate_pixel_receipt(self.reply(shift_b=6), self.before, self.basis)
        self.assertFalse(validated['layout_unchanged'])

    def test_white_star_not_ready_moved_card_and_reused_frame_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'NOT_READY'):
            validate_pixel_receipt(self.reply(gold=False), self.before, self.basis)
        packet = self.reply(ready=True, gold=False)
        with self.assertRaisesRegex(ValueError, 'STAR_NOT_GOLD'):
            validate_pixel_receipt(packet, self.before, self.basis)
        with self.assertRaisesRegex(ValueError, 'GEOMETRY_CONFLICT'):
            validate_pixel_receipt(self.reply(move_a=6), self.before, self.basis)
        reused = self.reply()
        for key in ('collection_layout', 'collection_observation', 'collection_selected_card', 'collection_pixel_receipt'):
            reused[key]['frame_id'] = self.before['source_frame_id']
        with self.assertRaisesRegex(ValueError, 'REOBSERVATION_REQUIRED'):
            validate_pixel_receipt(reused, self.before, self.basis)

    def test_reference_binding_and_transition_are_required(self):
        packet = self.reply()
        packet['collection_pixel_receipt']['reference']['card_id'] = 'other'
        with self.assertRaisesRegex(ValueError, 'REFERENCE_BINDING'):
            validate_pixel_receipt(packet, self.before, self.basis)
        before = copy.deepcopy(self.before)
        before['favorite_before']['favorite_warm_fraction'] = .2
        with self.assertRaisesRegex(ValueError, 'TRANSITION_UNPROVEN'):
            validate_pixel_receipt(self.reply(), before, self.basis)

    def test_ambiguous_selection_is_rejected(self):
        packet = self.reply()
        for card in packet['collection_layout']['cards']:
            card['selected'] = True
        with self.assertRaisesRegex(ValueError, 'SELECTION'):
            validate_pixel_receipt(packet, self.before, self.basis)

    def test_layouts_equivalent_needs_same_members_and_scrollbar(self):
        a = observe_layout(self.basis)
        b = observe_layout(two_card_packet('d'))
        self.assertTrue(layouts_equivalent(a, b))
        self.assertFalse(layouts_equivalent(a, a))
        self.assertFalse(layouts_equivalent(a, observe_layout(two_card_packet('d', shift_b=4))))
        moved = two_card_packet('d')
        moved['collection_layout']['scrollbar']['thumb_bounds'][1] += 2
        self.assertFalse(layouts_equivalent(a, observe_layout(moved)))
        self.assertFalse(layouts_equivalent(a, observe_layout(two_card_packet('d', selected='B'))))


class RecordedRun01LayoutTests(unittest.TestCase):
    """Real S11 run01 numbers: only clipped tail members jittered."""
    def packet(self, layout):
        layout = copy.deepcopy(layout)
        return dict(frames=[dict(width=2560, height=1440, sha256=layout['frame_sha256'])],
                    collection_observation=dict(frame_id=layout['frame_id'], frame_sha256=layout['frame_sha256'],
                                                same_frame=True),
                    collection_layout=dict(layout, schema='collection-layout-v1', detector='visible_edges',
                                           same_frame=True))

    def setUp(self):
        path = Path(__file__).resolve().parents[1] / 'fixtures/collection_pipeline_run01_layouts.json'
        self.recorded = json.loads(path.read_text('utf-8'))

    def layout(self, name, mutate=None):
        packet = self.packet(self.recorded[name])
        if mutate:
            mutate(packet['collection_layout'])
        return observe_layout(packet, require_page=False)

    def test_tail_jitter_and_resting_hover_are_the_same_list(self):
        basis = self.layout('basis')
        self.assertTrue(layouts_equivalent(basis, self.layout('pixel_receipt')))
        self.assertTrue(layouts_equivalent(basis, self.layout('recheck')))

    def test_list_growing_below_unmoved_view_is_the_same_list(self):
        # run02: thumb 62 -> 47 px at the top while every complete card stayed.
        def grow(layout):
            layout['scrollbar']['thumb_bounds'][3] = 47
        def one_pixel(layout):
            layout['scrollbar']['thumb_bounds'][1] += 1
        def centreline(layout):
            # run04: the 1 px scrollbar centreline measured at x 1877, then 1878.
            for key in ('track_bounds', 'thumb_bounds'):
                layout['scrollbar'][key][0] += 1
        def two_columns(layout):
            for key in ('track_bounds', 'thumb_bounds'):
                layout['scrollbar'][key][0] += 2
        basis = self.layout('basis')
        self.assertTrue(layouts_equivalent(basis, self.layout('pixel_receipt', grow)))
        self.assertTrue(layouts_equivalent(basis, self.layout('pixel_receipt', one_pixel)))
        self.assertTrue(layouts_equivalent(basis, self.layout('pixel_receipt', centreline)))
        self.assertFalse(layouts_equivalent(basis, self.layout('pixel_receipt', two_columns)))

    def test_moved_complete_card_selection_or_scrollbar_is_not_the_same_list(self):
        basis = self.layout('basis')
        def move(layout):
            for key in ('bounds', 'fields_bounds', 'condition_bounds', 'price_bounds'):
                layout['cards'][1][key][1] += 6
        def reselect(layout):
            layout['cards'][0]['selected'] = False
            layout['cards'][1]['selected'] = True
        def scroll(layout):
            layout['scrollbar']['thumb_bounds'][1] += 2
        def shorter_track(layout):
            layout['scrollbar']['track_bounds'][3] -= 5
        def drop(layout):
            layout['cards'][2]['edges']['bottom'] = False
        for mutate in (move, reselect, scroll, shorter_track, drop):
            with self.subTest(mutate=mutate.__name__):
                self.assertFalse(layouts_equivalent(basis, self.layout('pixel_receipt', mutate)))


class PipelinedSessionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.snapshot_path = self.root / 'artifacts/trial/input/snapshot.json'
        self.snapshot_path.parent.mkdir(parents=True)
        self.snapshot_path.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        self.clock = fixtures.Clock()
        self.backend = CallableBackend(self.clock)

    def session(self, **options):
        options.setdefault('pixel_receipts', True)
        options.setdefault('pipeline_receipts', True)
        return ForegroundSession('artifacts/trial/run.json', root=self.root, backend=self.backend,
                                 now=self.clock.now, wait=self.clock.wait, fast_settle=True, **options)

    def collect_a(self, session, receipt):
        """Observe A+B, select A, collect A; return the selection packet."""
        listing = two_card_packet('a', selected=None)
        for card in listing['collection_layout']['cards']:
            card['selected'] = False
        listing['collection_selected_card']['selected'] = False
        selected = two_card_packet('b')
        self.backend.replies = [listing, selected, receipt(selected)]
        session.perform(fixtures.capture(collection_layout=True))
        plan = selection_plan(session.previous, rule(), 'card-A')
        for step in plan['steps']:
            session.perform(step)
        return selected

    def journal_records(self):
        directory = self.root / 'artifacts/m2_savedvalue_collection/journal'
        return [json.loads(p.read_text('utf-8')) for p in directory.glob('*.json')]

    def test_next_selection_input_waits_for_pixel_receipt_and_listener(self):
        confirmed = []
        session = self.session()
        session.receipt_listener = confirmed.append
        with session:
            selected = self.collect_a(session, lambda basis: pixel_reply(basis, 'c'))
            action = session.perform(fixtures.collect())
            self.assertEqual(action['receipt_mode'], 'pipelined_pixel')
            self.assertIsNotNone(session.pending)
            self.assertEqual(session.previous['collection_observation']['frame_id'], 'fake:b')
            self.assertEqual(confirmed, [])
            lease_plan = selection_plan(session.previous, rule(), 'card-B')
            session.perform(lease_plan['steps'][0])
            self.assertIsNone(session.pending)
            self.assertEqual(len(confirmed), 1)
            self.assertEqual(confirmed[0]['mode'], 'pixel')
        events = [e for e in self.backend.events if isinstance(e, tuple)]
        pixel_index = next(i for i, e in enumerate(events) if e[0] == 'capture' and '--collection-pixel-receipt' in e[1])
        clicks = [i for i, e in enumerate(events) if e[0] == 'click']
        self.assertEqual(len(clicks), 3)
        self.assertLess(clicks[1], pixel_index)
        self.assertLess(pixel_index, clicks[2])
        records = self.journal_records()
        self.assertEqual([r['status'] for r in records], ['confirmed'])
        self.assertEqual(records[0]['receipt_kind'], 'pixel_same_card_white_to_gold')
        self.assertFalse(records[0]['receipt']['text_reread'])
        self.assertEqual(records[0]['receipt']['price'], records[0]['candidate']['price'])
        settlement = session.steps[-1]['receipt_settlements'][0]
        self.assertEqual(settlement['outcome']['mode'], 'pixel')

    def test_white_star_falls_back_to_full_receipt_before_next_input(self):
        session = self.session()
        with session:
            self.collect_a(session, lambda basis: pixel_reply(basis, 'c', gold=False))
            session.perform(fixtures.collect())
            full = two_card_packet('d', gold=True)
            full['collection_selected_card'].update(favorite_warm_fraction=.3, favorite_bright_fraction=.005)
            self.backend.replies = [full]
            session.perform(selection_plan(session.previous, rule(), 'card-B')['steps'][0])
            self.assertIsNone(session.pending)
        records = self.journal_records()
        self.assertEqual(records[0]['status'], 'confirmed')
        self.assertIn(records[0]['receipt_kind'], ('toast_and_gold', 'same_item_white_to_gold'))
        self.assertEqual(session.steps[-1]['receipt_settlements'][0]['outcome']['mode'], 'full_fallback')

    def test_changed_list_confirms_star_but_blocks_planned_input(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_RECEIPT_LAYOUT_CHANGED'):
            with session:
                self.collect_a(session, lambda basis: pixel_reply(basis, 'c', shift_b=6))
                session.perform(fixtures.collect())
                session.perform(selection_plan(session.previous, rule(), 'card-B')['steps'][0])
        clicks = [e for e in self.backend.events if isinstance(e, tuple) and e[0] == 'click']
        self.assertEqual(len(clicks), 2)
        self.assertEqual(self.journal_records()[0]['status'], 'confirmed')

    def test_unproven_list_in_moving_pointer_frames_is_reread_at_rest(self):
        session = self.session()
        with session:
            self.collect_a(session, lambda basis: pixel_reply(basis, 'c', shift_b=6))
            session.perform(fixtures.collect())
            self.backend.replies = [two_card_packet('e', gold=True)]
            session.perform(selection_plan(session.previous, rule(), 'card-B')['steps'][0])
            self.assertIsNone(session.pending)
        clicks = [e for e in self.backend.events if isinstance(e, tuple) and e[0] == 'click']
        self.assertEqual(len(clicks), 3)
        self.assertEqual(session.steps[-1]['receipt_settlements'][0]['outcome']['layout_unchanged'], False)
        self.assertIn(dict(mode='layout_recheck', layout_unchanged=True), session.receipt_settlements)

    def test_navigation_and_segment_reset_settle_first(self):
        session = self.session()
        with session:
            self.collect_a(session, lambda basis: pixel_reply(basis, 'c'))
            session.perform(fixtures.collect())
            session.reset_segment()
            self.assertIsNone(session.pending)
        self.assertTrue(session.report['passed'])

    def test_finish_settles_in_flight_receipt(self):
        session = self.session()
        with session:
            self.collect_a(session, lambda basis: pixel_reply(basis, 'c'))
            session.perform(fixtures.collect())
        self.assertIsNone(session.pending)
        self.assertTrue(session.report['passed'])
        self.assertEqual(self.journal_records()[0]['status'], 'confirmed')

    def test_synchronous_pixel_mode_confirms_inside_collect(self):
        session = self.session(pipeline_receipts=False)
        with session:
            self.collect_a(session, lambda basis: pixel_reply(basis, 'c'))
            action = session.perform(fixtures.collect())
            self.assertEqual(action['receipt_mode'], 'pixel')
            self.assertIsNone(session.pending)
            self.assertTrue(action['receipt_outcome']['layout_unchanged'])
        self.assertTrue(session.report['passed'])

    def test_pipeline_requires_pixel_receipts_and_fast_settle(self):
        with self.assertRaisesRegex(RuntimeError, 'PIPELINE_REQUIRES_PIXEL'):
            ForegroundSession('artifacts/trial/x.json', root=self.root, backend=self.backend,
                              pipeline_receipts=True)
        with self.assertRaisesRegex(RuntimeError, 'PIXEL_RECEIPT_REQUIRES_FAST_SETTLE'):
            ForegroundSession('artifacts/trial/y.json', root=self.root, backend=self.backend, pixel_receipts=True)

    def test_async_reports_are_complete_after_finish(self):
        session = self.session(async_reports=True)
        with session:
            self.collect_a(session, lambda basis: pixel_reply(basis, 'c'))
            session.perform(fixtures.collect())
            session.flush_reports()
            first = json.loads((session.step_directory / '000001.json').read_text('utf-8'))
            self.assertEqual(first['kind'], 'capture')
        report = json.loads(session.report_path.read_text('utf-8'))
        self.assertTrue(report['passed'])
        self.assertEqual(report['report_writer']['error'], None)
        self.assertEqual(len(report['steps']), len(session.steps))


class PipelinedTrialTests(unittest.TestCase):
    """Runner counts session-confirmed receipts; coverage waits for them."""
    def setUp(self):
        from unittest.mock import patch
        validation = patch('collection_scroll.validate_scroll_calibration', side_effect=copy.deepcopy, create=True)
        validation.start()
        self.addCleanup(validation.stop)

    def make_session(self, candidates):
        from test_collection_trial import FakeSession

        class PipelinedSession(FakeSession):
            receipt_listener = None
            def settle_receipts(inner):
                if inner.pending is None:
                    return None
                key, candidate = inner.pending_key, inner.pending
                inner.pending = None
                inner.receipt_listener(dict(mode='pixel', key=key, layout_unchanged=True,
                                            frame_sha256='f' * 64, candidate=candidate))
                return dict(mode='pixel', layout_unchanged=True)
            def reset_segment(inner):
                inner.settle_receipts()
                super().reset_segment()
            def perform(inner, step):
                if step['kind'] in ('select_visible_card', 'scroll_visible_list', 'capture'):
                    inner.settle_receipts()
                result = super().perform(step)
                if step['kind'] == 'collect_selected' and result.get('collection_attempt'):
                    inner.serial += 1
                    inner.pending_key = 'k%d' % inner.serial
                    inner.previous = copy.deepcopy(listing_for(inner))
                    result.update(receipt_mode='pipelined_pixel', collection_attempt_key=inner.pending_key)
                return result

        def listing_for(inner):
            from test_collection_trial import listing
            return listing(inner.index, inner.serial, inner.window)
        return PipelinedSession(candidates)

    def test_pipelined_receipts_are_counted_and_confirmed_before_window_coverage(self):
        from unittest.mock import patch
        from test_collection_trial import TestTrial, row, snapshot
        session = self.make_session([{'price': 230}] * 6 + [{'price': 601, 'eligible': False}])
        events = []
        trial = TestTrial(session, snapshot(), 'artifacts/mock/input.json', emit=events.append,
                          scroll_profile=dict(schema='mock', delta=-480))
        seen = []
        def coverage(packet, rule, processed):
            seen.append([entry['disposition'] for entry in processed])
            return dict(schema='mock-coverage')
        def plan(packet, rule, records, **options):
            return dict(steps=[dict(kind='scroll_visible_list', lease=dict(delta=options['delta'])),
                               dict(kind='capture', expected_page='skin_listings', collection_layout=True)])
        with patch('collection_scroll.build_scroll_coverage', side_effect=coverage, create=True), \
             patch('run_collection_trial.scroll_plan', side_effect=plan):
            trial.scan_row(row())
        self.assertEqual(trial.summary['confirmed_new'], 6)
        self.assertTrue(seen and all(d == 'favorite_confirmed' for window in seen for d in window))
        self.assertEqual(sum(e['event'] == 'favorite_confirmed' for e in events), 6)
        self.assertFalse(any(s.get('expect_collection_added') for s in session.steps))


class ScrollReadinessRetryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        path = self.root / 'artifacts/trial/input/snapshot.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        self.clock = fixtures.Clock()
        self.backend = fixtures.FakeBackend(self.clock)
        self.backend.ready_stream = self.backend.reuse_capture = True

    def run_scroll(self, unstable):
        from collection_scroll import scroll_plan
        not_stable = dict(capture_passed=False, error='E_COLLECTION_SCROLL_NOT_STABLE', target_foreground_retained=True,
                          focus_activation_requests=0, focus_restore_requests=0, frames=[], _exit=1)
        self.backend.replies = ([dynamic_packet(gold=True)] + [copy.deepcopy(not_stable) for _ in range(unstable)]
                                + [dynamic_packet('b', gold=True, thumb_y=370)])
        session = ForegroundSession('artifacts/trial/session.json', root=self.root, backend=self.backend,
                                    now=self.clock.now, wait=self.clock.wait, fast_settle=True)
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            for step in scroll_plan(session.previous, rule(), [], delta=-120)['steps']:
                last = session.perform(step)
        return session, last

    def test_unfinished_wheel_stability_is_read_again_without_another_wheel(self):
        session, last = self.run_scroll(1)
        self.assertTrue(session.report['passed'])
        self.assertEqual(last['attempt_count'], 2)
        self.assertTrue(last['attempts'][0]['collection_scroll_readiness_retryable'])
        self.assertEqual(sum(1 for e in self.backend.events if isinstance(e, tuple) and e[0] == 'scroll'), 1)

    def test_stability_retries_stay_bounded(self):
        from unittest.mock import ANY
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            self.run_scroll(3)
        self.assertEqual(sum(1 for e in self.backend.events if isinstance(e, tuple) and e[0] == 'scroll'), 1)


class BackendProtocolTests(unittest.TestCase):
    def create(self, **options):
        from unittest.mock import patch
        from collection_live_session import WindowsBackend
        from run_collection_observed import MemoryReviewBackend
        def init_stub(backend, root, *, persistent_capture=False):
            backend._metadata = {}
        with patch.object(WindowsBackend, '__init__', init_stub):
            return MemoryReviewBackend(Path('synthetic-project'), fast_capture=True, **options)

    def send(self, backend, command, packet):
        import subprocess
        from unittest.mock import patch
        from collection_live_session import WindowsBackend
        reply = subprocess.CompletedProcess(command, 0, json.dumps(packet).encode(), b'')
        with patch.object(WindowsBackend, 'capture', return_value=reply) as base:
            result = backend.capture(command, 10)
        return json.loads(result.stdout), base.call_args.args[0]

    def test_pixel_receipt_passes_through_without_crops_or_local_ocr(self):
        backend = self.create()
        self.assertTrue(backend.pixel_receipt)
        class Exploding:
            def apply(self, packet): raise AssertionError('no OCR for pixel receipts')
            apply_catalog = apply
        backend.local_title = Exploding()
        command = ['app', '--live-capture-check', '--frames', '1', '--collection-pixel-receipt',
                   '--collection-receipt-reference', '{}', '--reuse-capture-resources', '--collection-ready-stream']
        packet, sent = self.send(backend, command, dict(capture_passed=True))
        self.assertEqual(sent, command)
        self.assertEqual(packet['diagnostic_actual_command'], command)
        self.assertFalse(self.create(ready_stream=False).pixel_receipt)

    def test_post_scroll_listing_capture_gets_calibrated_hotpath(self):
        backend = self.create()
        command = ['app', '--live-capture-check', '--ocr', '--collection-observation', '--collection-layout',
                   '--expected-page', 'skin_listings', '--collection-scroll-readiness']
        _, sent = self.send(backend, command, dict(capture_passed=False))
        self.assertEqual(sent.count('--collection-local-hotpath'), 1)

    def test_parallel_price_runs_beside_title_and_surfaces_its_error(self):
        import threading
        backend = self.create()
        started = threading.Event()
        order = []
        class Title:
            def apply(self, packet):
                self.ok = started.wait(2)
                order.append('title')
            def apply_catalog(self, packet):
                order.append('catalog')
        class Price:
            def __init__(self, error=None): self.error = error
            def apply(self, packet):
                started.set(); order.append('price')
                if self.error: raise self.error
        backend.local_title, backend.local_price, backend.parallel_local_price = Title(), Price(), True
        command = ['app', '--live-capture-check', '--ocr', '--collection-observation', '--collection-layout',
                   '--collection-numeric-price']
        packet, _ = self.send(backend, command, dict(capture_passed=True))
        self.assertTrue(backend.local_title.ok)
        self.assertEqual(set(order), {'title', 'catalog', 'price'})
        self.assertTrue(packet['capture_passed'])
        started.clear(); order.clear()
        backend.local_price = Price(ValueError('LOCAL_PRICE_CONFIDENCE'))
        packet, _ = self.send(backend, command, dict(capture_passed=True))
        self.assertFalse(packet['capture_passed'])
        self.assertEqual(packet['local_title_error'], 'LOCAL_PRICE_CONFIDENCE')


class UnsentReservationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        path = self.root / 'artifacts/trial/input/snapshot.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        self.clock = fixtures.Clock()

    def run_interrupted(self, *, dispatched):
        class Interrupting(CallableBackend):
            last_dispatch = None
            def click(inner, point, before_dispatch=None):
                inner.last_dispatch = None
                if inner.interrupt_next_star and point == [2339, 320]:
                    if dispatched:
                        inner.last_dispatch = dict(api='SendInput', returned_events=1)
                    raise RuntimeError('CURSOR_INTERFERENCE')
                return super().click(point, before_dispatch)
        self.backend = Interrupting(self.clock)
        self.backend.interrupt_next_star = True
        helper = PipelinedSessionTests()
        helper.root, helper.backend, helper.clock = self.root, self.backend, self.clock
        session = helper.session()
        with self.assertRaisesRegex(RuntimeError, 'CURSOR_INTERFERENCE'):
            with session:
                helper.collect_a(session, lambda basis: pixel_reply(basis, 'c'))
                session.perform(fixtures.collect())
        return session

    def test_star_motion_interrupted_before_sendinput_releases_reservation(self):
        session = self.run_interrupted(dispatched=False)
        self.assertIsNone(session.pending)
        journal = self.root / 'artifacts/m2_savedvalue_collection/journal'
        self.assertEqual(list(journal.glob('*.json')), [])
        archived = list((self.root / 'artifacts/m2_savedvalue_collection/released_unsent').glob('*.json'))
        self.assertEqual(len(archived), 1)
        record = json.loads(archived[0].read_text('utf-8'))
        self.assertEqual(record['original_record']['status'], 'prepared')
        self.assertEqual(record['evidence']['reason'], 'input_not_dispatched')
        self.assertEqual(record['input_actions'], 0)

    def test_any_sendinput_attempt_keeps_the_reservation_for_reconciliation(self):
        session = self.run_interrupted(dispatched=True)
        self.assertIsNotNone(session.pending)
        journal = self.root / 'artifacts/m2_savedvalue_collection/journal'
        self.assertEqual([json.loads(p.read_text('utf-8'))['status'] for p in journal.glob('*.json')], ['prepared'])

    def test_release_refuses_dispatched_or_foreign_records(self):
        from collection_journal import CollectionJournal
        from test_collection_journal_history import candidate
        journal = CollectionJournal(self.root / 'artifacts/j')
        key = journal.prepare(candidate('x'), {}, allow_confirmed_history=True)
        journal.update(key, 'dispatched', sent=2)
        with self.assertRaisesRegex(ValueError, 'REQUIRES_UNSENT_PREPARED'):
            journal.release_unsent(key, archive_directory=self.root / 'artifacts/archive', evidence={})
        with self.assertRaisesRegex(ValueError, 'RELEASE_ARCHIVE'):
            journal.release_unsent(key, archive_directory=self.root / 'artifacts/j', evidence={})


class AsyncWriterTests(unittest.TestCase):
    def test_latest_bytes_win_and_errors_surface(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'a.json'
            writer = AsyncJsonWriter()
            for index in range(20):
                writer.submit(path, json.dumps(dict(index=index)).encode())
            writer.flush()
            self.assertEqual(json.loads(path.read_text()), dict(index=19))
            writer.submit(Path(temporary) / 'missing' / 'b.json', b'{}')
            with self.assertRaisesRegex(RuntimeError, 'COLLECTION_REPORT_WRITE'):
                writer.flush()
            with self.assertRaisesRegex(RuntimeError, 'COLLECTION_REPORT_WRITE'):
                writer.submit(path, b'{}')


if __name__ == '__main__':
    unittest.main()
