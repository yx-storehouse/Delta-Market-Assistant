"""The F2 cycle without a game: scripted attempts and collections."""
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import purchase_cycle
from evidence_archive import EvidenceKeeper
from purchase_cycle import EMPTY_BACKOFF_S, MAX_FAILURES, classify, empty_backoff, final_text, run_cycle, wait_seconds


class Stop:
    def __init__(self):
        self.flag, self.reason = False, None

    def __call__(self):
        return self.flag

    def request(self, reason='hotkey'):
        self.flag, self.reason = True, reason


class Pool:
    def __init__(self, on_take=None):
        self.taken, self.discarded, self.on_take = 0, 0, on_take

    def take(self, on_wait=None):
        self.taken += 1
        if self.on_take:
            self.on_take()
        return SimpleNamespace(identities=dict(target_hwnd=1), discard=self.discard)

    def discard(self):
        self.discarded += 1


class Overlay:
    def __init__(self):
        self.lines = []

    def show(self, text, tone='info', ttl_ms=0):
        self.lines.append(text)


def watchlist_look(seconds, frame_ms, *, page='watchlist_listings', title='AUG', button=None):
    return dict(state='counting' if seconds is not None and page == 'watchlist_listings' else page,
                countdown_seconds=seconds, frame_qpc_ms=frame_ms, page=page, overlay='none', title=title,
                button_state=button or ('publicity' if seconds is not None else 'price_button'), countdown_unread=False)


class CycleTests(unittest.TestCase):
    far_head_collection = False

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.settings = SimpleNamespace(buy=True, follow_limit=600, delay_ms=830, config=self.root / 'config.json')
        self.stop = Stop()
        self.overlay = Overlay()
        self.slept = []
        self.now = [1_000_000.0]
        self.collect_stops = []
        if not self.far_head_collection:
            # The waits are tested on their own here; collecting before a far head is in FarHeadCollectsTests.
            patch = unittest.mock.patch.object(purchase_cycle, 'COLLECT_MIN_S', 10 ** 9)
            patch.start()
            self.addCleanup(patch.stop)

    def cycle(self, attempts, collections=(), pool=None, disk_free=lambda: 10 ** 12, keeper=None):
        attempts, collections = list(attempts), list(collections)
        calls = []
        self.attempt_kw = []

        def attempt(settings, **kw):
            calls.append('attempt')
            self.attempt_kw.append(kw)
            outcome = attempts.pop(0)
            if callable(outcome):
                outcome = outcome()
            if not attempts:
                self.stop.request()
            return outcome

        def collect(args, stop, pool, overlay, **kw):
            calls.append('collect')
            self.collect_stops.append(stop)
            outcome = collections.pop(0)
            if isinstance(outcome, Exception):
                raise outcome
            if callable(outcome):
                outcome = outcome(stop)
            return outcome

        counter = iter(range(1, 100))
        peek = getattr(self, 'peek', lambda: None)
        summary = run_cycle(SimpleNamespace(config=self.settings.config), self.stop, pool or Pool(), self.overlay, hotkey='F2',
                            monitor_hwnd=1, collect=collect, runs_dir=self.root,
                            new_run_directory=lambda runs: (runs / ('cycle%d' % next(counter))).mkdir() or runs / 'cycle1',
                            attempt_fn=attempt, settings=self.settings, snapshot={}, root=self.root,
                            sleep=lambda s: self.slept.append(s), qpc_ms=lambda: self.now[0],
                            keeper=keeper or EvidenceKeeper(pack=lambda folder: 0), disk_free=disk_free, peek=peek,
                            game_in_front=getattr(self, 'game_in_front', lambda: False))
        return summary, calls

    def test_empty_watchlist_collects_then_buys(self):
        summary, calls = self.cycle([dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY'),
                                     dict(status='passed', confirm_clicks=1, outcome='bought')],
                                    [(0, dict(status='completed', new_favorites=4))])
        self.assertEqual(calls, ['attempt', 'collect', 'attempt'])
        self.assertEqual([a['action'] for a in summary['attempts']], ['collect', 'next'])
        self.assertEqual(summary['status'], 'stopped')
        self.assertTrue((self.root / 'cycle1' / 'cycle.json').is_file())

    def advancing_idle(self):
        """idle() replaced by a clock that moves by the waited time."""
        waited = []

        def fake(seconds, stop, sleep=None):
            waited.append(seconds)
            self.now[0] += seconds * 1000.0
            return False
        return waited, unittest.mock.patch.object(purchase_cycle, 'idle', side_effect=fake)

    def test_a_head_far_from_its_unlock_is_waited_for(self):
        far = dict(status='blocked', error='PURCHASE_TIMED_TOO_EARLY:1200',
                   head=dict(state='counting', countdown_seconds=1200, frame_qpc_ms=1_000_000.0))
        seconds, zero = wait_seconds(far, 600, 1_000_000.0)
        self.assertAlmostEqual(seconds, 1199.5 - 600 + purchase_cycle.WAKE_MARGIN_S)
        waited, patch = self.advancing_idle()
        with patch:
            summary, calls = self.cycle([far, dict(status='passed', confirm_clicks=1)])
        self.assertEqual(calls, ['attempt', 'attempt'])
        self.assertAlmostEqual(sum(waited), seconds)
        self.assertLessEqual(max(waited), purchase_cycle.LOOK_INTERVAL_S)    # looked at every 15 s

    def test_another_listing_seen_while_waiting_moves_the_wait(self):
        # User 2026-10-10: "我改变选中皮肤好久了都没有变化".
        far = dict(status='blocked', error='PURCHASE_TIMED_TOO_EARLY:1200',
                   head=dict(state='counting', countdown_seconds=1200, frame_qpc_ms=1_000_000.0))
        looks = []

        def peek():
            looks.append(self.now[0])
            if len(looks) == 2:   # the user selected one that unlocks in 5 min
                return watchlist_look(300, self.now[0], title='P90冲锋枪-天命')
            return watchlist_look(1200 - (self.now[0] - 1_000_000.0) / 1000.0, self.now[0], title='AUG')
        self.peek = peek
        waited, patch = self.advancing_idle()
        with patch:
            summary, calls = self.cycle([far, dict(status='passed', confirm_clicks=1)])
        self.assertEqual(len(looks), 2)                      # the second look ended the wait
        self.assertAlmostEqual(sum(waited), 2 * purchase_cycle.LOOK_INTERVAL_S)
        self.assertEqual(summary['looks']['changes'], 1)
        self.assertTrue(any('右边换成了 P90' in line for line in self.overlay.lines))

    def far(self):
        return dict(status='blocked', error='PURCHASE_TIMED_TOO_EARLY:1200',
                    head=dict(state='counting', countdown_seconds=1200, frame_qpc_ms=1_000_000.0))

    def test_looks_elsewhere_in_the_game_change_nothing(self):
        # Review wf_f9f407ac-b6e: the market list, the lobby or a match are no 我的关注 reading.
        for look in (watchlist_look(3000, 0, page='skin_listings'), watchlist_look(None, 0, page='skin_listings'),
                     watchlist_look(None, 0, page='lobby'), watchlist_look(None, 0, page='unknown')):
            self.stop, self.root, self.now = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name)), [1_000_000.0]
            self.peek = lambda look=look: dict(look, frame_qpc_ms=self.now[0])
            waited, patch = self.advancing_idle()
            with patch:
                summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1)])
            seconds, zero = wait_seconds(self.far(), 600, 1_000_000.0)
            self.assertAlmostEqual(sum(waited), seconds, msg=look['page'])       # the whole wait, unchanged
            self.assertEqual(summary['looks']['changes'], 0)
            self.assertGreater(summary['looks']['elsewhere'], 0)

    def test_a_later_listing_needs_two_agreeing_looks(self):
        later = []

        def peek():
            later.append(self.now[0])
            elapsed = (self.now[0] - 1_000_000.0) / 1000.0
            return watchlist_look(1500 - elapsed if len(later) >= 2 else 1200 - elapsed, self.now[0])
        self.peek = peek
        waited, patch = self.advancing_idle()
        with patch:
            summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1)])
        self.assertEqual(summary['looks']['changes'], 1)          # moved once, on the second agreeing look
        seconds, zero = wait_seconds(self.far(), 600, 1_000_000.0)
        self.assertAlmostEqual(sum(waited), seconds + 300, delta=1)   # the wait now ends before the later listing

    def test_an_empty_watchlist_ends_the_wait(self):
        self.peek = lambda: watchlist_look(None, self.now[0], page='empty_watchlist')
        waited, patch = self.advancing_idle()
        with patch:
            summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1)])
        self.assertEqual(waited, [purchase_cycle.LOOK_INTERVAL_S])
        self.assertEqual(summary['looks']['ended_by'], 'empty_watchlist')

    def test_a_panel_without_countdown_ends_the_wait(self):
        far = dict(status='blocked', error='PURCHASE_TIMED_TOO_EARLY:1200',
                   head=dict(state='counting', countdown_seconds=1200, frame_qpc_ms=1_000_000.0))
        self.peek = lambda: dict(watchlist_look(None, self.now[0]), state='watchlist_listings/price_button')
        waited, patch = self.advancing_idle()
        with patch:
            summary, calls = self.cycle([far, dict(status='passed', confirm_clicks=1)])
        self.assertEqual(waited, [purchase_cycle.LOOK_INTERVAL_S])
        self.assertEqual(summary['looks']['ended_by'], 'watchlist_listings/price_button')

    def test_a_press_that_did_not_buy_still_moves_on(self):
        summary, calls = self.cycle([dict(status='blocked', error='PURCHASE_TIMED_OUTCOME:lottery_lost', confirm_clicks=1),
                                     dict(status='passed', confirm_clicks=1)])
        self.assertEqual([a['action'] for a in summary['attempts']], ['next', 'next'])

    def test_waits_between_failures_do_not_reset_the_count(self):
        # Review 2026-10-09: a failure on every listing, each followed by a wait
        # for the next head, must still end the cycle.
        far = dict(status='blocked', error='PURCHASE_TIMED_TOO_EARLY:1200',
                   head=dict(state='counting', countdown_seconds=1200, frame_qpc_ms=1_000_000.0))
        failed = dict(status='blocked', error='PURCHASE_TIMED_NOT_ENTERED:PAGE')
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([failed, far, failed, far, failed, far, dict(status='passed', confirm_clicks=1)])
        self.assertEqual(len(summary['attempts']), 5)
        self.assertTrue(summary['error'].startswith('CYCLE_REPEATED_FAILURES'))

    def test_insufficient_balance_stops_buying(self):
        for result in (dict(status='blocked', error='PURCHASE_TIMED_WRONG_DIALOG:insufficient_balance,recharge_prompt'),
                       dict(status='blocked', error='PURCHASE_TIMED_OUTCOME:insufficient_balance', confirm_clicks=1,
                            outcome='insufficient_balance')):
            self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
            summary, calls = self.cycle([result, dict(status='passed', confirm_clicks=1)])
            self.assertEqual(calls, ['attempt'])
            self.assertEqual(summary['error'], 'CYCLE_INSUFFICIENT_BALANCE')
            self.assertIn('三角币不足', self.overlay.lines[-1])

    def test_no_attempt_after_a_stop_while_the_backend_was_prepared(self):
        pool = Pool(on_take=lambda: self.stop.request())
        summary, calls = self.cycle([dict(status='passed', confirm_clicks=1)], pool=pool)
        self.assertEqual(calls, [])
        self.assertEqual(pool.discarded, 1)
        self.assertEqual(summary['status'], 'stopped')

    def test_attempts_leave_the_failure_banner_to_the_cycle(self):
        summary, calls = self.cycle([dict(status='passed', confirm_clicks=1)])
        self.assertIs(self.attempt_kw[0]['final_banner'], False)
        self.assertIs(self.attempt_kw[0]['stop_requested'], self.stop)

    def test_an_exception_in_an_attempt_or_collection_is_a_failure_not_a_crash(self):
        def boom():
            raise RuntimeError('COLLECTION_CAPTURE_WORKER_START')
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([boom, dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY'),
                                         dict(status='passed', confirm_clicks=1)],
                                        [RuntimeError('COLLECTION_SOMETHING')])
        self.assertEqual(summary['attempts'][0]['error'], 'COLLECTION_CAPTURE_WORKER_START')
        self.assertEqual(summary['collections'][0]['error'], 'COLLECTION_SOMETHING')
        self.assertEqual(calls, ['attempt', 'attempt', 'collect', 'attempt'])

    def test_a_full_disk_stops_the_cycle_before_the_next_attempt(self):
        summary, calls = self.cycle([dict(status='passed', confirm_clicks=1)], disk_free=lambda: 10 ** 8)
        self.assertEqual((calls, summary['error']), ([], 'CYCLE_DISK_FULL'))
        self.assertIn('磁盘', self.overlay.lines[-1])

    def test_cycle_record_is_written_after_every_attempt(self):
        def check():
            record = json.loads((self.root / 'cycle1' / 'cycle.json').read_text(encoding='utf-8'))
            self.assertEqual(len(record['attempts']), 1)
            return dict(status='passed', confirm_clicks=1)
        summary, calls = self.cycle([dict(status='passed', confirm_clicks=1), check])
        self.assertEqual(summary['run_directory'], str(self.root / 'cycle1'))

    def test_repeated_failures_stop_the_cycle(self):
        failing = [dict(status='blocked', error='PURCHASE_TIMED_HEAD_STATE:unknown')] * (MAX_FAILURES + 2)
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle(failing)
        self.assertEqual(calls.count('attempt'), MAX_FAILURES)
        self.assertEqual(summary['status'], 'blocked')
        self.assertTrue(summary['error'].startswith('CYCLE_REPEATED_FAILURES'))

    def test_still_empty_after_collecting_backs_off_whatever_was_collected(self):
        # Review 2026-10-09: a collection can add listings past their public
        # notice that the cleanup removes again; new_favorites is no guide.
        empty = dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY')
        after_cleanup = dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY_AFTER_CLEANUP')
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False) as idle:
            summary, calls = self.cycle([empty, after_cleanup, empty, empty, empty, empty,
                                         dict(status='passed', confirm_clicks=1)],
                                        [(0, dict(new_favorites=3))] * 6)
        self.assertEqual(calls.count('collect'), 6)
        self.assertEqual([c[0][0] for c in idle.call_args_list], [60, 120, 300, 900, 900])
        self.assertEqual([empty_backoff(n) for n in range(6)], [0, *EMPTY_BACKOFF_S, EMPTY_BACKOFF_S[-1]])

    def test_a_stop_inside_an_attempt_ends_the_cycle(self):
        summary, calls = self.cycle([dict(status='blocked', error='COLLECTION_STOP_REQUESTED'),
                                     dict(status='passed', confirm_clicks=1)])
        self.assertEqual(calls, ['attempt'])
        self.assertEqual(summary['attempts'][0]['action'], 'stopped')

    def test_a_stopped_collection_ends_the_cycle(self):
        summary, calls = self.cycle([dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY'),
                                     dict(status='passed', confirm_clicks=1)],
                                    [(3, dict(status='stopped'))])
        self.assertEqual(calls, ['attempt', 'collect'])

    def test_someone_at_the_mouse_or_window_stops_the_cycle_at_once(self):
        # Live buy_cycle02 2026-10-10: the collection stopped on
        # CURSOR_INTERFERENCE and the cycle went on to three refused attempts.
        empty = dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY')
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([empty, dict(status='passed', confirm_clicks=1)],
                                        [(1, dict(status='blocked', error='CURSOR_INTERFERENCE', new_favorites=4))])
        self.assertEqual(calls, ['attempt', 'collect'])
        self.assertEqual(summary['error'], 'CYCLE_USER_INTERVENED:CURSOR_INTERFERENCE')
        self.assertIn('鼠标、键盘或游戏窗口被操作，已停止', self.overlay.lines[-1])
        for error in ('PURCHASE_TIMED_POINTER_MOVED', 'PURCHASE_TIMED_NO_PRESS:POINTER_MOVED', 'BATCH_FOREGROUND_LOST',
                      'CURSOR_START_CHANGED'):
            self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
            with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
                summary, calls = self.cycle([dict(status='blocked', error=error), dict(status='passed', confirm_clicks=1)])
            with self.subTest(error=error):
                self.assertEqual(classify(dict(error=error)), 'user')
                self.assertEqual(calls, ['attempt'])
                self.assertEqual(summary['error'], 'CYCLE_USER_INTERVENED:' + error)
        # Our own pointer set not taking, or an unread page: retried, not a user.
        self.assertEqual(classify(dict(error='CURSOR_SET_NOT_APPLIED')), 'failed')
        self.assertEqual(classify(dict(error='PURCHASE_TIMED_WATCH_PAGE:unknown')), 'failed')
        # After a press: a session that lost the game, or the game not in front at the end.
        pressed = dict(status='blocked', error='PURCHASE_TIMED_OUTCOME:queue_full', confirm_clicks=1)
        self.assertEqual(classify(pressed), 'next')
        self.assertEqual(classify(dict(pressed, session_error='BATCH_FOREGROUND_LOST')), 'user')
        self.assertEqual(classify(dict(pressed, ide_restored=False)), 'user')
        self.assertEqual(classify(dict(pressed, error='PURCHASE_TIMED_CLOSE_UNVERIFIED:BATCH_FOREGROUND_LOST')), 'user')
        for error in ('COLLECTION_CURSOR_MOVED_BEFORE_INPUT', 'COLLECTION_USER_INPUT_ACTIVE', 'CURSOR_USER_CHECK_FAILED',
                      'COLLECTION_INPUT_GUARD_CHANGED', 'COLLECTION_TARGET_OCCLUDED', 'CURSOR_ENDPOINT_NOT_REACHED',
                      'PURCHASE_TIMED_DIALOG_WATCH:E_WINDOW_NOT_FOREGROUND', 'PURCHASE_TIMED_FOREGROUND_NOT_RETURNED'):
            with self.subTest(error=error):
                self.assertEqual(classify(dict(error=error)), 'user')
        # Any other failed collection is still retried.
        self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([empty, dict(status='passed', confirm_clicks=1)],
                                        [(1, dict(status='blocked', error='COLLECTION_TITLE_UNREAD'))])
        self.assertEqual(calls, ['attempt', 'collect', 'attempt'])

    def test_the_game_left_between_leases_stops_a_cycle_started_in_it(self):
        # Review wf_b0b8309e-39d: F2 pressed in the game, then the user
        # switches away during a wait: never pulled back.
        front = dict(game=True)
        self.game_in_front = lambda: front['game']
        waited, patch = self.advancing_idle()

        def fake_idle(seconds, stop, sleep=None):
            front['game'] = False              # the user alt-tabs during the wait
            return stop()
        with unittest.mock.patch.object(purchase_cycle, 'idle', side_effect=fake_idle):
            summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1)])
        self.assertEqual(calls, ['attempt'])
        self.assertEqual(summary['error'], 'CYCLE_USER_INTERVENED:GAME_NOT_IN_FRONT')
        self.assertTrue(summary['started_in_game'])
        # Every lease of such a cycle refuses to bring the game back (review wf_3215e495-f2d).
        self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
        self.game_in_front = lambda: True
        seen = []
        pool = Pool(on_take=lambda: seen.append(pool.require_game_in_front))
        summary, calls = self.cycle([dict(status='passed', confirm_clicks=1)], pool=pool)
        self.assertEqual(seen, [True])
        self.assertFalse(pool.require_game_in_front)
        # Started outside the game: every lease brings it to the front as before.
        self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
        self.game_in_front = lambda: False
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1)])
        self.assertEqual(calls, ['attempt', 'attempt'])

    def test_a_lease_that_never_bound_the_game_is_no_user(self):
        self.assertEqual(classify(dict(error='COLLECTION_WINDOW_IDENTITY', ide_restored=False, lease_entered=False)),
                         'failed')
        self.assertEqual(classify(dict(error='PURCHASE_TIMED_OUTCOME:queue_full', confirm_clicks=1, ide_restored=False,
                                       lease_entered=True)), 'user')
        # A read failure after a press whose outcome was read: the cycle goes on.
        self.assertEqual(classify(dict(status='blocked', error='PURCHASE_TIMED_SESSION_FAILED:BATCH_STEP_FAILED',
                                       confirm_clicks=1, ide_restored=True, session_error='BATCH_STEP_FAILED')), 'next')
        self.assertEqual(classify(dict(error='PURCHASE_TIMED_SESSION_FAILED:BATCH_FOREGROUND_LOST', confirm_clicks=1)),
                         'user')

    def test_final_banner(self):
        self.assertEqual(final_text(dict(status='stopped'))[0], '收藏+购买已停止')
        text, tone, ttl = final_text(dict(status='blocked', error='CYCLE_REPEATED_FAILURES:PURCHASE_TIMED_DIALOG_NOT_OPEN'))
        self.assertEqual((text, tone), ('连续 3 把没能完成，已停止：购买小窗没有打开', 'error'))

    def test_a_head_withdrawn_while_followed_is_no_failure(self):
        changed = dict(status='blocked', error='PURCHASE_TIMED_LISTING_CHANGED', price_button_clicks=0)
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False) as idle:
            summary, calls = self.cycle([changed] * (MAX_FAILURES + 1) + [dict(status='passed', confirm_clicks=1)])
        self.assertEqual(calls.count('attempt'), MAX_FAILURES + 2)
        self.assertNotIn('error', summary)
        idle.assert_not_called()
        # After an entry click it is an ordinary failure.
        self.assertEqual(classify(dict(changed, price_button_clicks=1)), 'failed')
        # A panel that switched to a listing past its notice: look again (the cleanup clears it).
        self.assertEqual(classify(dict(status='blocked', error='PURCHASE_TIMED_HEAD_SWITCHED:price_button')), 'retry')
        # A head that keeps "changing" (a misread) ends as failures, not an endless loop.
        self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([changed] * 40 + [dict(status='passed', confirm_clicks=1)])
        self.assertTrue(summary['error'].startswith('CYCLE_REPEATED_FAILURES'))
        self.assertEqual(calls.count('attempt'), MAX_FAILURES * (purchase_cycle.MAX_RETRIES + 1))

    def test_classify(self):
        self.assertEqual(classify(dict(error='PURCHASE_TIMED_TOO_EARLY:700')), 'failed')   # no head to wait for
        self.assertEqual(classify(dict(error='PURCHASE_TIMED_WRONG_DIALOG:confirm_dialog')), 'failed')
        self.assertEqual(classify(dict(error='x', interrupted=True)), 'stopped')
        self.assertEqual(classify(dict(status='passed')), 'next')


class FarHeadCollectsTests(unittest.TestCase):
    """User 2026-10-10 "这个要11分钟换一把时间短的重新收藏一点": a head that
    unlocks after the follow window sends the cycle collecting once first."""
    far_head_collection = True
    setUp, cycle, advancing_idle, far = CycleTests.setUp, CycleTests.cycle, CycleTests.advancing_idle, CycleTests.far

    def test_a_far_head_collects_once_then_waits_for_the_head(self):
        far = self.far()
        seconds, zero = wait_seconds(far, 600, self.now[0])
        waited, patch = self.advancing_idle()
        with patch:
            summary, calls = self.cycle([far, self.far(), dict(status='passed', confirm_clicks=1)],
                                        [(0, dict(status='completed', new_favorites=2))])
        self.assertEqual(calls, ['attempt', 'collect', 'attempt', 'attempt'])
        limited = self.collect_stops[0]
        self.assertIsInstance(limited, purchase_cycle.DeadlineStop)
        self.assertEqual(limited.deadline_ms, zero - purchase_cycle.COLLECT_BACK_BEFORE_S * 1000.0)
        self.assertTrue(any('先收藏一轮' in line for line in self.overlay.lines))
        self.assertTrue(summary['collections'][0]['before_far_head'])
        self.assertAlmostEqual(sum(waited), seconds, delta=1)      # the second far reading is waited for
        self.assertEqual([a['action'] for a in summary['attempts']], ['wait', 'wait', 'next'])

    def test_a_collection_ends_before_the_head_unlocks_and_the_cycle_goes_on(self):
        far = self.far()
        seconds, zero = wait_seconds(far, 600, self.now[0])

        def long_collection(stop):
            self.assertFalse(stop())
            self.now[0] = zero - purchase_cycle.COLLECT_BACK_BEFORE_S * 1000.0   # at the deadline
            self.assertTrue(stop())
            self.assertEqual(stop.reason, 'collection_deadline')
            return 3, dict(status='stopped', error='COLLECTION_STOP_REQUESTED')
        summary, calls = self.cycle([far, dict(status='passed', confirm_clicks=1)], [long_collection])
        self.assertEqual(calls, ['attempt', 'collect', 'attempt'])
        self.assertTrue(any('收藏到点了' in line for line in self.overlay.lines))
        self.assertNotIn('error', summary)
        # The cycle's own stop still ends a collection, and the cycle with it.
        self.stop, self.root, self.now = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name)), [1_000_000.0]

        def stopped(stop):
            self.stop.request()
            self.assertTrue(stop())
            self.assertEqual(stop.reason, 'hotkey')
            return 3, dict(status='stopped', error='COLLECTION_STOP_REQUESTED')
        summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1)], [stopped])
        self.assertEqual((calls, summary['status']), (['attempt', 'collect'], 'stopped'))

    def test_each_new_far_head_after_a_press_collects_again_and_failures_still_count(self):
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1), self.far(),
                                         dict(status='passed', confirm_clicks=1)],
                                        [(0, dict(status='completed'))] * 2)
        self.assertEqual(calls.count('collect'), 2)
        self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
        broken = (1, dict(status='blocked', error='COLLECTION_TITLE_UNREAD'))
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([self.far(), self.far(), dict(status='passed', confirm_clicks=1)], [broken])
        self.assertEqual(calls, ['attempt', 'collect', 'attempt', 'attempt'])
        self.assertNotIn('error', summary)
        # Someone at the mouse during that collection: the cycle stops.
        self.stop, self.root = Stop(), Path(tempfile.mkdtemp(dir=self.folder.name))
        summary, calls = self.cycle([self.far(), dict(status='passed', confirm_clicks=1)],
                                    [(1, dict(status='blocked', error='CURSOR_INTERFERENCE'))])
        self.assertEqual(calls, ['attempt', 'collect'])
        self.assertEqual(summary['error'], 'CYCLE_USER_INTERVENED:CURSOR_INTERFERENCE')

    def test_a_far_head_right_after_a_full_collection_is_waited_for(self):
        # Review wf_993b38d0-6b8: a second pass from rule 1 started at once.
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY'), self.far(),
                                         dict(status='passed', confirm_clicks=1)],
                                        [(0, dict(status='completed', new_favorites=3))])
        self.assertEqual(calls, ['attempt', 'collect', 'attempt', 'attempt'])
        self.assertFalse(summary['collections'][0]['before_far_head'])

    def test_a_deadline_stop_is_recorded_as_such(self):
        far = self.far()
        seconds, zero = wait_seconds(far, 600, self.now[0])

        def long_collection(stop):
            self.now[0] = zero
            stop()
            return 3, dict(status='stopped_by_user', error='COLLECTION_STOP_REQUESTED')
        summary, calls = self.cycle([far, dict(status='passed', confirm_clicks=1)], [long_collection])
        record = summary['collections'][0]
        self.assertEqual((record['stop_reason'], record['deadline_expired']), ('collection_deadline', True))

    def test_a_failed_attempt_after_a_full_pass_does_not_start_another(self):
        # Review wf_b0b8309e-39d: only a press clears a completed pass.
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([dict(status='blocked', error='PURCHASE_TIMED_WATCHLIST_EMPTY'),
                                         dict(status='blocked', error='PURCHASE_TIMED_HEAD_STATE:unknown'), self.far(),
                                         dict(status='passed', confirm_clicks=1)],
                                        [(0, dict(status='completed'))] * 2)
        self.assertEqual(calls.count('collect'), 1)

    def test_no_collection_when_the_head_is_too_close_for_one(self):
        near = dict(status='blocked', error='PURCHASE_TIMED_TOO_EARLY:170',
                    head=dict(state='counting', countdown_seconds=170, frame_qpc_ms=self.now[0]))
        with unittest.mock.patch.object(purchase_cycle, 'idle', return_value=False):
            summary, calls = self.cycle([near, dict(status='passed', confirm_clicks=1)])
        self.assertEqual(calls, ['attempt', 'attempt'])


import unittest.mock  # noqa: E402

if __name__ == '__main__':
    unittest.main()
