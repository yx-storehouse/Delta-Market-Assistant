"""Runner-only batch-scroll wiring; calibration geometry is tested separately.

These fixtures do not establish a measured wheel distance, invoke a model,
capture a game, or send OS input. Geometry/provider functions are mocked only
where this suite verifies orchestration ownership and propagation.
"""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_collection_trial as runner
from run_collection_trial import CollectionTrial, load_scroll_profile
from test_collection_trial import FakeSession, TestTrial, listing, row, snapshot
from test_collection_scroll import card as geometry_card
from collection_scroll import observe_layout, validate_scroll_lease, rebind_after_scroll


def fixture_profile(delta=-480):
    return dict(schema='mock-calibration-for-runner-contract-only', delta=delta,
                evidence=['inert-fixture-not-live-measurement'])


def fixture_plan(packet, rule, records, **options):
    return dict(steps=[dict(kind='scroll_visible_list', lease=dict(delta=options['delta'])),
                       dict(kind='capture', expected_page='skin_listings', collection_layout=True)])


class BatchRunnerTests(unittest.TestCase):
    def setUp(self):
        self.validation = patch('collection_scroll.validate_scroll_calibration',
                                side_effect=copy.deepcopy, create=True)
        self.validator = self.validation.start()
        self.addCleanup(self.validation.stop)

    def trial(self, session=None, profile=None, events=None):
        return TestTrial(session or FakeSession(), snapshot(), 'artifacts/mock/input.json',
                         emit=(events.append if events is not None else lambda event: None),
                         scroll_profile=profile if profile is not None else fixture_profile())

    def test_explicit_profile_is_frozen_and_has_hashed_summary_provenance(self):
        profile = fixture_profile()
        source = dict(path='artifacts/mock/measurement.json', sha256='b' * 64)
        trial = CollectionTrial(FakeSession(), snapshot(), 'artifacts/mock/input.json',
                                scroll_profile=profile, scroll_profile_source=source)
        expected = copy.deepcopy(trial.scroll_profile)
        profile['delta'] = -120
        source['sha256'] = 'c' * 64
        self.assertEqual(trial.scroll_profile, expected)
        policy = trial.summary['listing_scroll_policy']
        self.assertEqual(policy['mode'], 'calibrated_complete_window')
        self.assertEqual(policy['calibration_source']['sha256'], 'b' * 64)
        self.assertEqual(policy['calibration_sha256'], hashlib.sha256(json.dumps(expected,
            ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()).hexdigest())
        self.assertFalse(policy['infer_batch_distance_from_single_notch'])
        self.assertFalse(policy['cross_window_geometry_identity_skip'])

    def test_positive_profile_is_not_used_to_scan_backwards(self):
        session = FakeSession()
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_PROFILE_DIRECTION$'):
            self.trial(session, fixture_profile(480))
        self.assertEqual(session.steps, [])

    def test_calibration_validator_failure_happens_before_any_session_actions(self):
        self.validator.side_effect = ValueError('MOCK_UNMEASURED_PROFILE')
        session = FakeSession()
        with self.assertRaisesRegex(ValueError, 'MOCK_UNMEASURED_PROFILE'):
            self.trial(session)
        self.assertEqual(session.steps, [])

    def test_window_passes_complete_records_to_coverage_and_profile_to_planner(self):
        events = []
        trial = self.trial(events=events)
        records = [dict(card={'id': 'inert'}, candidate={'price': '230'}, disposition='favorite_preserved')]
        coverage = dict(schema='mock-current-window-coverage', frame_id='fake:1')
        before = trial.observed
        with patch('collection_scroll.build_scroll_coverage', return_value=coverage, create=True) as builder, \
             patch('run_collection_trial.scroll_plan', side_effect=fixture_plan) as planner:
            trial.scroll_window(row(), records)
        builder.assert_called_once_with(before, row(), records)
        self.assertEqual(planner.call_args.kwargs,
                         dict(delta=-480, calibration=trial.scroll_profile, coverage=coverage))
        self.assertEqual([step['kind'] for step in trial.session.steps], ['scroll_visible_list', 'capture'])
        self.assertEqual([event['event'] for event in events],
                         ['listing_window_coverage_proven', 'listing_scroll_observed', 'listing_scroll_window_complete'])
        self.assertFalse(events[-1]['prior_positions_skipped_as_identity'])
        self.assertNotEqual(events[-1]['next_frame_id'], 'fake:1')

    def test_incomplete_or_conflicting_coverage_never_dispatches_a_wheel(self):
        trial = self.trial()
        with patch('collection_scroll.build_scroll_coverage', side_effect=ValueError('MOCK_COVERAGE_INCOMPLETE'), create=True), \
             patch('run_collection_trial.scroll_plan') as planner:
            with self.assertRaisesRegex(ValueError, 'MOCK_COVERAGE_INCOMPLETE'):
                trial.scroll_window(row(), [])
        planner.assert_not_called()
        self.assertEqual(trial.session.steps, [])

    def test_profile_window_planner_failure_does_not_fall_back_to_single_notch(self):
        trial = self.trial()
        with patch('collection_scroll.build_scroll_coverage', return_value={}, create=True), \
             patch('run_collection_trial.scroll_plan', side_effect=ValueError('MOCK_OVERSHOOT')) as planner:
            with self.assertRaisesRegex(ValueError, 'MOCK_OVERSHOOT'):
                trial.scroll_window(row(), [])
        self.assertEqual(planner.call_count, 1)
        self.assertEqual(trial.session.steps, [])

    def test_failed_postscroll_rebind_does_not_announce_window_completion(self):
        events = []
        trial = self.trial(events=events)
        original_perform = trial.session.perform
        def perform(step):
            result = original_perform(step)
            if step['kind'] == 'capture':
                trial.session.previous.pop('collection_geometry_rebind', None)
            return result
        trial.session.perform = perform
        with patch('collection_scroll.build_scroll_coverage', return_value={}, create=True), \
             patch('run_collection_trial.scroll_plan', side_effect=fixture_plan):
            with self.assertRaisesRegex(RuntimeError, '^COLLECTION_SCROLL_REBIND_REQUIRED$'):
                trial.scroll_window(row(), [])
        self.assertNotIn('listing_scroll_window_complete', [e['event'] for e in events])
        self.assertEqual(sum(s['kind'] == 'scroll_visible_list' for s in trial.session.steps), 1)
        self.assertFalse(any(s['kind'] == 'collect_selected' for s in trial.session.steps))

    def test_ensure_list_top_uses_original_small_upward_scroll_without_profile(self):
        session = FakeSession()
        session.window = 1
        session.previous = listing(window=1)
        trial = self.trial(session)
        with patch('run_collection_trial.scroll_plan', side_effect=fixture_plan) as planner, \
             patch('collection_scroll.build_scroll_coverage', create=True) as builder:
            trial.ensure_list_top(row())
        self.assertEqual(planner.call_args.kwargs, dict(delta=120))
        builder.assert_not_called()
        self.assertEqual(session.steps[0]['lease']['delta'], 120)

    def test_no_profile_keeps_legacy_explicitly_labeled_without_invented_calibration(self):
        events = []
        trial = TestTrial(FakeSession(), snapshot(), 'artifacts/mock/input.json', emit=events.append)
        self.assertEqual(trial.summary['listing_scroll_policy']['mode'], 'legacy_single_notch')
        self.assertIsNone(trial.summary['listing_scroll_policy']['calibration_sha256'])
        self.validator.assert_not_called()
        with patch('run_collection_trial.scroll_plan', side_effect=fixture_plan) as planner, \
             patch('collection_scroll.build_scroll_coverage', create=True) as builder:
            trial.scroll_window(row(), [])
        self.assertEqual(planner.call_args.kwargs, dict(delta=-120))
        builder.assert_not_called()
        self.assertEqual(events[0]['event'], 'listing_scroll_calibration_not_selected')

    def test_scan_records_each_processed_card_and_clears_only_after_new_window(self):
        session = FakeSession([{'price': 230, 'gold': True}] * 6
            + [{'price': 230, 'gold': True}, {'price': 601, 'eligible': False}])
        trial = self.trial(session)
        captured = []
        def coverage(packet, current_rule, records):
            captured.append(copy.deepcopy(records))
            self.assertEqual(len(records), 6)
            self.assertTrue(all(item['disposition'] == 'favorite_preserved' for item in records))
            self.assertEqual({item['card']['id'] for item in records}, {f'c{i}' for i in range(6)})
            return dict(schema='mock-current-window-coverage')
        with patch('collection_scroll.build_scroll_coverage', side_effect=coverage, create=True), \
             patch('run_collection_trial.scroll_plan', side_effect=fixture_plan):
            trial.scan_row(row())
        self.assertEqual(trial.summary['scanned_candidates'], 8)
        self.assertEqual(trial.summary['already_favorited'], 7)
        self.assertEqual(trial.summary['rows'][0]['visible_windows'], 2)
        # The inert fixture deliberately reuses the same rectangles/ids in a
        # new frame. Runner must parse new items, never skip by old position.
        self.assertEqual(sum(s['kind'] == 'collect_selected' for s in session.steps), 8)
        self.assertEqual(sum(s['kind'] == 'scroll_visible_list' for s in session.steps), 1)
        self.assertEqual(len(captured), 1)


class ScrollProfileFileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'artifacts').mkdir()
        self.root_patch = patch.object(runner, 'ROOT', self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def test_profile_file_is_explicit_hashed_and_validated(self):
        path = self.root / 'artifacts/profile.json'
        evidence = self.root / 'artifacts/measured.json'
        evidence.write_bytes(b'{"inert_test_evidence":true}')
        profile_document = dict(fixture_profile(), source=dict(record_path=str(evidence),
                                record_sha256=hashlib.sha256(evidence.read_bytes()).hexdigest()))
        raw = json.dumps(profile_document).encode()
        path.write_bytes(raw)
        with patch('collection_scroll.validate_scroll_calibration', side_effect=copy.deepcopy, create=True) as validate, \
                patch.object(runner, 'validate_scroll_measurement_evidence', return_value=True) as evidence_validate:
            profile, source = load_scroll_profile(path)
        validate.assert_called_once_with(profile_document)
        evidence_validate.assert_called_once_with(profile_document, {'inert_test_evidence': True})
        self.assertEqual(profile, profile_document)
        self.assertEqual(source, dict(path=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest(),
            evidence_path=str(evidence), evidence_sha256=hashlib.sha256(evidence.read_bytes()).hexdigest(),
            evidence_hash_verified=True, evidence_semantics_verified=True))

    def test_profile_evidence_must_exist_inside_artifacts_and_match_hash(self):
        path = self.root / 'artifacts/profile.json'
        outside = self.root / 'not-in-artifacts.json'
        outside.write_text('{}', encoding='utf-8')
        actual = self.root / 'artifacts/measurement.json'
        actual.write_text('{}', encoding='utf-8')
        for source, expected in ((outside, 'EVIDENCE_PATH'),
                                 (self.root / 'artifacts/missing.json', 'EVIDENCE_PATH'),
                                 (actual, 'EVIDENCE_HASH')):
            data = dict(fixture_profile(), source=dict(record_path=str(source), record_sha256='a' * 64))
            path.write_text(json.dumps(data), encoding='utf-8')
            with patch('collection_scroll.validate_scroll_calibration', side_effect=copy.deepcopy, create=True):
                with self.subTest(source=source), self.assertRaisesRegex(ValueError, expected):
                    load_scroll_profile(path)

    def test_outside_workspace_or_nonjson_profile_is_rejected(self):
        for path in (self.root / 'outside.json', self.root / 'artifacts/profile.txt'):
            with patch('collection_scroll.validate_scroll_calibration', create=True) as validate:
                with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'PROFILE_PATH'):
                    load_scroll_profile(path)
                validate.assert_not_called()

    def test_empty_oversized_or_invalid_json_profile_rejected_before_execution(self):
        path = self.root / 'artifacts/profile.json'
        for raw in (b'', b'x' * (512 * 1024 + 1), b'{'):
            path.write_bytes(raw)
            with patch('collection_scroll.validate_scroll_calibration', create=True) as validate:
                with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                    load_scroll_profile(path)
                validate.assert_not_called()


def geometry_profile():
    """Synthetic dimensional contract only; not a deployed live calibration."""
    return dict(schema='collection-scroll-calibration-v1', delta=-480,
        source=dict(record_path='artifacts/inert-contract-only.json', record_sha256='a' * 64,
                    before_frame_id='inert:1', after_frame_id='inert:2',
                    measurement_method='synthetic_contract_fixture_not_live'),
        viewport=[2560, 1440], listing_viewport=[120, 303, 1742, 947],
        shape=dict(column_left_ranges=[[119, 122], [996, 999]], card_width_range=[861, 867],
                   card_height_range=[260, 268], fields_height_range=[40, 46], row_pitch_range=[270, 280]),
        scrollbar=dict(track_bounds=[1870, 303, 8, 947], thumb_height_range=[199, 201]),
        content_displacement_pixels=[598, 602], thumb_displacement_pixels=[99, 101],
        content_pixels_per_thumb_pixel=[5.98, 6.02])


def processed_window(packet):
    layout = observe_layout(packet)
    return [dict(card=copy.deepcopy(card), disposition='favorite_preserved',
        candidate=dict(product=row()['product_name'], condition='成色S', price='230',
            wear=f'0.{index + 1:06d}', row_index=row()['row_index'], eligible=True,
            source_frame_id=layout['frame_id'], source_frame_sha256=layout['frame_sha256'],
            geometry_mode='observed_dynamic', card_id=card['id'], card_bounds=card['bounds'],
            fields_bounds=card['fields_bounds']))
        for index, card in enumerate(layout['cards']) if card['selectable']]


def geometry_after_batch():
    packet = listing(serial=2)
    cards = []
    for column, x in enumerate((120, 997)):
        top = geometry_card(f'old-clipped-{column}', x, 303, height=216, partial=True)
        top['edges'].update(top=False, bottom=True)
        cards.extend([top, geometry_card(f'first-unscanned-{column}', x, 528),
                      geometry_card(f'new-complete-{column}', x, 803),
                      geometry_card(f'new-partial-{column}', x, 1078, height=172, partial=True)])
    packet['collection_layout']['cards'] = cards
    packet['collection_layout']['scrollbar']['thumb_bounds'][1] += 100
    packet['collection_selected_card'] = dict(selected=False)
    return packet


class ContractSession(FakeSession):
    """Real plan/lease/rebind contracts, inert packets in place of wheel IO."""
    def __init__(self, after=None):
        super().__init__()
        self.after = after or geometry_after_batch()
        self.scroll_lease = None

    def perform(self, step):
        self.steps.append(copy.deepcopy(step))
        if step['kind'] == 'scroll_visible_list':
            self.scroll_lease = validate_scroll_lease(self.previous, row(), [], step['lease'])
            return dict(passed=True)
        if step['kind'] == 'capture':
            self.previous = copy.deepcopy(self.after)
            rebound = rebind_after_scroll(self.previous, row(), self.scroll_lease)
            self.previous['collection_geometry_rebind'] = dict(kind='scroll', passed=True, evidence=rebound)
            return dict(passed=True)
        raise AssertionError('No input beyond the inert planned wheel is permitted: ' + step['kind'])


class RealBatchContractIntegrationTests(unittest.TestCase):
    def test_runner_uses_real_coverage_plan_and_readback_contracts_without_identity_skip(self):
        session = ContractSession()
        events = []
        trial = CollectionTrial(session, snapshot(), 'artifacts/inert/input.json',
            emit=events.append, scroll_profile=geometry_profile())
        records = processed_window(session.previous)
        before = copy.deepcopy(records)
        trial.scroll_window(row(), records)
        self.assertEqual(records, before)
        self.assertEqual([s['kind'] for s in session.steps], ['scroll_visible_list', 'capture'])
        lease = session.steps[0]['lease']
        self.assertEqual(lease['delta'], -480)
        self.assertEqual(lease['coverage']['mode'], 'collection_completed')
        self.assertEqual(len(lease['coverage']['current_cards']), 6)
        rebound = session.previous['collection_geometry_rebind']['evidence']
        self.assertFalse(rebound['collection_allowed'])
        self.assertFalse(rebound['old_card_coordinates_reused'])
        self.assertTrue(rebound['batch_scroll_evidence']['first_unscanned_boundary_visible_under_calibrated_motion_model'])
        self.assertFalse(rebound['batch_scroll_evidence']['skipped_by_old_identity'])
        self.assertEqual(events[-1]['event'], 'listing_scroll_window_complete')

    def test_real_coverage_incomplete_or_readonly_probe_disposition_never_reaches_wheel(self):
        for mutation in ('missing_card', 'read_only_probe'):
            session = ContractSession()
            trial = CollectionTrial(session, snapshot(), 'artifacts/inert/input.json', scroll_profile=geometry_profile())
            records = processed_window(session.previous)
            if mutation == 'missing_card':
                records.pop()
            else:
                records[0]['disposition'] = 'read_only_observed'
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_COVERAGE_'):
                trial.scroll_window(row(), records)
            self.assertEqual(session.steps, [])

    def test_real_profile_excess_distance_is_rejected_without_single_notch_fallback(self):
        session = ContractSession()
        profile = geometry_profile()
        profile['content_displacement_pixels'] = [840, 860]
        trial = CollectionTrial(session, snapshot(), 'artifacts/inert/input.json', scroll_profile=profile)
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE$'):
            trial.scroll_window(row(), processed_window(session.previous))
        self.assertEqual(session.steps, [])

    def test_real_postscroll_drift_remains_a_failure_and_no_collection_follows(self):
        after = geometry_after_batch()
        after['collection_layout']['scrollbar']['thumb_bounds'][1] += 8
        session = ContractSession(after)
        events = []
        trial = CollectionTrial(session, snapshot(), 'artifacts/inert/input.json',
            emit=events.append, scroll_profile=geometry_profile())
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_BATCH_THUMB_DISTANCE$'):
            trial.scroll_window(row(), processed_window(session.previous))
        self.assertEqual(len(session.steps), 2)
        self.assertFalse(any(step['kind'] == 'collect_selected' for step in session.steps))
        self.assertNotIn('listing_scroll_window_complete', [event['event'] for event in events])


if __name__ == '__main__':
    unittest.main()
