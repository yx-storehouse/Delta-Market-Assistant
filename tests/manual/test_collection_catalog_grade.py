"""Catalogue filter ticks 已拥有 + 未拥有 and the skin's 品阶; Back is the Esc key.

User 2026-10-09: the filter should select the skin's own quality tier and both
ownership boxes, and the Back button should be pressed with the keyboard.
Pure fakes: no game, no input.
"""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import collection_live_session as live
from run_collection_trial import CollectionTrial, GRADE_BY_LABEL
from test_collection_live_session import Clock, FakeBackend, capture, snapshot as session_snapshot
from test_collection_trial import FakeSession, snapshot

KEYS = ('owned', 'unowned', 'legendary', 'epic', 'rare', 'common')


def purple_row(**extra):
    return dict(dict(snapshot()['rows'][0], product_id='10602', game_grade='史诗品阶'), **extra)


class FilterHarness:
    """A trial whose capture/click act on one simulated catalogue-filter dialog."""

    def __init__(self, test, *, start=None, page='skin_home'):
        self.test = test
        self.session = FakeSession()
        self.session.previous = {'startup_page': {'page': page}}
        self.events, self.clicks, self.keys, self.captures = [], [], [], []
        self.serial = 0
        self.current = page  # what the fake game shows when no page is asked for
        # Per-read overrides of what the game shows, e.g. a box under the
        # pointer's glow reading unknown: [{'owned': 'unknown'}, ...].
        self.reads = []
        self.state = {'complete': True, 'season_label': '棱镜攻势 S2', 'checkboxes': {
            key: {'state': (start or {}).get(key, 'unchecked'), 'bounds': [600 + 250 * index, 540, 30, 30]}
            for index, key in enumerate(KEYS)}}
        self.trial = CollectionTrial(self.session, snapshot(), 'unused.json', emit=self.events.append)
        self.trial.capture = self.capture
        self.trial.click = self.click
        self.trial.back = self.back
        self.hovers = []
        self.trial.hover = lambda point, page=None: self.hovers.append((list(point), len(self.clicks)))
        self.trial.click_label = lambda region, label: None
        self.trial.find_season = lambda target: None

    def capture(self, page=None, **options):
        self.captures.append(dict(page=page, **options))
        self.serial += 1
        digest = format(self.serial, '064x')
        state = copy.deepcopy(self.state)
        for key, value in (self.reads.pop(0) if self.reads else {}).items():
            state['checkboxes'][key]['state'] = value
        state.update(valid_page=True, same_frame=True, frame_id='fake:%d' % self.serial, frame_sha256=digest,
                     complete=all(box['state'] in ('checked', 'unchecked') for box in state['checkboxes'].values()))
        self.session.previous = {'startup_page': {'page': page or self.current},
                                 'catalog_filter_state': state, 'frames': [dict(sha256=digest)]}

    def click(self, point, *args, **kwargs):
        self.clicks.append(list(point))
        if list(point) == [340, 274]:
            self.current = 'catalog_filter'
        elif list(point) == [1280, 1008]:
            self.current = 'skin_home'
        for key, box in self.state['checkboxes'].items():
            x, y, width, height = box['bounds']
            if point == [x + width // 2, y + height // 2]:
                box['state'] = 'checked' if box['state'] == 'unchecked' else 'unchecked'

    def back(self):
        self.keys.append(self.trial.page())
        self.current = 'skin_home'
        self.session.previous = {}

    def states(self):
        return {key: box['state'] for key, box in self.state['checkboxes'].items()}

    def toggled(self):
        return [key for key, box in self.state['checkboxes'].items()
                if [box['bounds'][0] + 15, box['bounds'][1] + 15] in self.clicks]


class CatalogGradeFilterTests(unittest.TestCase):
    def test_purple_skin_ticks_both_ownerships_and_epic_only(self):
        h = FilterHarness(self)
        h.trial.filter_for(purple_row())
        self.assertEqual(h.states(), dict(owned='checked', unowned='checked', legendary='unchecked',
                                          epic='checked', rare='unchecked', common='unchecked'))
        # One readback after each tick where the pointer rests, then a strict
        # readback of all six with the pointer resting on 确认.
        self.assertEqual(len([c for c in h.captures if c['page'] == 'catalog_filter']), 1 + 3 + 1)
        self.assertFalse(any(c.get('neutral_pointer') or 'expected_filter' in c for c in h.captures))
        self.assertEqual(h.hovers, [([1280, 905], 4)])  # after the 3 ticks, before 确认
        ready = next(e for e in h.events if e['event'] == 'catalog_filter_ready')
        self.assertEqual((ready['ownership'], ready['game_grade'], ready['game_grade_label'], ready['game_grade_source']),
                         ('owned+unowned', 'epic', '史诗品阶', 'catalogue_grade'))
        self.assertEqual(h.trial.active_grade, 'epic')
        self.assertEqual(h.clicks[-1], [1280, 1008])  # 确认

    def test_residual_grades_are_cleared_and_a_correct_dialog_is_left_alone(self):
        h = FilterHarness(self, start=dict(owned='checked', legendary='checked', rare='checked'))
        h.trial.filter_for(purple_row())
        self.assertEqual(h.toggled(), ['unowned', 'legendary', 'epic', 'rare'])
        self.assertEqual(h.states()['epic'], 'checked')
        ready = dict(owned='checked', unowned='checked', epic='checked')
        h = FilterHarness(self, start=ready)
        h.trial.filter_for(purple_row())
        self.assertEqual(h.clicks, [[340, 274], [1280, 1008]])  # 筛选, 确认
        self.assertFalse(any('expected_filter' in c for c in h.captures))

    def test_unknown_or_missing_colour_filters_all_grades(self):
        for row in (dict(snapshot()['rows'][0]), purple_row(game_grade='')):
            h = FilterHarness(self, start=dict(epic='checked'))
            h.trial.filter_for(row)
            self.assertEqual(h.states(), dict(owned='checked', unowned='checked', legendary='unchecked',
                                              epic='unchecked', rare='unchecked', common='unchecked'))
            self.assertIsNone(h.trial.active_grade)

    def test_an_explicit_task_grade_wins_over_the_colour(self):
        h = FilterHarness(self)
        h.trial.filter_for(purple_row(grade='legendary'))
        self.assertEqual(h.states()['legendary'], 'checked')
        self.assertEqual(h.states()['epic'], 'unchecked')

    def test_a_box_that_stays_unreadable_reopens_once_then_stops_without_ticking(self):
        h = FilterHarness(self)
        h.state['checkboxes']['epic']['state'] = 'unknown'
        with self.assertRaisesRegex(RuntimeError, '^COLLECTION_FILTER_STATE_INCOMPLETE:epic$'):
            h.trial.filter_for(purple_row())
        # 筛选 twice (one reopen via Esc), never a box and never 确认.
        self.assertEqual(h.clicks, [[340, 274], [340, 274]])
        self.assertEqual(h.keys, ['catalog_filter'])
        self.assertEqual([e['reason'] for e in h.events if e['event'] == 'catalog_filter_reopened'],
                         ['COLLECTION_FILTER_STATE_INCOMPLETE:epic'])
        self.assertEqual(h.hovers, [([1280, 905], 1), ([1280, 905], 2)])  # off the boxes before re-reading

    def test_a_glowing_box_is_read_again_with_the_pointer_on_confirm(self):
        # The season option leaves the pointer over 已拥有 (live 20261009-141700).
        h = FilterHarness(self, start=dict(owned='checked', unowned='checked', epic='checked'))
        h.state['season_label'] = '疾光魅影'
        selected = []
        def choose(region, label):
            selected.append(label)
            h.state['season_label'] = label
            h.reads.append({'owned': 'unknown'})  # this readback still glows
        h.trial.click_label = choose
        h.trial.filter_for(purple_row())
        self.assertEqual(len(selected), 1)
        self.assertEqual(h.clicks, [[340, 274], [1066, 446], [1280, 1008]])  # 筛选, season, 确认
        self.assertEqual(h.hovers, [([1280, 905], 2)])
        self.assertFalse(any(e['event'] == 'catalog_filter_reopened' for e in h.events))

    def test_a_tick_that_reads_unknown_twice_reopens_and_finishes_from_a_fresh_read(self):
        h = FilterHarness(self)
        h.reads = [{}, {'owned': 'unknown', 'unowned': 'unknown'}, {'owned': 'unknown', 'unowned': 'unknown'}]
        h.trial.filter_for(purple_row())
        self.assertEqual([e['reason'] for e in h.events if e['event'] == 'catalog_filter_reopened'],
                         ['COLLECTION_FILTER_READBACK_UNSETTLED:owned'])
        self.assertEqual(h.states(), dict(owned='checked', unowned='checked', legendary='unchecked',
                                          epic='checked', rare='unchecked', common='unchecked'))
        self.assertEqual(h.clicks[-1], [1280, 1008])
        self.assertEqual(h.keys, ['catalog_filter'])

    def test_each_catalogue_grade_ticks_its_box(self):
        for label, grade in (('传说品阶', 'legendary'), ('史诗品阶', 'epic'), ('稀有品阶', 'rare')):
            h = FilterHarness(self, start=dict(epic='checked'))
            h.trial.filter_for(purple_row(game_grade=label))
            self.assertEqual(h.trial.active_grade, grade)
            self.assertEqual({key: h.states()[key] for key in ('legendary', 'epic', 'rare', 'common')},
                             {key: 'checked' if key == grade else 'unchecked'
                              for key in ('legendary', 'epic', 'rare', 'common')})
        self.assertNotIn('common', GRADE_BY_LABEL.values())  # 普通品阶 is never collected
        h = FilterHarness(self)
        h.trial.filter_for(purple_row(game_grade='purple'))  # never a colour
        self.assertIsNone(h.trial.active_grade)

    def test_a_click_that_changes_another_box_is_reopened_and_redone_or_stops(self):
        # 已拥有 unticks 未拥有 in this fake game: the readback after that click
        # fails, the dialog is reopened, and the fresh read finishes the job.
        h = FilterHarness(self)
        original = h.click
        def linked(point, *args, **kwargs):
            original(point, *args, **kwargs)
            if point == [615, 555]:
                h.state['checkboxes']['unowned']['state'] = 'unchecked'
        h.trial.click = linked
        h.state['checkboxes']['unowned']['state'] = 'checked'
        h.trial.filter_for(purple_row())
        self.assertEqual([e['reason'] for e in h.events if e['event'] == 'catalog_filter_reopened'],
                         ['COLLECTION_FILTER_READBACK_MISMATCH:owned'])
        self.assertEqual(h.states(), dict(owned='checked', unowned='checked', legendary='unchecked',
                                          epic='checked', rare='unchecked', common='unchecked'))
        self.assertEqual(h.clicks[-1], [1280, 1008])
        # When the two boxes always exclude each other the second attempt fails
        # the same way and the run stops; 确认 is never clicked.
        h = FilterHarness(self)
        original = h.click
        def exclusive(point, *args, **kwargs):
            original(point, *args, **kwargs)
            other = {(615, 555): 'unowned', (865, 555): 'owned'}.get(tuple(point))
            if other:
                h.state['checkboxes'][other]['state'] = 'unchecked'
        h.trial.click = exclusive
        with self.assertRaisesRegex(RuntimeError, '^COLLECTION_FILTER_READBACK_MISMATCH:(owned|unowned)$'):
            h.trial.filter_for(purple_row())
        self.assertEqual(h.keys, ['catalog_filter'])
        self.assertNotIn([1280, 1008], h.clicks)


class OpenProductGradeTests(unittest.TestCase):
    def harness(self, found):
        h = FilterHarness(self)
        searches = []
        def find_product(name):
            searches.append(h.trial.active_grade)
            if not found(len(searches)):
                raise RuntimeError('COLLECTION_PRODUCT_NOT_FOUND_IN_OBSERVED_CATALOG:' + name)
        h.trial.find_product = find_product
        return h, searches

    def test_product_missing_under_its_grade_is_searched_again_under_all_grades(self):
        h, searches = self.harness(lambda attempt: attempt == 2)
        row = purple_row()
        try:
            h.trial.open_product(row)
        except RuntimeError as error:
            # The fake title readback is not part of this test.
            self.assertRegex(str(error), '^COLLECTION_CATALOG_TITLE_NOT_CONFIRMED')
        self.assertEqual(searches, ['epic', None])
        fallback = next(e for e in h.events if e['event'] == 'catalog_grade_filter_fallback')
        self.assertEqual((fallback['game_grade'], fallback['catalogue_grade']), ('epic', '史诗品阶'))
        self.assertIsNone(h.trial.grade_for(row))
        self.assertEqual(h.states()['epic'], 'unchecked')
        ready = [e for e in h.events if e['event'] == 'catalog_filter_ready']
        self.assertEqual([e['game_grade_source'] for e in ready], ['catalogue_grade', 'fallback_all_grades'])

    def test_an_empty_list_under_the_grade_also_falls_back(self):
        h = FilterHarness(self)
        empty = [dict(kind='catalog_names', ok=False, error='LOCAL_CATALOG_EMPTY', words=[], truncated=False),
                 dict(kind='catalog_names', ok=True, error='', words=[], truncated=False)]
        for region in empty:
            h = FilterHarness(self)
            captures = h.capture
            def capture(page=None, **options):
                captures(page, **options)
                if page == 'skin_home' and h.trial.active_grade is not None:
                    h.session.previous['collection_observation'] = dict(regions=[region])
            h.trial.capture = capture
            with self.assertRaises(RuntimeError) as failure:
                h.trial.open_product(purple_row())
            # Under all grades the fake catalogue is unobserved; the point is the fallback.
            self.assertEqual(str(failure.exception), 'COLLECTION_CATALOG_NAMES_UNOBSERVED')
            self.assertTrue(any(e['event'] == 'catalog_grade_filter_fallback' for e in h.events))
            self.assertIsNone(h.trial.active_grade)

    def test_fallbacks_are_shared_by_the_runs_segments(self):
        shared = set()
        first = CollectionTrial(FakeSession(), snapshot(), 'unused.json', grade_fallbacks=shared)
        first.grade_fallbacks.add('10602')
        second = CollectionTrial(FakeSession(), snapshot(), 'unused.json', grade_fallbacks=shared)
        self.assertIsNone(second.grade_for(purple_row()))
        self.assertEqual(second.summary['catalog_filter_policy']['catalogue_grades'],
                         {'传说品阶': 'legendary', '史诗品阶': 'epic', '稀有品阶': 'rare'})

    def test_missing_under_all_grades_still_stops(self):
        h, searches = self.harness(lambda attempt: False)
        with self.assertRaisesRegex(RuntimeError, '^COLLECTION_PRODUCT_NOT_FOUND_IN_OBSERVED_CATALOG'):
            h.trial.open_product(purple_row())
        self.assertEqual(searches, ['epic', None])
        h, searches = self.harness(lambda attempt: False)
        with self.assertRaisesRegex(RuntimeError, '^COLLECTION_PRODUCT_NOT_FOUND_IN_OBSERVED_CATALOG'):
            h.trial.open_product(dict(snapshot()['rows'][0]))
        self.assertEqual(searches, [None])

    def test_a_different_grade_refilters_and_listing_back_uses_the_key(self):
        h, _ = self.harness(lambda attempt: True)
        h.trial.active_season = '棱镜攻势S2'
        h.trial.active_grade = 'rare'
        h.session.previous = {'startup_page': {'page': 'skin_listings'}}
        with self.assertRaises(RuntimeError):
            h.trial.open_product(purple_row())
        self.assertEqual(h.keys, ['skin_listings'])
        self.assertTrue(any(e['event'] == 'catalog_filter_ready' for e in h.events))


class TrialBackKeyTests(unittest.TestCase):
    def test_back_is_one_escape_step_bound_to_the_current_page(self):
        session = FakeSession()
        steps = []
        session.perform = lambda step: steps.append(copy.deepcopy(step)) or dict(passed=True)
        trial = CollectionTrial(session, snapshot(), 'unused.json')
        trial.back()
        self.assertEqual(steps, [dict(kind='key', key='escape', expected_before='skin_listings')])


class KeyBackend(FakeBackend):
    def key(self, virtual_key, before_dispatch=None):
        if before_dispatch is not None:
            before_dispatch()
        self.events.append(('key', virtual_key))
        self.last_dispatch = dict(api='SendInput', kind='key', virtual_key=virtual_key, returned_events=self.sent)
        return self.sent


class SessionKeyStepTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        path = self.root / 'artifacts/trial/input/snapshot.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(session_snapshot()), encoding='utf-8')
        self.clock = Clock()
        self.backend = KeyBackend(self.clock)
        self.runs = 0

    def session(self):
        self.runs += 1
        return live.ForegroundSession('artifacts/trial/run%d.json' % self.runs, root=self.root, backend=self.backend,
                                      now=self.clock.now, wait=self.clock.wait)

    def test_escape_is_sent_once_and_the_page_must_be_read_again(self):
        session = self.session()
        with session:
            session.perform(capture())
            step = session.perform(dict(kind='key', key='escape', expected_before='skin_listings'))
            self.assertEqual(session.previous, {})
            with self.assertRaisesRegex(RuntimeError, 'COLLECTION_PAGE'):
                session.perform(dict(kind='key', key='escape', expected_before='skin_listings'))
        self.assertEqual([e for e in self.backend.events if isinstance(e, tuple) and e[0] == 'key'], [('key', 0x1B)])
        self.assertIsNone(step['point'])
        self.assertEqual(step['input_dispatch']['kind'], 'key')
        self.assertEqual(session.report['key_presses'], 1)
        self.assertEqual(session.report['manual_clicks'], 0)

    def test_wrong_page_other_keys_and_a_dropped_event_are_refused(self):
        for step, error in ((dict(kind='key', key='escape', expected_before='skin_home'), 'COLLECTION_PAGE'),
                            (dict(kind='key', key='enter', expected_before='skin_listings'), 'COLLECTION_KEY')):
            self.backend = KeyBackend(self.clock)
            session = self.session()
            with self.assertRaisesRegex(RuntimeError, error):
                with session:
                    session.perform(capture())
                    session.perform(step)
            self.assertFalse(any(isinstance(e, tuple) and e[0] == 'key' for e in self.backend.events))
        self.backend = KeyBackend(self.clock)
        self.backend.sent = 1
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_INPUT_UNCERTAIN'):
            with session:
                session.perform(capture())
                session.perform(dict(kind='key', key='escape', expected_before='skin_listings'))

    def test_real_backend_refuses_any_key_but_escape_before_input(self):
        backend = live.WindowsBackend.__new__(live.WindowsBackend)
        with self.assertRaisesRegex(RuntimeError, '^COLLECTION_KEY$'):
            backend.key(0x0D)


class FakeUser32:
    def __init__(self, cursor_path=None, foreground_path=None):
        self.cursor = list(cursor_path or [(500, 500)])
        self.front = list(foreground_path or [True])
    def GetCursorPos(self, pointer):
        x, y = self.cursor.pop(0) if len(self.cursor) > 1 else self.cursor[0]
        pointer._obj.x, pointer._obj.y = x, y
        return 1
    def GetForegroundWindow(self):
        front = self.front.pop(0) if len(self.front) > 1 else self.front[0]
        return 77 if front else 5
    def GetAsyncKeyState(self, key):
        return 0


class RealKeyDispatchTests(unittest.TestCase):
    """WindowsBackend.key with SendInput replaced: event order, hold, cleanup."""

    def setUp(self):
        # These are fake-API sequencing tests, not a Windows timer benchmark.
        # The real coarse monotonic clock occasionally reports a 16 ms delta
        # around the requested 30 ms sleep. Inject both clock and sleep so the
        # test proves the requested hold and release order deterministically.
        from unittest.mock import patch
        self.clock = Clock()
        for target, callback in (('monotonic', self.clock.now), ('sleep', self.clock.wait)):
            mocked = patch.object(live.time, target, side_effect=callback)
            mocked.start()
            self.addCleanup(mocked.stop)

    def backend(self, user32, results=None):
        import ctypes
        from ctypes import wintypes
        from navigate_lobby_to_warehouse import Input
        backend = live.WindowsBackend.__new__(live.WindowsBackend)
        backend.c, backend.w, backend.Input, backend.u = ctypes, wintypes, Input, user32
        backend.identities = dict(target_hwnd=77)
        KeyboardInput = backend._keyboard.__func__(SimpleBackend(ctypes, wintypes, Input))[0]
        self.sent = []
        results = list(results or [])
        def send(count, item, size):
            record = ctypes.cast(item, ctypes.POINTER(KeyboardInput)).contents
            self.sent.append((count, record.type, record.data.ki.wVk, record.data.ki.wScan, record.data.ki.dwFlags, size))
            return results.pop(0) if results else 1
        backend._keyboard_api = (KeyboardInput, send, lambda vk, kind: 1)
        return backend

    def test_escape_is_one_press_held_then_released(self):
        backend = self.backend(FakeUser32())
        self.assertEqual(backend.key(0x1B), 2)
        self.assertEqual([entry[:5] for entry in self.sent], [(1, 1, 0x1B, 1, 0), (1, 1, 0x1B, 1, 2)])
        self.assertEqual(self.sent[0][5], 40)
        self.assertAlmostEqual(backend.last_dispatch['observed_hold_ms'], 30)
        self.assertIn(.03, self.clock.waits)
        self.assertEqual((backend.last_dispatch['press_events'], backend.last_dispatch['release_events']), (1, 1))

    def test_a_moving_pointer_stops_before_any_key_event(self):
        backend = self.backend(FakeUser32(cursor_path=[(500, 500), (512, 530)]))
        with self.assertRaisesRegex(RuntimeError, '^CURSOR_INTERFERENCE$'):
            backend.key(0x1B)
        self.assertEqual(self.sent, [])

    def test_foreground_lost_during_hold_still_releases_once(self):
        # foreground(): before guard, after guard, after the hold (lost).
        backend = self.backend(FakeUser32(foreground_path=[True, True, False]))
        with self.assertRaisesRegex(RuntimeError, '^BATCH_FOREGROUND_LOST$'):
            backend.key(0x1B)
        self.assertEqual([entry[4] for entry in self.sent], [0, 2])

    def test_a_dropped_release_is_retried_as_cleanup_and_reported(self):
        backend = self.backend(FakeUser32(), results=[1, 0, 1])
        self.assertEqual(backend.key(0x1B), 1)
        self.assertEqual([entry[4] for entry in self.sent], [0, 2, 2])
        self.assertEqual(backend.last_dispatch['release_cleanup_events'], 1)


class SimpleBackend:
    def __init__(self, c, w, Input):
        self.c, self.w, self.Input = c, w, Input


if __name__ == '__main__':
    unittest.main()


FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures'


class FilterHoverReadbackTests(unittest.TestCase):
    """Live 09:56: the ticked 已拥有 read 'unknown' under the resting pointer."""

    @classmethod
    def setUpClass(cls):
        cls.recorded = json.loads((FIXTURES / 'hotkey_filter_hover.json').read_text('utf-8'))

    def frame(self, tag, **states):
        packet = copy.deepcopy(self.recorded['hovered'])
        digest = (tag * 64)[:64]
        packet['frames'][-1]['sha256'] = digest
        state = packet['catalog_filter_state']
        state.update(frame_sha256=digest, frame_id='dxgi:filter-' + tag)
        for key, value in states.items():
            state['checkboxes'][key]['state'] = value
        state['complete'] = all(box['state'] != 'unknown' for box in state['checkboxes'].values())
        return packet

    def run_capture(self, replies):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = Clock()
        backend = FakeBackend(clock, replies)
        session = live.ForegroundSession('artifacts/filter/run.json', root=Path(temporary.name), backend=backend,
                                         now=clock.now, wait=clock.wait)
        session.enter()
        self.addCleanup(session.finish)
        step = dict(kind='capture', expected_page='catalog_filter', filter_state=True,
                    expected_filter=dict(self.recorded['expected']), neutral_pointer=True)
        session.steps.append(dict(kind='capture', step=step))
        session._capture_attempts = []
        return backend, session._capture(step, 30)

    def test_recorded_ambiguous_tick_is_read_again_and_then_passes(self):
        self.assertEqual(self.recorded['hovered']['catalog_filter_state']['checkboxes']['owned']['state'], 'unknown')
        self.assertTrue(live.filter_readback_unsettled(self.recorded['hovered'], self.recorded['expected']))
        backend, observed = self.run_capture([self.frame('1'), self.frame('2', owned='checked')])
        self.assertTrue(observed['passed'])
        self.assertEqual(observed['attempt_count'], 2)
        self.assertTrue(observed['attempts'][0]['catalog_filter_retryable'])
        self.assertIn('neutral', backend.events)
        self.assertFalse(any(isinstance(e, tuple) and e[0] == 'click' for e in backend.events))

    def test_an_opposite_or_still_unknown_box_stops(self):
        _, observed = self.run_capture([self.frame('1', owned='unchecked')])
        self.assertFalse(observed['passed'])
        self.assertEqual(observed['attempt_count'], 1)
        self.assertFalse(live.filter_readback_unsettled(self.frame('3', owned='checked', epic='unchecked'),
                                                         self.recorded['expected']))
        _, observed = self.run_capture([self.frame('1'), self.frame('2'), self.frame('4')])
        self.assertFalse(observed['passed'])
        self.assertEqual(observed['attempt_count'], 3)

    def test_the_pointer_goes_box_to_box_to_confirm_without_return_trips(self):
        h = FilterHarness(self)
        h.trial.filter_for(purple_row())
        self.assertEqual(h.clicks, [[340, 274], [615, 555], [865, 555], [1365, 555], [1280, 1008]])
        self.assertFalse(any(c.get('neutral_pointer') for c in h.captures))
        h = FilterHarness(self, start=dict(owned='checked', unowned='checked', epic='checked'))
        h.trial.filter_for(purple_row())
        self.assertEqual(h.hovers, [])  # nothing ticked: no extra pointer motion

    def test_a_readback_under_the_pointer_tolerates_only_that_boxs_glow(self):
        hovered = self.recorded['hovered']
        expected = self.recorded['expected']
        self.assertTrue(live.filter_matches_under_pointer(hovered, expected, 'owned'))
        self.assertFalse(live.filter_matches_under_pointer(hovered, expected, 'unowned'))
        self.assertFalse(live.filter_expectation_matches(hovered, expected))
        self.assertTrue(live.filter_matches_under_pointer(self.frame('5', owned='checked'), expected, 'owned'))
        self.assertFalse(live.filter_matches_under_pointer(self.frame('6', owned='unchecked'), expected, 'owned'))
        self.assertFalse(live.filter_matches_under_pointer(self.frame('7', epic='unknown'), expected, 'owned'))


class FailedDispatchEvidenceTests(unittest.TestCase):
    def test_a_half_sent_key_is_kept_in_the_failed_step(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        path = root / 'artifacts/trial/input/snapshot.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(session_snapshot()), encoding='utf-8')
        clock = Clock()
        backend = KeyBackend(clock)
        backend.sent = 1
        backend.last_dispatch = dict(kind='stale-from-earlier')
        session = live.ForegroundSession('artifacts/trial/run.json', root=root, backend=backend,
                                         now=clock.now, wait=clock.wait)
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_INPUT_UNCERTAIN'):
            with session:
                session.perform(capture())
                session.perform(dict(kind='key', key='escape', expected_before='skin_listings'))
        failed = session.steps[-1]
        self.assertEqual(failed['status'], 'failed')
        self.assertEqual(failed['input_dispatch'], dict(api='SendInput', kind='key', virtual_key=0x1B, returned_events=1))

    def test_a_preflight_failure_records_no_older_input(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        path = root / 'artifacts/trial/input/snapshot.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(session_snapshot()), encoding='utf-8')
        clock = Clock()
        backend = KeyBackend(clock)
        backend.last_dispatch = dict(kind='stale-from-earlier')
        session = live.ForegroundSession('artifacts/trial/run.json', root=root, backend=backend,
                                         now=clock.now, wait=clock.wait)
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_PAGE'):
            with session:
                session.perform(capture())
                session.perform(dict(kind='key', key='escape', expected_before='skin_home'))
        self.assertNotIn('input_dispatch', session.steps[-1])


class NoReturnTripTests(unittest.TestCase):
    """User 2026-10-09: the pointer must not go back to a neutral spot."""

    def test_session_readback_under_the_pointer_passes_without_moving_it(self):
        recorded = json.loads((FIXTURES / 'hotkey_filter_hover.json').read_text('utf-8'))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = Clock()
        backend = FakeBackend(clock, [copy.deepcopy(recorded['hovered'])])
        session = live.ForegroundSession('artifacts/filter/run.json', root=Path(temporary.name), backend=backend,
                                         now=clock.now, wait=clock.wait)
        session.enter()
        self.addCleanup(session.finish)
        events_after_entry = len(backend.events)
        step = dict(kind='capture', expected_page='catalog_filter', filter_state=True,
                    expected_filter=dict(recorded['expected']), filter_pointer_key='owned')
        session.steps.append(dict(kind='capture', step=step))
        session._capture_attempts = []
        observed = session._capture(step, 30)
        self.assertTrue(observed['passed'])
        self.assertEqual(observed['attempt_count'], 1)
        self.assertNotIn('neutral', backend.events[events_after_entry:])

    def test_hotkey_started_in_the_game_leaves_the_pointer_in_place(self):
        for stay, neutral in ((True, 0), (False, 1)):
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            clock = Clock()
            backend = FakeBackend(clock)
            backend.stay_in_target = stay
            session = live.ForegroundSession('artifacts/entry/run.json', root=Path(temporary.name), backend=backend,
                                             now=clock.now, wait=clock.wait)
            with session:
                session.perform(capture())
            self.assertEqual(backend.events.count('neutral'), neutral)
            self.assertEqual(session.report['entry_neutral_policy'],
                             'skipped_started_in_target_pointer_left_in_place' if stay
                             else 'after_settle_checked_foreground_and_viewport')

    def test_hover_settles_briefly_not_like_navigation(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = Clock()
        backend = FakeBackend(clock)
        session = live.ForegroundSession('artifacts/hover/run.json', root=Path(temporary.name), backend=backend,
                                         now=clock.now, wait=clock.wait)
        with session:
            session.perform(capture())
            step = session.perform(dict(kind='hover', point=[1280, 1008], viewport=[2560, 1440],
                                        expected_before='skin_listings'))
        self.assertEqual(step['timings']['settle_wait_ms'], 100)
        self.assertIn(('hover', [1280, 1008]), backend.events)
