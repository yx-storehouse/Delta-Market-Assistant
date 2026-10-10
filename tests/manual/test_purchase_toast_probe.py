"""The toast probe without a game: its allowlist and the press gates."""
import unittest

from purchase_observation import read_screen
from run_purchase_preentry_probe import BUY_POINT
from run_purchase_toast_probe import (MIN_DIALOG_S, MIN_FOOTER_S, SCROLL_DELTA, SCROLL_POINT, card_points, press_permitted, same_bounds,
                                      toast_step_permitted)
from test_purchase_readonly import packet, with_toast


def screen_with(seconds, *, button='公示中', source=10000):
    timer = None if seconds is None else '%d分%d秒后解锁购买' % divmod(seconds, 60)
    return read_screen(packet(source=source, timer=timer, button=button))


class AllowlistTests(unittest.TestCase):
    def test_one_card_click_and_one_footer_press_only_when_armed(self):
        plan = dict(card_point=None, footer_armed=False)
        footer = dict(kind='click', point=BUY_POINT, expected_before='watchlist_listings')
        card = dict(kind='click', point=[552, 436], expected_before='watchlist_listings')
        self.assertFalse(toast_step_permitted(footer, plan))
        self.assertFalse(toast_step_permitted(card, plan))
        plan['card_point'] = [552, 436]
        self.assertTrue(toast_step_permitted(card, plan))
        self.assertFalse(toast_step_permitted(card, plan))       # once per arming
        plan['footer_armed'] = True
        self.assertFalse(toast_step_permitted(dict(footer, expected_before='skin_listings'), plan))
        self.assertTrue(toast_step_permitted(footer, plan))
        self.assertFalse(toast_step_permitted(footer, plan))
        for step in (dict(kind='click', point=[1626, 934], expected_before='watchlist_listings'),
                     dict(kind='collect_selected', point=[2339, 320]), dict(kind='key', key='enter')):
            self.assertFalse(toast_step_permitted(step, plan))
        self.assertTrue(toast_step_permitted(dict(kind='capture', purchase_observation=True), plan))
        scroll = dict(kind='scroll', point=SCROLL_POINT, delta=SCROLL_DELTA, expected_before='watchlist_listings')
        self.assertFalse(toast_step_permitted(scroll, plan))
        plan['scroll_armed'] = True
        self.assertFalse(toast_step_permitted(dict(scroll, delta=360), plan))
        self.assertTrue(toast_step_permitted(scroll, plan))
        self.assertFalse(toast_step_permitted(scroll, plan))


class PressGateTests(unittest.TestCase):
    def test_footer_press_needs_a_fresh_read_far_from_the_unlock(self):
        self.assertIsNone(press_permitted(screen_with(MIN_FOOTER_S), 10500, MIN_FOOTER_S, dialog=False))
        self.assertIn('TOO_CLOSE', press_permitted(screen_with(MIN_FOOTER_S - 1), 10500, MIN_FOOTER_S, dialog=False))
        self.assertIn('READ_AGE', press_permitted(screen_with(600), 14000, MIN_FOOTER_S, dialog=False))
        self.assertIn('READ_AGE', press_permitted(screen_with(600), 9000, MIN_FOOTER_S, dialog=False))
        self.assertIn('BUTTON_STATE', press_permitted(screen_with(None, button='300'), 10500, MIN_FOOTER_S,
                                                      dialog=False))
        toast = read_screen(with_toast(packet(timer='5分0秒后解锁购买'), '订单尚未开放购买'))
        self.assertIn('PAGE', press_permitted(toast, 10500, MIN_FOOTER_S, dialog=False))

    def test_dialog_press_needs_the_purchase_dialog_far_from_the_unlock(self):
        # No 外观购买 dialog on a watchlist frame: refused before any countdown check.
        self.assertEqual(press_permitted(screen_with(600), 10500, MIN_DIALOG_S, dialog=True), 'NOT_A_PURCHASE_DIALOG')


class CardTests(unittest.TestCase):
    def test_only_whole_unselected_cards_in_reading_order(self):
        p = dict(collection_layout=dict(complete=True, cards=[
            dict(bounds=[119, 304, 866, 264], selected=True), dict(bounds=[997, 306, 863, 260], selected=False),
            dict(bounds=[120, 580, 863, 261], selected=False), dict(bounds=[121, 1130, 860, 73], selected=False)]))
        self.assertEqual([point for point, bounds in card_points(p)], [[1428, 436], [551, 710]])
        self.assertEqual(card_points(dict(collection_layout=dict(complete=False, cards=[]))), [])
        self.assertTrue(same_bounds([119, 304, 866, 264], [120, 305, 864, 262]))
        self.assertFalse(same_bounds([119, 304, 866, 264], [997, 306, 863, 260]))


if __name__ == '__main__':
    unittest.main()
