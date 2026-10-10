"""Offline semantic evidence and actual session dispatch; no OS input."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_collection_trial as runner
from collection_scroll import build_scroll_coverage, scroll_plan
from test_collection_batch_scroll import calibration, processed, batch_after
from test_collection_scroll import packet, rule
import test_collection_live_session as session_fixtures


def measured_fixture():
    profile = calibration()
    profile['source']['measurement_method'] = 'same_listing_identity_after_independent_batch'
    before, after = packet(), batch_after()
    document = dict(schema='collection-scroll-calibration-measurement-v1', status='measured',
        delta=-480, measurement_method=profile['source']['measurement_method'],
        profile_parameters={key: copy.deepcopy(value) for key, value in profile.items() if key != 'source'},
        identity_matches=[])
    for name, value in [('before', before), ('after', after)]:
        layout = value['collection_layout']
        profile['source'][name + '_frame_id'] = layout['frame_id']
        document[name] = {key: copy.deepcopy(layout[key]) for key in
                          ('frame_id', 'frame_sha256', 'viewport', 'listing_viewport', 'scrollbar')}
    for column in range(2):
        candidate = dict(product=rule()['product_name'], condition=rule()['condition_label'],
                         price='230', wear=f'0.{column + 1:06d}')
        document['identity_matches'].append(dict(column_index=column,
            before_boundary_top_y=1128, after_card_top_y=540,
            reference_observation_frame_id=f'synthetic:reference:{column}',
            reference_observation_frame_sha256=str(column + 4) * 64,
            before_candidate=candidate, after_candidate=copy.deepcopy(candidate)))
    return profile, document


class EvidenceTests(unittest.TestCase):
    def test_valid_semantically_bound_record(self):
        profile, record = measured_fixture()
        self.assertTrue(runner.validate_scroll_measurement_evidence(profile, record))

    def test_arbitrary_text_or_schema_is_not_measured_evidence(self):
        profile, unused = measured_fixture()
        for value in ({'inert_test_evidence': True}, [], None):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'EVIDENCE_SCHEMA'):
                runner.validate_scroll_measurement_evidence(profile, value)

    def test_semantic_mutations_rejected(self):
        mutations = [
            ('delta', lambda r: r.update(delta=-120)),
            ('parameters', lambda r: r['profile_parameters'].update(delta=-120)),
            ('frame', lambda r: r['before'].update(frame_id='wrong')),
            ('hash', lambda r: r['after'].update(frame_sha256=r['before']['frame_sha256'])),
            ('viewport', lambda r: r['before'].update(viewport=[1920, 1080])),
            ('thumb', lambda r: r['after']['scrollbar']['thumb_bounds'].__setitem__(1, 310)),
            ('identity', lambda r: r['identity_matches'][0]['after_candidate'].update(wear='0.999999')),
            ('columns', lambda r: r['identity_matches'][1].update(column_index=0)),
            ('distance', lambda r: r['identity_matches'][0].update(after_card_top_y=815)),
            ('reference', lambda r: r['identity_matches'][0].update(reference_observation_frame_sha256='not-sha')),
        ]
        for name, mutate in mutations:
            profile, record = measured_fixture()
            mutate(record)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'EVIDENCE_'):
                runner.validate_scroll_measurement_evidence(profile, record)

    def test_matching_hash_no_longer_accepts_arbitrary_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / 'artifacts'
            artifacts.mkdir()
            profile, record = measured_fixture()
            evidence = artifacts / 'measurement.json'
            path = artifacts / 'profile.json'
            for value, valid in [({'inert_test_evidence': True}, False), (record, True)]:
                evidence.write_text(json.dumps(value), encoding='utf-8')
                profile['source'].update(record_path=str(evidence),
                    record_sha256=hashlib.sha256(evidence.read_bytes()).hexdigest())
                path.write_text(json.dumps(profile), encoding='utf-8')
                with patch.object(runner, 'ROOT', root):
                    if valid:
                        loaded, audit = runner.load_scroll_profile(path)
                        self.assertEqual(loaded, profile)
                        self.assertTrue(audit['evidence_semantics_verified'])
                    else:
                        with self.assertRaisesRegex(ValueError, 'EVIDENCE_SCHEMA'):
                            runner.load_scroll_profile(path)


class ActualSessionBatchDispatchTests(unittest.TestCase):
    def setUp(self):
        self.fixture = session_fixtures.SessionTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def prepare(self):
        value = packet()
        value.update(capture_passed=True, ocr_passed=True,
                     focus_activation_requests=0, focus_restore_requests=0,
                     ocr=dict(frame_age_at_result_ms=200))
        value['frames'][-1].update(source_age_ms=25, source_uncertainty_ms=1, capture_ms=100)
        self.fixture.backend.replies = [value]
        return value

    def test_validated_minus480_reaches_real_session_fake_backend_once(self):
        self.prepare()
        session = self.fixture.session()
        with session:
            session.perform(session_fixtures.capture(collection_layout=True))
            coverage = build_scroll_coverage(session.previous, rule(), processed(session.previous))
            plan = scroll_plan(session.previous, rule(), [], delta=-480,
                               calibration=calibration(), coverage=coverage)
            result = session.perform(plan['steps'][0])
            self.assertTrue(result['passed'])
            self.assertEqual([e for e in self.fixture.backend.events if isinstance(e, tuple) and e[0] == 'scroll'],
                             [('scroll', [991, 776], -480)])
            self.assertEqual(session.pending_geometry['lease']['delta'], -480)

    def test_unvalidated_generic_minus480_remains_rejected(self):
        self.prepare()
        session = self.fixture.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SCROLL_BOUND'):
            with session:
                session.perform(session_fixtures.capture(collection_layout=True))
                session.perform(dict(kind='scroll', expected_before='skin_listings',
                                     point=[991, 776], delta=-480, calibration=calibration()))
        self.assertFalse(any(isinstance(e, tuple) and e[0] == 'scroll' for e in self.fixture.backend.events))

    def test_modified_batch_lease_never_dispatches(self):
        self.prepare()
        session = self.fixture.session()
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_LEASE_EXPIRED'):
            with session:
                session.perform(session_fixtures.capture(collection_layout=True))
                coverage = build_scroll_coverage(session.previous, rule(), processed(session.previous))
                plan = scroll_plan(session.previous, rule(), [], delta=-480,
                                   calibration=calibration(), coverage=coverage)
                plan['steps'][0]['lease']['point'] = [2339, 320]
                session.perform(plan['steps'][0])
        self.assertFalse(any(isinstance(e, tuple) and e[0] == 'scroll' for e in self.fixture.backend.events))


if __name__ == '__main__':
    unittest.main()
