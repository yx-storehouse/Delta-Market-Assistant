"""Pure fake-backend session tests; never bind a game or send OS input."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from collection_live_session import ForegroundSession, conservative_observation_age
from run_foreground_batch import validate_plan


def packet(digest='a', *, gold=False):
    regions = [dict(kind='product_title', ok=True, truncated=False,
                    words=[dict(text='AUG突击步枪-天命', x=1930, y=245, width=240, height=25)]),
               dict(kind='first_card_fields', ok=True, truncated=False,
                    words=[dict(text='成色S', x=127, y=536, width=60, height=20),
                           dict(text='230', x=937, y=538, width=39, height=19)]),
               dict(kind='selected_detail', ok=True, truncated=False,
                    words=[dict(text='S(0.187079)', x=2214, y=660, width=140, height=20)])]
    return dict(capture_passed=True, ocr_passed=True, focus_activation_requests=0, focus_restore_requests=0,
                startup_page=dict(page='skin_listings', overlay='none', anchor_checks={'collection.added': gold}),
                frames=[dict(width=2560, height=1440, sha256=digest * 64,
                             source_age_ms=25, source_uncertainty_ms=1, capture_ms=100)],
                ocr=dict(frame_age_at_result_ms=200),
                collection_observation=dict(frame_id='fake:' + digest, frame_sha256=digest * 64,
                                            same_frame=True, regions=regions),
                collection_selected_card=dict(index=0, selected=True,
                    favorite_warm_fraction=.12 if gold else 0, favorite_bright_fraction=0 if gold else .1))


def snapshot():
    return dict(ready=True, mode='collect_only', purchase_phase_enabled=False, source_sha256='e' * 64,
                rows=[dict(enabled=True, dictionary_resolved=True, row_index=0, product_name='AUG突击步枪-天命',
                           condition_label='成色S', price_min='10', price_max='600', max_wear='5', limit_raw=0)])


def capture(**extra):
    return dict(kind='capture', expected_page='skin_listings', collection_observation=True, **extra)


def collect(**extra):
    return dict(kind='collect_selected', expected_before='skin_listings',
                snapshot='artifacts/trial/input/snapshot.json', row_index=0,
                viewport=[2560, 1440], skip_ineligible=True, skip_favorited=True, **extra)


class Clock:
    def __init__(self):
        self.value = 1000.0
        self.waits = []
    def now(self):
        return self.value
    def wait(self, seconds):
        self.waits.append(seconds)
        self.value += seconds


class FakeBackend:
    def __init__(self, clock, replies=None):
        self.clock = clock
        self.replies = list(replies or [])
        self.identities = dict(target_hwnd=1, target_pid=2, return_hwnd=3, return_pid=4)
        self.events = []
        self.active = False
        self.enter_error = self.leave_error = None
        self.capture_seconds = .4
        self.sent = 2
        self.motion_seconds = 0
        self.last_motion = None
        self.viewport_size = (2560, 1440)
    def enter(self):
        self.events.append('enter')
        self.active = True
        if self.enter_error:
            raise self.enter_error
    def foreground(self):
        return self.active
    def viewport(self):
        return self.viewport_size
    def neutral_pointer(self):
        self.events.append('neutral')
    def capture(self, command, timeout):
        self.events.append(('capture', list(command), timeout))
        self.clock.wait(self.capture_seconds)
        value = self.replies.pop(0) if self.replies else packet()
        if isinstance(value, BaseException):
            raise value
        return SimpleNamespace(returncode=value.pop('_exit', 0), stdout=json.dumps(value).encode(), stderr=b'')
    def click(self, point, before_dispatch=None):
        self.clock.wait(self.motion_seconds)
        if self.motion_seconds:
            self.last_motion = dict(elapsed_ms=self.motion_seconds*1000, completed=True, planned_points=5,
                                    moved_points=4, path_type='fake_motion')
        if before_dispatch is not None:
            before_dispatch()
        self.events.append(('click', list(point)))
        self.clock.wait(.01)
        return self.sent
    def scroll(self, point, delta, before_dispatch=None):
        self.clock.wait(self.motion_seconds)
        if before_dispatch is not None:
            before_dispatch()
        self.events.append(('scroll', list(point), delta))
        return 1
    def hover(self, point):
        self.events.append(('hover', list(point)))
    def leave(self):
        self.events.append('leave')
        self.active = False
        if self.leave_error:
            raise self.leave_error
        return True
    def metadata(self):
        return dict(identities=self.identities, backend='fake_offline', foreground_transitions=[])


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.snapshot_path = self.root / 'artifacts/trial/input/snapshot.json'
        self.snapshot_path.parent.mkdir(parents=True)
        self.snapshot_path.write_text(json.dumps(snapshot()), encoding='utf-8')
        self.clock = Clock()
        self.backend = FakeBackend(self.clock)

    def session(self, **options):
        return ForegroundSession('artifacts/trial/run.json', root=self.root,
            backend=self.backend, now=self.clock.now, wait=self.clock.wait, **options)

    def load(self, session):
        return json.loads(session.report_path.read_text(encoding='utf-8'))

    def test_multiple_subplans_hold_one_foreground_lease(self):
        session = self.session()
        with session:
            for unused in range(3):
                session.perform(capture())
                session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
                session.perform(capture())
            self.assertEqual(self.backend.events.count('enter'), 1)
            self.assertNotIn('leave', self.backend.events)
            self.assertEqual(len(self.load(session)['steps']), 9)
        self.assertEqual(self.backend.events.count('leave'), 1)
        self.assertTrue(session.report['passed'])
        self.assertEqual(session.report['manual_clicks'], 3)
        self.assertEqual(session.report['enter_calls'], 1)
        self.assertEqual(session.report['leave_calls'], 1)

    def test_finish_is_idempotent(self):
        session = self.session()
        with session:
            session.perform(capture())
        first = copy.deepcopy(session.report)
        self.assertEqual(session.finish(), first)
        self.assertEqual(self.backend.events.count('leave'), 1)

    def test_external_exception_restores_once_and_records_error(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'orchestrator conflict'):
            with session:
                session.perform(capture())
                raise RuntimeError('orchestrator conflict')
        self.assertEqual(self.backend.events.count('leave'), 1)
        self.assertEqual(self.load(session)['error'], 'orchestrator conflict')
        self.assertFalse(session.report['passed'])

    def test_entry_failure_still_cleans_up_once(self):
        self.backend.enter_error = RuntimeError('activation failed')
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'activation failed'):
            with session:
                self.fail('entry must fail')
        session.finish()
        self.assertEqual(self.backend.events, ['enter', 'leave'])
        self.assertEqual(self.load(session)['error'], 'activation failed')

    def test_restore_failure_is_not_success(self):
        self.backend.leave_error = RuntimeError('restore failed')
        session = self.session()
        with session:
            session.perform(capture())
        self.assertFalse(session.report['passed'])
        self.assertEqual(session.report['restore_error'], 'restore failed')

    def test_focus_loss_stops_without_reacquisition(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_FOREGROUND_LOST'):
            with session:
                session.perform(capture())
                self.backend.active = False
                session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
        self.assertEqual(self.backend.events.count('enter'), 1)
        self.assertEqual(self.backend.events.count('leave'), 1)
        self.assertFalse(any(isinstance(event, tuple) and event[0] == 'click' for event in self.backend.events))
        self.assertEqual(self.load(session)['steps'][-1]['status'], 'failed')

    def test_subprocess_timeout_records_step_and_restores(self):
        self.backend.replies = [subprocess.TimeoutExpired('diagnostic', 20)]
        session = self.session()
        with self.assertRaises(subprocess.TimeoutExpired):
            with session:
                session.perform(capture())
        report = self.load(session)
        self.assertEqual(report['steps'][0]['status'], 'failed')
        self.assertIn('timings', report['steps'][0])
        self.assertTrue(report['ide_restored'])

    def test_failed_known_capture_is_not_retried(self):
        self.backend.replies = [dict(packet(), capture_passed=False, _exit=1)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(capture())
        self.assertEqual(len([e for e in self.backend.events if isinstance(e, tuple) and e[0] == 'capture']), 1)

    def test_a_lost_desktop_frame_is_read_again_except_in_a_countdown_watch(self):
        # Live 2026-10-10 00:53/00:54: E_DXGI_ACQUIRE once, the next capture fine.
        lost = dict(packet(), capture_passed=False, _exit=1, error='E_DXGI_ACQUIRE')
        self.backend.replies = [lost, packet('b')]
        session = self.session()
        with session:
            result = session.perform(capture())
        self.assertEqual(result['attempt_count'], 2)
        self.assertTrue(result['capture_attempts'][0]['capture_acquire_retryable'])
        self.assertIn(.3, self.clock.waits)

    def test_a_follow_watch_carries_its_followed_zero(self):
        session = self.session()
        with session:
            command = session._command(dict(kind='capture', collection_observation=True, purchase_observation=True,
                                            countdown_watch_ms=1000, stop_on_jump=True, expect_zero_ms=1234567.25))
        self.assertIn('--purchase-countdown-stop-on-jump', command)
        self.assertEqual(command[command.index('--purchase-countdown-expect-zero-ms') + 1], '1234567.2')

    def test_a_timed_countdown_watch_is_never_repeated(self):
        # Its length is planned to the moment of the entry.
        self.backend.replies = [dict(packet(), capture_passed=False, _exit=1, error='E_DXGI_ACQUIRE')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(dict(kind='capture', collection_observation=True, purchase_observation=True,
                                     countdown_watch_ms=1000))
        self.assertEqual(len([e for e in self.backend.events if isinstance(e, tuple) and e[0] == 'capture']), 1)

    def test_unknown_page_retry_stays_in_same_session(self):
        unknown = packet()
        unknown.update(_exit=1, page_error='E_DIAGNOSTIC_PAGE_MISMATCH')
        unknown['startup_page']['page'] = 'unknown'
        self.backend.replies = [unknown, packet('b')]
        session = self.session()
        with session:
            result = session.perform(capture())
        self.assertEqual(result['attempt_count'], 2)
        self.assertEqual(session.report['enter_calls'], 1)
        self.assertEqual(session.report['leave_calls'], 1)
        self.assertEqual(len(result['capture_attempts']), 2)

    def test_deadline_and_caller_budget_are_not_sleep_durations(self):
        session = self.session(timeout_seconds=10)
        with session:
            session.perform(capture(), remaining=2)
        capture_event = next(e for e in self.backend.events if isinstance(e, tuple) and e[0] == 'capture')
        self.assertEqual(capture_event[2], 2)
        self.assertNotIn(2, self.clock.waits)
        self.assertNotIn(10, self.clock.waits)

    def test_deadline_expiry_stops_next_step(self):
        session = self.session(timeout_seconds=2)
        with self.assertRaisesRegex(RuntimeError, 'BATCH_DEADLINE'):
            with session:
                self.clock.wait(3)
                session.perform(capture())
        self.assertTrue(session.report['ide_restored'])

    def test_image_preview_is_only_kept_in_memory(self):
        marker = 'SECRET_IMAGE_PAYLOAD'
        self.backend.replies = [dict(packet(), preview_png_base64=marker)]
        session = self.session()
        with session:
            session.perform(capture(preview=True))
            self.assertEqual(session.last_preview, marker)
            self.assertNotIn('preview_png_base64', session.previous)
        for path in (self.root / 'artifacts').rglob('*'):
            if path.is_file():
                self.assertNotIn(marker, path.read_text(encoding='utf-8'))
        self.assertEqual(session.report['image_file_writes'], 0)

    def test_click_requires_fresh_observation_before_second_input(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_PAGE'):
            with session:
                session.perform(capture())
                session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
                session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_timing_fields_persist_after_every_step(self):
        session = self.session()
        with session:
            session.perform(capture())
            self.assertAlmostEqual(self.load(session)['steps'][0]['capture_roundtrip_ms'], 400)
            session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
            timing = self.load(session)['steps'][1]['timings']
            self.assertAlmostEqual(timing['input_dispatch_ms'], 10)
            self.assertAlmostEqual(timing['settle_wait_ms'], 600)
            self.assertAlmostEqual(timing['total_ms'], 610)

    def test_snapshot_collection_receipt_and_legacy_journal_identity(self):
        self.backend.replies = [packet(), packet('b', gold=True)]
        session = self.session()
        with session:
            session.perform(capture())
            session.perform(collect())
            self.assertIsNotNone(session.pending)
            key = session.pending['key']
            record = json.loads((session.journal.directory / (key + '.json')).read_text(encoding='utf-8'))
            self.assertEqual(record['status'], 'dispatched')
            session.perform(capture(expect_collection_added=True, receipt_if_pending=True))
            self.assertIsNone(session.pending)
            record = json.loads((session.journal.directory / (key + '.json')).read_text(encoding='utf-8'))
            self.assertEqual(record['status'], 'confirmed')
            self.assertEqual(record['candidate']['source_sha256'], 'e' * 64)
        self.assertTrue(session.report['passed'])
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_snapshot_first_read_must_match_orchestrator_byte_hash(self):
        original_hash = hashlib.sha256(self.snapshot_path.read_bytes()).hexdigest()
        data = snapshot()
        data['rows'][0]['price_max'] = '1000'
        self.snapshot_path.write_text(json.dumps(data), encoding='utf-8')
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SNAPSHOT_CHANGED'):
            with session:
                session.perform(capture())
                session.perform(collect(snapshot_sha256=original_hash))
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertEqual(list(session.journal.directory.glob('*.json')), [])

    def test_snapshot_changed_after_first_read_stops_even_without_supplied_hash(self):
        self.backend.replies = [packet(gold=True), packet('b')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SNAPSHOT_CHANGED'):
            with session:
                session.perform(capture())
                session.perform(collect())  # Gold skips input but freezes rule bytes.
                data = snapshot()
                data['rows'][0]['max_wear'] = '0.1'
                self.snapshot_path.write_text(json.dumps(data), encoding='utf-8')
                session.perform(capture())
                session.perform(collect())
        self.assertEqual(session.report['manual_clicks'], 0)

    def test_matching_snapshot_hash_accepts_same_bytes_and_retains_source_hash(self):
        digest = hashlib.sha256(self.snapshot_path.read_bytes()).hexdigest()
        self.backend.replies = [packet(), packet('b', gold=True)]
        session = self.session()
        with session:
            session.perform(capture())
            session.perform(collect(snapshot_sha256=digest))
            self.assertEqual(session.pending['candidate']['source_sha256'], 'e' * 64)
            session.perform(capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertTrue(session.report['passed'])

    def test_per_step_fields_persist_without_rewriting_historical_payloads(self):
        session = self.session()
        with session:
            session.perform(capture())
            first = session.step_directory / '000001.json'
            frozen = first.read_bytes()
            session.perform(capture())
            self.assertEqual(first.read_bytes(), frozen)
            summary = self.load(session)
            self.assertTrue(summary['step_payloads_external_until_finish'])
            self.assertNotIn('result', summary['steps'][0])
            self.assertIn('result', json.loads(first.read_text(encoding='utf-8')))
            self.assertIn('result', session.steps[0])
        self.assertIn('result', self.load(session)['steps'][0])

    def test_pending_receipt_blocks_navigation_and_segment_reset(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_PENDING_RECONCILIATION'):
            with session:
                session.perform(capture())
                session.perform(collect())
                with self.assertRaisesRegex(RuntimeError, 'COLLECTION_PENDING_RECONCILIATION'):
                    session.reset_segment()
                session.perform(dict(kind='click', expected_before='skin_listings', point=[168, 1403]))
        self.assertIsNotNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_finish_with_pending_does_not_claim_completed(self):
        session = self.session()
        with session:
            session.perform(capture())
            session.perform(collect())
        self.assertFalse(session.report['passed'])
        self.assertEqual(session.report['error'], 'COLLECTION_PENDING_RECONCILIATION')

    def test_partial_input_is_durable_and_not_repeated(self):
        self.backend.sent = 1
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_INPUT_UNCERTAIN'):
            with session:
                session.perform(capture())
                session.perform(collect())
        paths = list(session.journal.directory.glob('*.json'))
        self.assertEqual(len(paths), 1)
        self.assertEqual(json.loads(paths[0].read_text(encoding='utf-8'))['status'], 'input_uncertain')
        self.assertEqual(len([e for e in self.backend.events if isinstance(e, tuple) and e[0] == 'click']), 1)

    def test_existing_gold_is_never_toggled(self):
        self.backend.replies = [packet(gold=True)]
        session = self.session()
        with session:
            session.perform(capture())
            result = session.perform(collect())
            skipped = session.perform(capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(result['reason'], 'already_favorited')
        self.assertEqual(skipped['reason'], 'no_collection_dispatched')
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertEqual(list(session.journal.directory.glob('*.json')), [])

    def test_nonzero_quantity_stops_before_any_input(self):
        data = snapshot()
        data['rows'][0]['limit_raw'] = 2
        self.snapshot_path.write_text(json.dumps(data), encoding='utf-8')
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_NONZERO_LIMIT_REQUIRES_COUNTER'):
            with session:
                session.perform(capture())
                session.perform(collect())
        self.assertEqual(session.report['manual_clicks'], 0)

    def test_snapshot_cannot_escape_artifacts_or_enable_purchase(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SNAPSHOT_PATH'):
            with session:
                session.perform(capture())
                step = collect()
                step['snapshot'] = '../outside.json'
                session.perform(step)
        self.assertEqual(session.report['manual_clicks'], 0)

    def test_wrong_snapshot_mode_stops_before_input(self):
        data = snapshot()
        data['purchase_phase_enabled'] = True
        self.snapshot_path.write_text(json.dumps(data), encoding='utf-8')
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SNAPSHOT_MODE'):
            with session:
                session.perform(capture())
                session.perform(collect())

    def test_segment_boundary_skips_until_explicit_reset(self):
        data = snapshot()
        data['rows'][0]['price_max'] = '100'
        self.snapshot_path.write_text(json.dumps(data), encoding='utf-8')
        session = self.session()
        with session:
            session.perform(capture())
            session.perform(collect(end_segment_on_price_above=True))
            self.assertTrue(session.segment_finished)
            self.assertTrue(session.perform(capture())['skipped'])
            session.reset_segment()
            self.assertNotIn('skipped', session.perform(capture()))
        self.assertTrue(session.report['passed'])

    def test_encoding_tail_is_counted_against_unchanged_5000ms(self):
        self.backend.capture_seconds = 5.1
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_OBSERVATION_EXPIRED'):
            with session:
                session.perform(capture())
                self.assertEqual(session.previous['ocr']['frame_age_at_result_ms'], 200)
                self.assertGreater(session.previous_age_upper_ms, 5000)
                session.perform(collect())
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertEqual(list(session.journal.directory.glob('*.json')), [])

    def test_delay_after_capture_is_counted(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_OBSERVATION_EXPIRED'):
            with session:
                session.perform(capture())
                self.clock.wait(5)
                session.perform(collect())

    def test_age_rechecked_after_journal_prepare(self):
        session = self.session()
        prepare = session.journal.prepare
        def slow_prepare(*args, **kwargs):
            result = prepare(*args, **kwargs)
            self.clock.wait(5)
            return result
        session.journal.prepare = slow_prepare
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_OBSERVATION_EXPIRED'):
            with session:
                session.perform(capture())
                session.perform(collect())
        self.assertIsNotNone(session.pending)
        path = next(session.journal.directory.glob('*.json'))
        self.assertEqual(json.loads(path.read_text(encoding='utf-8'))['status'], 'prepared')
        self.assertFalse(any(isinstance(e, tuple) and e[0] == 'click' for e in self.backend.events))

    def test_existing_report_not_overwritten(self):
        first = self.session()
        original = first.report_path.read_bytes()
        with self.assertRaises(FileExistsError):
            self.session()
        self.assertEqual(first.report_path.read_bytes(), original)

    def test_path_outside_artifacts_rejected_before_backend_entry(self):
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_REPORT_PATH'):
            ForegroundSession('elsewhere.json', root=self.root, backend=self.backend)
        self.assertEqual(self.backend.events, [])


class WindowLostTests(unittest.TestCase):
    """Review wf_993b38d0-6b8: a capture the native side refused because the
    game left the front (or was covered) is reported as that, not as a read
    failure, so the F2 cycle stops instead of pulling the game back."""

    def run_capture(self, error, foreground_after=True):
        clock = Clock()
        refused = dict(packet(), capture_passed=False, error=error, _exit=1)
        backend = FakeBackend(clock, [refused])
        original = backend.capture

        def capture(command, timeout):
            reply = original(command, timeout)
            backend.active = foreground_after
            return reply
        backend.capture = capture
        with tempfile.TemporaryDirectory() as directory:
            session = ForegroundSession('artifacts/test/session.json', root=Path(directory), backend=backend,
                                        now=clock.now, wait=clock.wait)
            with self.assertRaises(RuntimeError) as raised:
                with session:
                    session.perform(dict(kind='capture', collection_observation=True, attempts=1))
        return str(raised.exception)

    def test_native_window_refusals_name_the_window(self):
        self.assertEqual(self.run_capture('E_WINDOW_NOT_FOREGROUND'), 'BATCH_FOREGROUND_LOST')
        self.assertEqual(self.run_capture('E_BATCH_FOREGROUND_NOT_OWNED'), 'BATCH_FOREGROUND_LOST')
        self.assertEqual(self.run_capture('E_WINDOW_OCCLUDED'), 'COLLECTION_TARGET_OCCLUDED')
        self.assertEqual(self.run_capture('E_PAGE_INSUFFICIENT_ANCHORS', foreground_after=False), 'BATCH_FOREGROUND_LOST')
        self.assertEqual(self.run_capture('E_PAGE_INSUFFICIENT_ANCHORS'), 'BATCH_STEP_FAILED')


class AgeAndLegacyPlanTests(unittest.TestCase):
    def test_age_upper_bound_includes_full_roundtrip_and_source_uncertainty(self):
        value = packet()
        self.assertEqual(conservative_observation_age(value, 400), 426)
        value['ocr']['frame_age_at_result_ms'] = 800
        self.assertEqual(conservative_observation_age(value, 400), 800)

    def test_unproven_or_nonfinite_age_is_not_zero(self):
        for key, value in [('source_age_ms', None), ('source_uncertainty_ms', -1),
                           ('source_age_ms', float('nan')), ('source_age_ms', True)]:
            observed = packet()
            observed['frames'][0][key] = value
            self.assertIsNone(conservative_observation_age(observed, 400))

    def test_legacy_limits_and_kinds_preserved(self):
        self.assertEqual(validate_plan(dict(steps=[capture()] * 20, timeout_seconds=30))[1], 30)
        for data in (dict(steps=[]), dict(steps=[capture()] * 21),
                     dict(steps=[capture()], timeout_seconds=31),
                     dict(steps=[dict(kind='click')]), dict(steps=[dict(kind='purchase')]),
                     dict(steps=[capture()], timeout_seconds=True)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                validate_plan(data)


if __name__ == '__main__':
    unittest.main(verbosity=2)
