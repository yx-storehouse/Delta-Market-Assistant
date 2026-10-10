"""The 我的关注 sort step without a game (user 2026-10-10: 按稀有度升序)."""
import unittest

from purchase_observation import read_screen
from purchase_watchlist_sort import (RARITY_ASCENDING, SORT_POINT, controls_text, ensure_rarity_sort, list_open,
                                     option_point, sort_step_permitted)
from test_purchase_readonly import packet

# The open list as recorded live (sort02, 2560 x 1440): option centres.
OPTIONS = (('默认排序', 1192, 324), ('按稀有度升序', 1213, 388), ('按稀有度降序', 1213, 452), ('按价格升序', 1202, 516))


def watchlist(sort='默认排序', open_list=False):
    p = packet()
    controls = [dict(text=t, x=x, y=253, width=w, height=22)
                for t, x, w in (('筛选', 300, 44), ('不限公示期', 527, 105), (sort, 1153, 126))]
    p['collection_observation'] = dict(regions=[dict(kind='listing_controls', ok=True, words=controls)])
    if open_list:
        listing = next(r for r in p['purchase_observation']['regions'] if r['kind'] == 'listing_text')
        listing['words'] = [dict(text=t, x=x - 60, y=y - 11, width=120, height=22) for t, x, y in OPTIONS]
    return p


class Game:
    """Clicking the dropdown opens the list; clicking an option sorts by it."""

    def __init__(self, sort='默认排序', options=True, open_list=False):
        self.sort, self.open, self.options = sort, open_list, options
        self.session_clicks, self.backend_clicks, self.plan = [], [], None

    def perform(self, step):
        assert sort_step_permitted(step, self.plan), step
        self.session_clicks.append(step['point'])
        self.open = not self.open

    def click(self, point):
        self.backend_clicks.append(point)
        if self.open:
            hit = next((t for t, x, y in OPTIONS if abs(x - point[0]) < 60 and abs(y - point[1]) < 12), None)
            if hit:
                self.sort = hit
        self.open = False

    def read(self):
        p = watchlist(self.sort, self.open and self.options)
        return read_screen(p), p


class SortTests(unittest.TestCase):
    def run_sort(self, game, check_stop=None):
        plan = dict(sort_armed=False)
        game.plan = plan
        record = {}
        outcome = ensure_rarity_sort(game, game, game.read, plan, record, sleep=lambda s: None, check_stop=check_stop)
        self.assertFalse(plan['sort_armed'])
        return outcome, record

    def test_controls_and_option_reading(self):
        self.assertEqual(controls_text(watchlist(RARITY_ASCENDING)), '筛选不限公示期' + RARITY_ASCENDING)
        self.assertEqual(option_point(read_screen(watchlist(open_list=True))), [1213, 388])
        self.assertIsNone(option_point(read_screen(watchlist())))   # the dropdown label itself never counts

    def test_already_sorted_sends_nothing(self):
        game = Game(RARITY_ASCENDING)
        self.assertEqual(self.run_sort(game)[0], 'already')
        self.assertEqual((game.session_clicks, game.backend_clicks), ([], []))

    def test_switches_to_rarity_ascending_and_confirms(self):
        game = Game('默认排序')
        outcome, record = self.run_sort(game)
        self.assertEqual(outcome, 'sorted')
        self.assertEqual((game.session_clicks, game.backend_clicks), ([SORT_POINT], [[1213, 388]]))
        self.assertIn(RARITY_ASCENDING, record['after'])

    def test_an_unseen_option_goes_on_without_toggling_blindly(self):
        # The list did not show its options: nothing seen open, nothing clicked again.
        game = Game('默认排序', options=False)
        outcome, record = self.run_sort(game)
        self.assertEqual(outcome, 'unchanged:OPTION_NOT_FOUND')
        self.assertEqual((game.session_clicks, game.backend_clicks), ([SORT_POINT], []))

    def test_a_list_left_open_is_used_or_closed(self):
        # Review wf_da408028-592: an interrupted attempt may leave it open.
        game = Game('默认排序', open_list=True)
        outcome, record = self.run_sort(game)
        self.assertEqual(outcome, 'sorted')
        self.assertEqual((game.session_clicks, game.backend_clicks), ([], [[1213, 388]]))   # no toggle
        self.assertTrue(record['open_at_start'])
        game = Game(RARITY_ASCENDING, open_list=True)
        outcome, record = self.run_sort(game)
        self.assertEqual((outcome, game.open, game.session_clicks), ('already', False, [SORT_POINT]))
        self.assertTrue(record['closed'])

    def test_no_option_click_after_a_stop(self):
        game = Game('默认排序')

        def stop():
            raise RuntimeError('COLLECTION_STOP_REQUESTED')
        with self.assertRaisesRegex(RuntimeError, 'STOP'):
            self.run_sort(game, check_stop=stop)
        self.assertEqual(game.backend_clicks, [])

    def test_list_open_reads_the_options(self):
        self.assertTrue(list_open(read_screen(watchlist(open_list=True))))
        self.assertFalse(list_open(read_screen(watchlist())))

    def test_only_the_armed_dropdown_click_on_the_watchlist(self):
        plan = dict(sort_armed=False)
        step = dict(kind='click', point=SORT_POINT, expected_before='watchlist_listings')
        self.assertFalse(sort_step_permitted(step, plan))
        plan['sort_armed'] = True
        self.assertFalse(sort_step_permitted(dict(step, expected_before='skin_listings'), plan))
        self.assertFalse(sort_step_permitted(dict(step, point=[1213, 388]), plan))
        self.assertTrue(sort_step_permitted(step, plan))
        self.assertFalse(sort_step_permitted(step, plan))


if __name__ == '__main__':
    unittest.main()
