"""Pre-entry rehearsal decisions, without input: when to click, what may be
sent, and how the watchlist and the 外观购买 dialog are read in the last seconds."""
import copy
import unittest

from purchase_clock import CountdownClock
from purchase_observation import read_screen, strip_line_icon
from run_purchase_preentry_probe import (BUY_POINT, ENTRY_MARGIN_MS, FINAL_WATCH_MAX_MS, HOVER_MARGIN_MS, PRICE_STATES,
                                         AGE_LIMIT_MS, approach_watch, button_ready_now, dialog_matches, enter_target,
                                         entry_allowed, plan_next, preentry_step_permitted)
from test_purchase_clock import make_watch
from test_purchase_readonly import packet


def footer_words(p, words):
    region = next(r for r in p['purchase_observation']['regions'] if r['kind'] == 'purchase_footer')
    region['words'] = [dict(text=t, x=x, y=y, width=w, height=h) for t, x, y, w, h in words]
    return p


def dialog_words(p, words):
    region = next(r for r in p['purchase_observation']['regions'] if r['kind'] == 'dialog_text')
    region['words'] = [dict(text=t, x=x, y=y, width=w, height=h) for t, x, y, w, h in words]
    return p


COUNTDOWN_5S = [('0', 2096, 1172, 11, 17), ('分', 2109, 1171, 20, 20), ('5', 2131, 1172, 11, 17), ('秒', 2145, 1171, 19, 20),
                ('后', 2166, 1172, 19, 18), ('解', 2187, 1171, 19, 20), ('锁', 2208, 1171, 19, 20), ('购', 2229, 1171, 19, 20),
                ('买', 2250, 1172, 19, 19)]


def publicity_screen():
    return read_screen(footer_words(packet(), COUNTDOWN_5S + [('公', 2138, 1231, 23, 23), ('示', 2163, 1232, 23, 22),
                                                              ('中', 2189, 1231, 21, 24)]))


class ScreenTests(unittest.TestCase):
    def test_green_price_button_during_the_countdown_is_price_ready(self):
        # User screenshot 2026-10-09: "0分5秒后解锁购买" above a green "230".
        screen = read_screen(footer_words(packet(), COUNTDOWN_5S + [('230', 2172, 1233, 46, 19)]))
        self.assertEqual((screen['countdown_seconds'], screen['button_state']), (5, 'price_ready'))
        publicity = read_screen(footer_words(packet(), COUNTDOWN_5S + [('公', 2138, 1231, 23, 23), ('示', 2163, 1232, 23, 22),
                                                                        ('中', 2189, 1231, 21, 24)]))
        self.assertEqual(publicity['button_state'], 'publicity')
        self.assertIn('price_ready', PRICE_STATES)

    def test_dialog_countdown_and_unlocked_line_are_read(self):
        # Dialog line centred above its button; dialog03 read the icon as "0".
        counting = [('0', 1527, 850, 23, 23), ('0', 1558, 852, 11, 17), ('分', 1571, 851, 20, 20), ('3', 1593, 852, 11, 17),
                    ('秒', 1606, 851, 19, 20), ('后', 1627, 852, 19, 18), ('解', 1648, 851, 19, 20), ('锁', 1669, 851, 19, 20),
                    ('购', 1690, 851, 19, 20), ('买', 1711, 852, 13, 19), ('外观购买', 690, 380, 160, 40)]
        screen = read_screen(dialog_words(packet(), counting))
        self.assertEqual(screen['dialog_countdown_seconds'], 3)
        self.assertFalse(screen['dialog_unlocked'])
        unlocked = [('0', 1527, 850, 23, 23), ('剩', 1558, 851, 18, 20), ('余', 1579, 851, 19, 20), ('：', 1603, 858, 2, 10),
                    ('2', 1622, 852, 11, 17), ('天', 1635, 852, 19, 19), ('23', 1657, 852, 25, 17), ('小', 1683, 851, 20, 20),
                    ('时', 1706, 851, 18, 19)]
        screen = read_screen(dialog_words(packet(), unlocked))
        self.assertTrue(screen['dialog_unlocked'])
        self.assertIsNone(screen['dialog_countdown_seconds'])
        kept, icon = strip_line_icon([dict(text=t, x=x, y=y, width=w, height=h) for t, x, y, w, h in unlocked], 'dialog')
        self.assertEqual(icon['text'], '0')


class TimingTests(unittest.TestCase):
    def test_click_target_is_zero_minus_the_set_remaining_time(self):
        zero = 2_000_000.0
        clock = CountdownClock()
        clock.add_watch(make_watch(zero, zero - 20_000, 6_000))
        target, estimate = enter_target(clock, 20, zero - 20_000, 5000.0)
        self.assertAlmostEqual(target, zero - 5000, delta=17)
        self.assertIsNotNone(estimate)
        # Before any tick: from the single read, never later than possible.
        target, estimate = enter_target(CountdownClock(), 20, 1000.0, 5000.0)
        self.assertIsNone(estimate)
        self.assertEqual(target, 1000.0 + 19_000 + 500 - 5000)

    def test_approach_watches_stop_short_of_the_click_and_on_the_button(self):
        self.assertEqual(approach_watch(60_000, False), (10_000, False))
        self.assertEqual(approach_watch(10_000, False), (10_000 - ENTRY_MARGIN_MS - FINAL_WATCH_MAX_MS, True))
        self.assertEqual(approach_watch(3_500, True), (1750, True))   # the short final watch
        self.assertIsNone(approach_watch(2_000, True))                # ready: hover, wait, click
        self.assertEqual(approach_watch(-500, False), (1000, True))   # still waiting for the price

    def test_the_observation_behind_the_click_is_never_older_than_the_input_gate(self):
        # Review: the session refuses input on an observation older than 5 s;
        # a watch's age is its whole request. Model the loop for every set
        # remaining time, price appearance and read time.
        for enter_at in range(1, 11):
            for price_at in (-11_000, -10_500, -10_000, -9_500, -8_000):
                for read_ms in (300, 600, 1000):
                    for phase in range(0, 1000, 125):
                        target = -enter_at * 1000.0
                        now, ready, basis = target - 40_000 - phase, False, target - 40_000 - phase
                        for _ in range(100):
                            planned = approach_watch(target - now, ready)
                            if planned is None:
                                break
                            ms, stop = planned
                            start, end = now, now + ms
                            if stop and not ready and price_at < end:
                                end = max(start, price_at) + 15   # two frames after the change, or the green first frame
                            now, ready, basis = end + read_ms, end >= price_at, start
                        click = max(now + HOVER_MARGIN_MS, target)
                        with self.subTest(enter_at=enter_at, price_at=price_at, read_ms=read_ms, phase=phase):
                            self.assertTrue(ready)
                            self.assertLessEqual(click - basis, 4_600)
                            # On time, unless the price appeared too late: then at most
                            # one read gap plus one read and the hover after it.
                            late_bound = max(0.0, price_at + 15 + 2 * read_ms + HOVER_MARGIN_MS - target)
                            self.assertLessEqual(click - target, late_bound + 1)
                            if price_at + 2 * read_ms + 4_000 <= target:
                                self.assertLessEqual(click - target, 1)


class AllowlistTests(unittest.TestCase):
    def test_one_hover_and_one_click_on_the_buy_button_only_after_arming(self):
        plan = dict(armed=False, hovered=False, clicked=False)
        hover = dict(kind='hover', point=BUY_POINT, viewport=[2560, 1440], expected_before='watchlist_listings')
        click = dict(hover, kind='click')
        with self.assertRaisesRegex(ValueError, 'NOT_ALLOWED'):
            preentry_step_permitted(click, plan)
        plan['armed'] = True
        self.assertTrue(preentry_step_permitted(hover, plan))
        self.assertTrue(preentry_step_permitted(dict(click, expected_before='skin_listings'), plan))
        for step in (click, hover, dict(click, point=[1626, 934]), dict(kind='key', key='escape', expected_before='watchlist_listings'),
                     dict(kind='click', point=[1626, 934], expected_before='unknown')):
            with self.subTest(step=step), self.assertRaisesRegex(ValueError, 'NOT_ALLOWED'):
                preentry_step_permitted(step, plan)
        dialog_watch = dict(kind='capture', collection_observation=True, ui_regions=True, purchase_observation=True,
                            countdown_watch_ms=8000, countdown_area='dialog', stop_on_button=True)
        self.assertTrue(preentry_step_permitted(dialog_watch, plan))
        with self.assertRaisesRegex(ValueError, 'NOT_ALLOWED'):
            preentry_step_permitted(dict(dialog_watch, countdown_area='anywhere'), plan)


class ResyncTests(unittest.TestCase):
    def test_a_target_moved_by_a_resync_gets_a_refresh_read_not_a_refused_click(self):
        # Review: the final watch sees a whole-second re-sync (live clock05->06)
        # and the target moves; the click must not rest on a >4.5 s old read.
        for shift in (-1000, 0, 1000):
            for read_ms in (300, 1000):
                target = -5000.0
                now, ready, basis, shifted, refreshes = target - 30_000, False, target - 30_000, False, 0
                for _ in range(100):
                    age = max(target, now + HOVER_MARGIN_MS) - basis
                    planned = plan_next(target - now, ready, age)
                    if planned is None:
                        break
                    if approach_watch(target - now, ready) is None:
                        refreshes += 1
                    ms, stop = planned
                    start, end = now, now + ms
                    if stop and not ready and -10_000 < end:
                        end = max(start, -10_000) + 15
                    now, ready, basis = end + read_ms, end >= -10_000, start
                    if not shifted and target - now < 2_500:
                        target += shift   # the newest calibration moved display zero
                        shifted = True
                click = max(now + HOVER_MARGIN_MS, target)
                with self.subTest(shift=shift, read_ms=read_ms):
                    self.assertLessEqual(click - basis, AGE_LIMIT_MS + 200)
                    self.assertLessEqual(refreshes, 1)


class EvidenceTests(unittest.TestCase):
    def test_price_ready_needs_the_green_fill_on_the_same_frame(self):
        screen = read_screen(footer_words(packet(), COUNTDOWN_5S + [('230', 2172, 1233, 46, 19)]))
        self.assertTrue(button_ready_now(screen, dict(purchase_countdown_watch=dict(final_button_green_fraction=0.82))))
        self.assertFalse(button_ready_now(screen, dict(purchase_countdown_watch=dict(final_button_green_fraction=0.03))))
        self.assertFalse(button_ready_now(screen, {}))
        # A misread 公示中 ("公示申") is not a price button without the fill.
        misread = read_screen(footer_words(packet(), COUNTDOWN_5S + [('公示申', 2138, 1231, 75, 24)]))
        self.assertEqual(misread['button_state'], 'price_ready')
        self.assertFalse(button_ready_now(misread, dict(purchase_countdown_watch=dict(final_button_green_fraction=0.02))))
        # Live preentry01: the fill rule measured 3 %, but the button pixels
        # changed after a 公示中 read: that change is the evidence.
        self.assertTrue(button_ready_now(screen, dict(purchase_countdown_watch=dict(final_button_green_fraction=0.03)), True))
        self.assertFalse(button_ready_now(publicity_screen(), {}, True))

    def test_the_dialog_must_show_the_verified_listing(self):
        identity = dict(product_title='AUG突击步枪-黑银先锋', selected_detail_precise='0.256413')
        same = read_screen(dialog_words(packet(), [('厶外观购买', 520, 290, 180, 40), ('AUG突击步枪黑银先锋', 624, 390, 230, 26),
                                                  ('S〔J256413〕07', 860, 488, 120, 22)]))
        self.assertTrue(dialog_matches(same, identity))
        other_wear = read_screen(dialog_words(packet(), [('AUG突击步枪黑银先锋', 624, 390, 230, 26), ('C(3.731716)', 860, 488, 120, 22)]))
        self.assertFalse(dialog_matches(other_wear, identity))
        other_title = read_screen(dialog_words(packet(), [('P90冲锋枪黑银先锋', 624, 390, 230, 26), ('S〔J256413〕07', 860, 488, 120, 22)]))
        self.assertFalse(dialog_matches(other_title, identity))
        self.assertFalse(dialog_matches(same, dict(product_title='', selected_detail_precise='0.256413')))
        # Without the listing's wear there is no proof against identical cards.
        self.assertFalse(dialog_matches(same, dict(product_title='AUG突击步枪-黑银先锋', selected_detail_precise='')))
        # The wear digits must end one digit run, not appear across the text.
        scattered = read_screen(dialog_words(packet(), [('AUG突击步枪黑银先锋', 624, 390, 230, 26), ('25', 860, 488, 30, 22),
                                                       ('6413', 1000, 600, 60, 22)]))
        self.assertFalse(dialog_matches(scattered, identity))


class IdentityTests(unittest.TestCase):
    def test_wear_falls_back_to_the_full_read_when_the_crop_misses(self):
        # Live preentry01: the precise crop read nothing, the full read of the
        # same frame had the wear in the detail region.
        from run_purchase_countdown_probe import listing_identity
        p = packet()
        p['collection_observation']['regions'] = [dict(kind='product_title', words=[dict(text='P90冲锋枪-天命')]),
                                                   dict(kind='selected_detail_precise', words=[])]
        region = next(r for r in p['purchase_observation']['regions'] if r['kind'] == 'selected_detail')
        region['words'] = [dict(text=t, x=x, y=619, width=20, height=20) for t, x in
                           (('成', 1931), ('色', 1953), ('A', 2212), ('〔', 2228), ('0', 2244), ('．', 2258), ('827567', 2264), ('〕', 2349))]
        self.assertEqual(listing_identity(p)['selected_detail_precise'], '0.827567')


class EntryTests(unittest.TestCase):
    def test_entry_needs_the_same_listing_a_price_button_and_no_dialog(self):
        p = footer_words(packet(), COUNTDOWN_5S + [('230', 2172, 1233, 46, 19)])
        p['collection_observation']['regions'] = [dict(kind='product_title', words=[dict(text='AWM狙击步枪-私人定制')]),
                                                   dict(kind='selected_detail_precise', words=[dict(text='4.753632)')])]
        from run_purchase_countdown_probe import listing_identity
        identity = listing_identity(p)
        p['purchase_countdown_watch'] = dict(final_button_green_fraction=0.8)
        self.assertIsNone(entry_allowed(read_screen(p), identity, p))
        unproven = dict(p, purchase_countdown_watch=dict(final_button_green_fraction=0.05))
        self.assertEqual(entry_allowed(read_screen(unproven), identity, unproven), 'BUTTON_CHANGE_UNPROVEN')
        self.assertIsNone(entry_allowed(read_screen(unproven), identity, unproven, True))
        self.assertEqual(entry_allowed(read_screen(p), dict(identity, selected_detail_precise=''), p), 'IDENTITY_WEAR_UNREAD')
        other = copy.deepcopy(p)
        other['collection_observation']['regions'][1]['words'][0]['text'] = '0.187079'
        self.assertEqual(entry_allowed(read_screen(other), identity, other), 'LISTING_CHANGED')
        publicity = footer_words(copy.deepcopy(p), COUNTDOWN_5S + [('公', 2138, 1231, 23, 23), ('示', 2163, 1232, 23, 22),
                                                                    ('中', 2189, 1231, 21, 24)])
        self.assertEqual(entry_allowed(read_screen(publicity), identity, publicity), 'BUTTON_STATE:publicity')
        opened = dialog_words(copy.deepcopy(p), [('外观购买', 690, 380, 160, 40)])
        self.assertEqual(entry_allowed(read_screen(opened), identity, opened), 'DIALOG_ALREADY_OPEN')


if __name__ == '__main__':
    unittest.main()
