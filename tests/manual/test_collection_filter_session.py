"""filter_for through the REAL ForegroundSession against a scripted filter dialog.

Review 2026-10-09 (adapted from its scratch simulation): the fake trial harness
could not show that a mismatched expected page ends the lease, that one Esc may
only close the season dropdown, or that the season readback had no recovery.
This dialog model keeps the session's page checks real. No game, no input.
"""
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

import collection_live_session as live
from run_collection_trial import CollectionTrial, FilterUnsettled, PARK_POINT
from test_collection_trial import snapshot
from test_collection_live_session import Clock


KEYS = ('owned', 'unowned', 'legendary', 'epic', 'rare', 'common')
LOC = dict(owned=(754, 534), unowned=(1073, 534), legendary=(754, 630), epic=(1073, 630), rare=(1392, 630), common=(1711, 630))


class Game:
    def __init__(self, committed, season, *, esc_mode='discard', glow_unknown=('owned',), season_ocr=None,
                 linked=None, stuck_dropdown=False, glow_reads=None):
        self.page = 'skin_home'
        self.committed = dict(committed)
        self.committed_season = season
        self.boxes = None
        self.season = None
        self.dropdown = False
        self.pointer = (1280, 700)
        self.esc_mode = esc_mode
        self.glow_unknown = set(glow_unknown)
        self.season_ocr = list(season_ocr or [])
        self.linked = linked
        self.stuck_dropdown = stuck_dropdown
        self.glow_reads = glow_reads  # None = always glow when under pointer
        self.log = []
        self.serial = 0
        self.esc_ignored = False
        self.menu_words = None  # season menu view; None = both seasons listed

    def box_at(self, point):
        for k, (x, y) in LOC.items():
            if x <= point[0] < x + 36 + 190 and y <= point[1] < y + 36:
                return k

    def packet(self, command):
        self.serial += 1
        n = self.serial
        digest = format(n, '064x')
        page = self.page
        regions = []
        if page == 'catalog_filter' and self.dropdown:
            words = self.menu_words if self.menu_words is not None else [
                dict(text='棱镜攻势S2', x=760, y=525, width=144, height=26),
                dict(text='气象感应', x=760, y=600, width=120, height=26)]
            regions.append(dict(kind='season_options', ok=True, truncated=False, words=words))
        result = dict(capture_passed=True, ocr_passed=True, focus_activation_requests=0, focus_restore_requests=0,
                      startup_page=dict(page=page, overlay='none'),
                      frames=[dict(width=2560, height=1440, sha256=digest, source_age_ms=20,
                                   source_uncertainty_ms=1, capture_ms=10)],
                      ocr=dict(frame_age_at_result_ms=100),
                      collection_observation=dict(frame_id='f%d' % n, frame_sha256=digest, same_frame=True,
                                                  regions=regions))
        if '--expected-page' in command:
            exp = command[command.index('--expected-page') + 1]
            if exp != page:
                result['page_error'] = 'E_DIAGNOSTIC_PAGE_MISMATCH'
                result['_exit'] = 1
        if '--catalog-filter-state' in command:
            if page == 'catalog_filter':
                under = self.box_at(self.pointer)
                glow = True
                if self.glow_reads is not None:
                    glow = self.glow_reads.pop(0) if self.glow_reads else False
                boxes = {}
                for k in KEYS:
                    st = self.boxes[k]
                    if self.dropdown and k in ('owned', 'unowned'):
                        st = 'unknown'
                    elif k == under and k in self.glow_unknown and glow:
                        st = 'unknown'
                    boxes[k] = dict(state=st, bounds=[LOC[k][0], LOC[k][1], 36, 36])
                label = self.season_ocr.pop(0) if self.season_ocr else self.season
                complete = bool(label) and all(b['state'] != 'unknown' for b in boxes.values())
                result['catalog_filter_state'] = dict(valid_page=True, complete=complete, same_frame=True,
                                                      frame_id='dxgi:%d' % n, frame_sha256=digest,
                                                      season_label=label, checkboxes=boxes)
            else:
                result['catalog_filter_state'] = dict(valid_page=False, complete=False, reason='E_FILTER_PAGE')
        self.log.append(('read', page, 'dropdown' if self.dropdown else '',
                         {k: {'checked':'C','unchecked':'_','unknown':'?'}[v['state']] for k, v in result.get('catalog_filter_state', {}).get('checkboxes', {}).items()},
                         result.get('catalog_filter_state', {}).get('season_label')))
        return result

    def click(self, point):
        self.pointer = tuple(point)
        self.log.append(('click', list(point), self.page))
        if self.page == 'skin_home' and list(point) == [340, 274]:
            self.page = 'catalog_filter'
            self.boxes = dict(self.committed)
            self.season = self.committed_season
        elif self.page == 'catalog_filter':
            if self.dropdown:
                if 520 <= point[1] <= 560:
                    self.season = '棱镜攻势S2'
                elif 595 <= point[1] <= 630:
                    self.season = '气象感应'
                self.dropdown = self.stuck_dropdown
                return
            if list(point) == [1066, 446]:
                self.dropdown = True
                return
            if list(point) == [1280, 1008]:
                self.committed = dict(self.boxes)
                self.committed_season = self.season
                self.page = 'skin_home'
                return
            k = self.box_at(point)
            if k:
                self.boxes[k] = 'checked' if self.boxes[k] == 'unchecked' else 'unchecked'
                if self.linked and k in self.linked:
                    self.boxes[self.linked[k]] = 'unchecked'

    def escape(self):
        self.log.append(('esc', self.page, self.dropdown))
        if self.page == 'catalog_filter' and not self.esc_ignored:
            if self.dropdown:
                self.dropdown = False
                return
            if self.esc_mode == 'apply':
                self.committed = dict(self.boxes)
                self.committed_season = self.season
            self.page = 'skin_home'


class Backend:
    def __init__(self, clock, game):
        self.clock = clock
        self.game = game
        self.identities = dict(target_hwnd=1, target_pid=2, return_hwnd=3, return_pid=4)
        self.active = False
        self.last_motion = None
        self.last_dispatch = None

    def enter(self):
        self.active = True

    def foreground(self):
        return self.active

    def viewport(self):
        return (2560, 1440)

    def neutral_pointer(self):
        pass

    def capture(self, command, timeout):
        self.clock.wait(.3)
        v = self.game.packet(command)
        return SimpleNamespace(returncode=v.pop('_exit', 0), stdout=json.dumps(v).encode(), stderr=b'')

    def click(self, point, before_dispatch=None):
        if before_dispatch:
            before_dispatch()
        self.game.click(point)
        return 2

    def scroll(self, point, delta, before_dispatch=None):
        if before_dispatch:
            before_dispatch()
        return 1

    def hover(self, point):
        self.game.pointer = tuple(point)
        self.game.log.append(('hover', list(point)))

    def key(self, vk, before_dispatch=None):
        if before_dispatch:
            before_dispatch()
        self.game.escape()
        return 2

    def leave(self):
        self.active = False
        return True

    def metadata(self):
        return dict(identities=self.identities, backend='sim', foreground_transitions=[])



def row(**extra):
    r = dict(snapshot()['rows'][0], product_id='10602', game_grade='史诗品阶')
    r.update(extra)
    return r


WANT = dict(owned='checked', unowned='checked', legendary='unchecked', epic='checked', rare='unchecked',
            common='unchecked')


class FilterDialogSessionTests(unittest.TestCase):
    def run_filter(self, game):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = Clock()
        session = live.ForegroundSession('artifacts/filter-session/run.json', root=Path(temporary.name),
                                         backend=Backend(clock, game), now=clock.now, wait=clock.wait)
        events = []
        trial = CollectionTrial(session, snapshot(), 'unused.json', emit=events.append)
        error = None
        with session:
            session.perform(dict(kind='capture', expected_page='skin_home', collection_observation=True, ui_regions=True))
            try:
                trial.filter_for(row())
            except RuntimeError as caught:
                error = caught
        self.assertFalse(session._failed, 'no session step may fail: the lease must survive to recover')
        confirms = [entry for entry in game.log if entry[0] == 'click' and entry[1] == [1280, 1008]]
        reopened = [e['reason'] for e in events if e['event'] == 'catalog_filter_reopened']
        return error, confirms, reopened

    def test_live_glow_after_the_season_pick_is_read_again_off_the_boxes(self):
        game = Game(WANT, '气象感应')
        error, confirms, reopened = self.run_filter(game)
        self.assertIsNone(error)
        self.assertEqual((len(confirms), reopened), (1, []))
        self.assertEqual((game.committed, game.committed_season), (WANT, '棱镜攻势S2'))
        self.assertIn(('hover', PARK_POINT), game.log)

    def test_an_empty_season_readback_is_read_again(self):
        game = Game(WANT, '气象感应', season_ocr=['气象感应', ''])
        error, confirms, reopened = self.run_filter(game)
        self.assertIsNone(error)
        self.assertEqual((len(confirms), reopened, game.committed_season), (1, [], '棱镜攻势S2'))

    def test_a_dropdown_that_stays_open_is_escaped_twice_reopened_then_stops_unconfirmed(self):
        game = Game(WANT, '气象感应', stuck_dropdown=True)
        error, confirms, reopened = self.run_filter(game)
        self.assertRegex(str(error), '^COLLECTION_FILTER_CHECK_UNKNOWN')
        self.assertEqual((confirms, len(reopened)), ([], 1))
        # Esc closed the dropdown first, then the dialog.
        self.assertEqual([entry[1:] for entry in game.log if entry[0] == 'esc'][:2],
                         [('catalog_filter', True), ('catalog_filter', False)])
        self.assertEqual(game.committed_season, '气象感应')

    def test_a_lost_box_click_is_redone_from_a_fresh_dialog(self):
        for mode in ('discard', 'apply'):
            game = Game(dict(WANT, owned='unchecked'), '棱镜攻势S2', esc_mode=mode, glow_unknown=())
            original = game.click
            lost = []
            def lagged(point, game=game, original=original, lost=lost):
                if game.page == 'catalog_filter' and not game.dropdown and game.box_at(point) == 'owned' and not lost:
                    lost.append(point)
                    game.pointer = tuple(point)
                    return
                original(point)
            game.click = lagged
            error, confirms, reopened = self.run_filter(game)
            self.assertIsNone(error, mode)
            self.assertEqual((len(confirms), reopened), (1, ['COLLECTION_FILTER_READBACK_MISMATCH:owned']), mode)
            self.assertEqual(game.committed, WANT, mode)

    def test_ticks_under_a_glowing_pointer_finish_with_one_strict_proof(self):
        game = Game(dict(WANT, owned='unchecked', epic='unchecked'), '棱镜攻势S2', glow_unknown=KEYS)
        error, confirms, reopened = self.run_filter(game)
        self.assertIsNone(error)
        self.assertEqual((len(confirms), reopened, game.committed), (1, [], WANT))

    def test_a_stop_request_during_the_season_search_stays_a_stop(self):
        # Review 2026-10-09: session errors inside find_season must not be
        # taken for an unreadable dialog (false reopen, run marked blocked).
        game = Game(WANT, '气象感应')
        game.menu_words = [dict(text='气象感应', x=760, y=600, width=120, height=26),
                           dict(text='美杜莎', x=760, y=660, width=90, height=26)]
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = Clock()
        calls = []
        def stop():
            calls.append(1)
            return any(entry[0] == 'click' and entry[1] == [1066, 446] for entry in game.log) and len(calls) > 6
        session = live.ForegroundSession('artifacts/filter-session/stop.json', root=Path(temporary.name),
                                         backend=Backend(clock, game), now=clock.now, wait=clock.wait,
                                         stop_requested=stop)
        events = []
        trial = CollectionTrial(session, snapshot(), 'unused.json', emit=events.append)
        with self.assertRaisesRegex(RuntimeError, '^COLLECTION_STOP_REQUESTED$') as raised:
            with session:
                session.perform(dict(kind='capture', expected_page='skin_home', collection_observation=True,
                                     ui_regions=True))
                trial.filter_for(row())
        self.assertNotIsInstance(raised.exception, FilterUnsettled)
        self.assertFalse(any(e['event'] == 'catalog_filter_reopened' for e in events))
        self.assertNotIn(('esc', 'catalog_filter', True), game.log)

    def test_a_season_missing_from_the_menu_is_searched_once_more_in_a_fresh_dialog(self):
        game = Game(WANT, '气象感应')
        game.menu_words = [dict(text='气象感应', x=760, y=600, width=120, height=26)]
        error, confirms, reopened = self.run_filter(game)
        self.assertRegex(str(error), '^COLLECTION_SEASON_NOT_FOUND')
        self.assertEqual((confirms, len(reopened)), ([], 1))

    def test_an_ignored_esc_stops_without_confirming(self):
        game = Game(WANT, '气象感应', stuck_dropdown=True)
        game.esc_ignored = True
        error, confirms, reopened = self.run_filter(game)
        self.assertEqual(str(error), 'COLLECTION_FILTER_CLOSE_UNPROVEN:catalog_filter')
        self.assertEqual(confirms, [])


if __name__ == '__main__':
    unittest.main()
