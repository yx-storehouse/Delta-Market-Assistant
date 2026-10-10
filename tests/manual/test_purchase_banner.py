"""The purchase waiting banner and the 已抢 ledger, without a game."""
import json
import tempfile
import threading
import time
import unittest
from pathlib import Path

from purchase_banner import (BannerTicker, banner_text, display_seconds, grade_key, grade_of, ledger_append,
                             ledger_counts, ledger_path)


class BannerTests(unittest.TestCase):
    def test_the_original_programs_line_without_limits(self):
        # User screenshot: "购买：橙色0/10 紫皮2/10 蓝皮0/10 购买延迟：833 倒计时监控：0分9秒";
        # then "限购不要了…就做一个已抢多少多少就行了".
        text = banner_text(dict(orange=0, purple=2, blue=0), 830, 8600, False)
        self.assertEqual(text, '购买：已抢 橙色0 紫皮2 蓝皮0 购买延迟：830 倒计时监控：0分9秒')
        self.assertEqual(banner_text(dict(orange=1, purple=0, blue=3), 900, 75_000, True),
                         '购买(演练)：已抢 橙色1 紫皮0 蓝皮3 购买延迟：900 倒计时监控：1分15秒')
        self.assertTrue(banner_text(dict(orange=0, purple=0, blue=0), 830, None, False).endswith('倒计时监控：--'))

    def test_seconds_shown_like_the_game(self):
        self.assertEqual([display_seconds(v) for v in (9000, 8999.5, 8001, 8000, 1, 0, -300)], [9, 9, 9, 8, 1, 0, 0])

    def test_ticker_refreshes_and_never_raises(self):
        shown = []

        class Overlay:
            def show(self, text, tone='info', ttl_ms=0):
                shown.append(text)
        values = iter(['a', 'a', 'b'])

        def text():
            try:
                return next(values)
            except StopIteration:
                raise RuntimeError('boom')
        ticker = BannerTicker(Overlay(), text, interval=.01).start()
        time.sleep(.15)
        ticker.stop()
        self.assertEqual(shown, ['a', 'b'])
        self.assertGreater(ticker.errors, 0)


class LedgerTests(unittest.TestCase):
    def test_bought_today_per_grade(self):
        with tempfile.TemporaryDirectory() as folder:
            config = Path(folder) / 'config.json'
            path = ledger_path(config)
            self.assertEqual(ledger_counts(path), dict(orange=0, purple=0, blue=0))
            ledger_append(path, dict(title='AUG突击步枪-天命', grade='史诗品阶', grade_key='purple', outcome='bought'))
            ledger_append(path, dict(title='AWM狙击步枪-私人定制', grade='传说品阶', grade_key='orange', outcome='lottery_lost'))
            with open(path, 'a', encoding='utf-8') as handle:
                handle.write(json.dumps(dict(grade_key='purple', outcome='bought', time='2026-01-01T10:00:00+08:00')) + '\n')
                handle.write('not json\n')
            self.assertEqual(ledger_counts(path), dict(orange=0, purple=1, blue=0))

    def test_grade_from_the_configured_tasks(self):
        snapshot = dict(rows=[dict(product_name='ASVal突击步枪-黑银先锋', game_grade='史诗品阶'),
                              dict(product_name='AWM狙击步枪-私人定制', game_grade='传说品阶')])
        self.assertEqual(grade_of('AS Val突击步枪-黑银先锋', snapshot), '史诗品阶')
        self.assertEqual(grade_of('AWM狙击步枪一私人定制', snapshot), '传说品阶')
        self.assertIsNone(grade_of('P90冲锋枪-天命', snapshot))
        self.assertEqual((grade_key('传说品阶'), grade_key('稀有品阶'), grade_key(None)), ('orange', 'blue', None))


if __name__ == '__main__':
    unittest.main()
