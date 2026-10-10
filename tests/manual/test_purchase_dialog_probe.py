"""Open-and-cancel dialog probe: price button reading, click planning, allowlist.

The live probe clicks only the price button once and closes the dialog with
Esc or its single 取消. These tests cover the decisions without any input.
"""
import copy
import json
from pathlib import Path
import unittest

from purchase_observation import read_screen
from run_purchase_dialog_probe import cancel_target, dialog_closed, dialog_open, plan_buy_click, step_permitted
from test_purchase_readonly import packet


def footer(p, *texts):
    """Footer lines: the remaining-time line at y 1171, anything else on the
    fixed buy button (y 1231), as the live frame shows them."""
    region = next(r for r in p['purchase_observation']['regions'] if r['kind'] == 'purchase_footer')
    words = []
    for i, text in enumerate(texts):
        on_button = not (text.startswith('剩余') or '解锁购买' in text or text == '公示中')
        words.append(dict(text=text, x=2134 + 90 * (i if not on_button else 0), y=1231 if on_button else 1140 + 30 * i,
                          width=24 * len(text), height=26))
    region['words'] = words
    return p


def dialog(p, *texts):
    region = next(r for r in p['purchase_observation']['regions'] if r['kind'] == 'dialog_text')
    region['words'] = [dict(text=text, x=900 + 200 * i, y=800, width=24 * len(text), height=26)
                       for i, text in enumerate(texts)]
    return p


def observed(screen, decision='Match', price='230'):
    candidate = dict(product='AUG突击步枪-天命', condition='成色S', price=price, wear='0.187079', row_index=0)
    return dict(screen=screen, decision=decision, candidates=[candidate] if decision == 'Match' else [])


class PriceButtonTests(unittest.TestCase):
    def test_remaining_time_and_text_on_the_fixed_button_is_the_buy_button(self):
        screen = read_screen(footer(packet(), '剩余：2天23小时', '230'))
        self.assertEqual(screen['button_state'], 'price_button')
        self.assertEqual((screen['price_button']['text'], screen['price_button']['price']), ('230', '230'))
        self.assertEqual(screen['price_button']['bounds'], [1978, 1206, 393, 74])
        # Live 2026-10-09 (purchase_readonly/dialog01): the red "230" read as "02彐B".
        garbled = read_screen(footer(packet(), '剩余：2天23小时', '02彐B'))
        self.assertEqual(garbled['button_state'], 'price_button')
        self.assertIsNone(garbled['price_button']['price'])

    def test_publicity_or_no_remaining_time_or_no_button_text_are_not_a_buy_button(self):
        for lines, state in ((('0分10秒后解锁购买', '公示中'), 'publicity'), (('公示中', '230'), 'publicity'),
                             (('剩余：2天23小时',), 'unknown'), (('230',), 'unknown')):
            with self.subTest(lines=lines):
                screen = read_screen(footer(packet(), *lines))
                self.assertEqual(screen['button_state'], state)
                self.assertIsNone(screen['price_button'])


class PlanTests(unittest.TestCase):
    def test_a_matching_listing_after_publicity_gives_the_button_centre(self):
        screen = read_screen(footer(packet(), '剩余：2天23小时', '230'))
        point, listing = plan_buy_click(observed(screen))
        self.assertEqual(point, [2174, 1243])
        self.assertEqual(listing['price'], '230')
        garbled = read_screen(footer(packet(), '剩余：2天23小时', '02彐B'))
        self.assertEqual(plan_buy_click(observed(garbled))[0], [2174, 1243])

    def test_anything_else_is_refused_before_input(self):
        good = read_screen(footer(packet(), '剩余：2天23小时', '230'))
        cases = [
            (observed(good, decision='NoMatch'), 'NO_SINGLE_MATCH'),
            (observed(good, price='231'), 'PRICE_MISMATCH'),
            (observed(read_screen(packet())), 'PUBLICITY_COUNTDOWN'),
            (observed(read_screen(footer(packet(), '剩余：2天23小时'))), 'BUTTON_STATE'),
            (observed(read_screen(dialog(footer(packet(), '剩余：2天23小时', '230'), '确认购买'))), 'ALREADY_OPEN'),
        ]
        moved = copy.deepcopy(good)
        moved['price_button']['bounds'] = [100, 100, 40, 20]
        cases.append((observed(moved), 'BUTTON_BOUNDS'))
        other = copy.deepcopy(good)
        other['page'] = 'skin_listings'
        cases.append((observed(other), 'PAGE'))
        for value, code in cases:
            with self.subTest(code=code), self.assertRaisesRegex(ValueError, code):
                plan_buy_click(value)


class DialogTests(unittest.TestCase):
    def test_single_cancel_is_found_and_ambiguity_is_not(self):
        screen = read_screen(dialog(packet(), '确认购买', '确认', '取消'))
        self.assertTrue(dialog_open(screen))
        self.assertEqual(cancel_target(screen), [1324, 813])
        self.assertIsNone(cancel_target(read_screen(dialog(packet(), '确认购买', '确认'))))
        self.assertIsNone(cancel_target(read_screen(dialog(packet(), '取消', '确认', '取消'))))

    def test_closed_means_no_dialog_text_on_a_recognised_page(self):
        self.assertTrue(dialog_closed(read_screen(packet())))
        self.assertFalse(dialog_closed(read_screen(dialog(packet(), '确认购买', '取消'))))
        unknown = packet()
        unknown['startup_page']['page'] = unknown['purchase_observation']['page'] = 'unknown'
        self.assertFalse(dialog_closed(read_screen(unknown)))

    def test_the_price_button_is_allowed_once_and_nothing_else_in_the_watchlist(self):
        buy = dict(point=[2174, 1243], used=False)
        click = dict(kind='click', point=[2174, 1243], viewport=[2560, 1440], expected_before='watchlist_listings')
        self.assertTrue(step_permitted(click, buy))
        with self.assertRaisesRegex(ValueError, 'STEP_NOT_ALLOWED'):
            step_permitted(click, buy)
        for step in (dict(click, point=[2186, 1180]), dict(kind='key', key='escape', expected_before='watchlist_listings'),
                     dict(kind='click', point=[2339, 320], expected_before='watchlist_listings')):
            with self.subTest(step=step), self.assertRaisesRegex(ValueError, 'STEP_NOT_ALLOWED'):
                step_permitted(step, dict(point=[2174, 1243], used=False))
        self.assertFalse(step_permitted(dict(kind='capture', purchase_observation=True, preview=True), buy))
        self.assertFalse(step_permitted(dict(kind='click', point=[2310, 274], expected_before='skin_home'), buy))


class InsufficientBalanceDialogTests(unittest.TestCase):
    """The recorded live dialog (user: "这个页面先记住有用")."""

    @classmethod
    def setUpClass(cls):
        path = Path(__file__).resolve().parents[1] / 'fixtures' / 'purchase_insufficient_balance.json'
        cls.packet = json.loads(path.read_text('utf-8'))['packet']

    def test_recorded_dialog_is_recognised_as_insufficient_balance(self):
        screen = read_screen(copy.deepcopy(self.packet))
        kinds = {signal['kind'] for signal in screen['signals']}
        self.assertTrue({'insufficient_balance', 'recharge_prompt'} <= kinds)
        self.assertEqual(screen['balance_shortfall'], 223)
        self.assertTrue(dialog_open(screen))
        self.assertFalse(dialog_closed(screen))

    def test_its_cancel_was_not_read_so_escape_is_the_close(self):
        # OCR read 充值 but not 取消 on this dialog: no button is clicked by guess.
        self.assertIsNone(cancel_target(read_screen(copy.deepcopy(self.packet))))


if __name__ == '__main__':
    unittest.main()
