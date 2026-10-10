"""Expired-head cleanup without input: states, the allowlist and the
刷新 (then 取消收藏 + 刷新) sequence against a scripted fake session.

User 2026-10-10: the right panel is what counts, whichever card or skin it
shows ("不要在意收藏夹里面的皮肤是不是什么选中皮肤")."""
import copy
import unittest

from purchase_observation import read_screen
from purchase_watchlist_cleanup import (MAX_EXPIRED, REFRESH_POINT, SETTLE_READS, STAR_POINT, clean_expired_head,
                                        cleanup_step_permitted, head_state, star_state)
from run_purchase_countdown_probe import listing_identity, same_listing
from test_purchase_preentry import COUNTDOWN_5S, footer_words
from test_purchase_readonly import packet

REMAINING = [('剩', 2107, 1171, 18, 20), ('余', 2128, 1171, 19, 20), ('：', 2152, 1178, 3, 10), ('2', 2171, 1172, 11, 17),
             ('天', 2184, 1172, 19, 19), ('23', 2206, 1172, 25, 17), ('小', 2233, 1171, 19, 20), ('时', 2255, 1171, 18, 19),
             ('230', 2172, 1233, 46, 19)]


def listing(title, wear, footer, star):
    p = footer_words(packet(), footer)
    p['collection_observation']['regions'] = [dict(kind='product_title', words=[dict(text=title)]),
                                               dict(kind='selected_detail_precise', words=[dict(text=wear)])]
    warm, bright = dict(gold=(.13, 0), white=(0, .2), unknown=(.01, .01))[star]
    p['collection_selected_card'] = dict(selected=True, bounds=[119, 304, 866, 264],
                                         favorite_warm_fraction=warm, favorite_bright_fraction=bright)
    return p


def empty():
    p = packet()
    p['startup_page']['page'] = p['purchase_observation']['page'] = 'empty_watchlist'
    return p


def expired(star='gold', wear='0.392620'):
    return listing('AS Val突击步枪-黑银先锋', wear, REMAINING, star)


def counting():
    return listing('AUG突击步枪-黑银先锋', '0.324958', COUNTDOWN_5S + [('230', 2172, 1233, 46, 19)], 'gold')


class StateTests(unittest.TestCase):
    def test_states_of_the_right_panel(self):
        self.assertEqual(head_state(read_screen(expired()), expired()), 'expired')
        self.assertEqual(head_state(read_screen(counting()), counting()), 'counting')
        self.assertEqual(head_state(read_screen(expired('white')), expired('white')), 'cleared')
        self.assertEqual(head_state(read_screen(expired('unknown')), expired('unknown')), 'cleared')  # refresh only
        # Which card is selected, or whether its wear reads, no longer matters.
        self.assertEqual(head_state(read_screen(expired()), expired(), first_card=lambda s, p: False), 'expired')
        unreadable = expired(wear='')
        self.assertEqual(head_state(read_screen(unreadable), unreadable, identity_of=listing_identity), 'expired')
        self.assertEqual(head_state(read_screen(empty()), empty()), 'empty')
        self.assertEqual((star_state(expired()), star_state(expired('white')), star_state({})),
                         ('gold', 'white', 'unknown'))

    def test_only_armed_star_and_refresh_clicks_on_the_watchlist(self):
        plan = dict(star_armed=False, refresh_armed=False)
        star = dict(kind='click', point=STAR_POINT, expected_before='watchlist_listings')
        refresh = dict(star, point=REFRESH_POINT)
        self.assertFalse(cleanup_step_permitted(star, plan))
        plan['star_armed'] = True
        self.assertFalse(cleanup_step_permitted(dict(star, expected_before='skin_listings'), plan))
        self.assertTrue(cleanup_step_permitted(star, plan))
        self.assertFalse(cleanup_step_permitted(star, plan))          # once per arming
        plan['refresh_armed'] = True
        self.assertFalse(cleanup_step_permitted(star, plan))
        self.assertTrue(cleanup_step_permitted(refresh, plan))
        self.assertEqual((plan['star_clicks'], plan['refresh_clicks']), (1, 1))


class FakeSession:
    def __init__(self, reads):
        self.reads, self.steps, self.plan = list(reads), [], None

    def perform(self, step):
        assert cleanup_step_permitted(step, self.plan), step
        self.steps.append(step['point'])

    def read(self):
        # Past the scripted reads the screen simply stays as it was.
        p = self.reads.pop(0) if len(self.reads) > 1 else copy.deepcopy(self.reads[0])
        return read_screen(p), p


class SequenceTests(unittest.TestCase):
    def run_cleanup(self, reads):
        session = FakeSession(reads)
        plan = dict(star_armed=False, refresh_armed=False)
        session.plan = plan
        record = []
        state, screen, p = clean_expired_head(session, session.read, listing_identity, same_listing, plan, record)
        self.assertEqual(plan, dict(plan, star_armed=False, refresh_armed=False))   # nothing left armed
        return state, session.steps, record

    def test_a_refresh_clears_the_expired_head(self):
        # Live toast03: one refresh dropped every listing past its notice.
        state, steps, record = self.run_cleanup([expired(), counting()])
        self.assertEqual((state, steps), ('counting', [REFRESH_POINT]))
        self.assertEqual([(r['star_before'], r['refreshes'], r['removed']) for r in record], [('gold', 1, True)])

    def test_one_a_refresh_keeps_is_unfavorited_then_refreshed(self):
        state, steps, record = self.run_cleanup([expired()] * (1 + SETTLE_READS) + [expired('white'), counting()])
        self.assertEqual((state, steps), ('counting', [REFRESH_POINT, STAR_POINT, REFRESH_POINT]))
        self.assertEqual((record[0]['star_after'], record[0]['refreshes'], record[0]['removed']), ('white', 2, True))

    def test_nothing_to_clean_sends_nothing(self):
        self.assertEqual(self.run_cleanup([counting()]), ('counting', [], []))

    def test_a_listing_that_will_not_go_is_bounded(self):
        for stays in (expired(), expired('white')):
            with self.subTest(star=star_state(stays)), self.assertRaisesRegex(ValueError, 'TOO_MANY_EXPIRED'):
                self.run_cleanup([stays])

    def test_a_star_already_cleared_gets_refreshes_only(self):
        state, steps, record = self.run_cleanup([expired('white'), counting()])
        self.assertEqual((state, steps), ('counting', [REFRESH_POINT]))
        state, steps, record = self.run_cleanup([expired('white')] * (1 + SETTLE_READS) + [counting()])
        self.assertEqual((state, steps), ('counting', [REFRESH_POINT, REFRESH_POINT]))   # a white star is never touched

    def test_a_reloading_list_after_refresh_is_read_again(self):
        # Live timed03: the read right after the refresh showed an unknown page.
        reloading = packet()
        reloading['startup_page']['page'] = reloading['purchase_observation']['page'] = 'unknown'
        state, steps, record = self.run_cleanup([expired(), reloading, counting()])
        self.assertEqual((state, steps), ('counting', [REFRESH_POINT]))

    def test_several_expired_listings_one_after_the_other(self):
        # A game that drops a listing on refresh only once its star is white.
        class Game:
            def __init__(self, count):
                self.heads, self.steps, self.plan = [['gold', '0.%06d' % (100000 + i)] for i in range(count)], [], None

            def perform(self, step):
                assert cleanup_step_permitted(step, self.plan), step
                self.steps.append(step['point'])
                if step['point'] == STAR_POINT and self.heads:
                    self.heads[0][0] = 'white'
                elif step['point'] == REFRESH_POINT and self.heads and self.heads[0][0] == 'white':
                    self.heads.pop(0)

            def read(self):
                p = expired(*self.heads[0]) if self.heads else counting()
                return read_screen(p), p
        for count, outcome in ((MAX_EXPIRED, 'counting'), (MAX_EXPIRED + 1, 'TOO_MANY_EXPIRED')):
            game = Game(count)
            game.plan = plan = dict(star_armed=False, refresh_armed=False)
            record = []
            if outcome == 'counting':
                state, screen, p = clean_expired_head(game, game.read, listing_identity, same_listing, plan, record)
                self.assertEqual(state, 'counting')
                self.assertEqual(game.steps, [REFRESH_POINT, STAR_POINT, REFRESH_POINT] * count)
                self.assertEqual([r['removed'] for r in record], [True] * count)
            else:
                with self.assertRaisesRegex(ValueError, outcome):
                    clean_expired_head(game, game.read, listing_identity, same_listing, plan, record)

    def test_cleanup_to_an_empty_watchlist(self):
        state, steps, record = self.run_cleanup([expired(), empty()])
        self.assertEqual((state, steps), ('empty', [REFRESH_POINT]))
        self.assertTrue(record[0]['removed'])


if __name__ == '__main__':
    unittest.main()
