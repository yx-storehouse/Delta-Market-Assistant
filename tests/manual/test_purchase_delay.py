"""队列已满减延迟 / 公示期加延迟 (user 2026-10-10: "用运行页那两个设置做自动调延迟")."""
import json
import tempfile
import unittest
import unittest.mock
from pathlib import Path
from types import SimpleNamespace

import purchase_cycle
from purchase_delay import STATE_NAME, DelayTuner
from test_purchase_cycle import Pool, Stop, Overlay
from evidence_archive import EvidenceKeeper


def config(folder, **run):
    path = Path(folder) / 'config.json'
    settings = dict(purchaseDelayMs=830, queueFullTrigger=1, queueFullStepMs=20.0, publicityTrigger=2, publicityStepMs=15.0)
    settings.update(run)
    path.write_text(json.dumps(dict(run_settings=settings)), encoding='utf-8')
    return path


class TunerTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)

    def test_queue_full_lowers_and_too_early_raises_after_their_triggers(self):
        tuner = DelayTuner(config(self.folder.name))
        self.assertEqual(tuner.current(), 830)
        change = tuner.record('queue_full')
        self.assertEqual((change['before_ms'], change['after_ms'], change['rule']), (830, 810, 'decrease'))
        self.assertIsNone(tuner.record('not_open_yet'))          # 1 of 2
        self.assertEqual(tuner.record('still_publicity')['after_ms'], 825)
        for outcome in ('bought', 'lottery_lost', 'unknown', 'too_many_buyers', 'rehearsal', None):
            self.assertIsNone(tuner.record(outcome))
        self.assertEqual(tuner.current(), 825)

    def test_a_new_base_set_while_running_is_used_from_the_next_attempt(self):
        path = config(self.folder.name)
        tuner = DelayTuner(path)
        tuner.record('queue_full')
        config(self.folder.name, purchaseDelayMs=700)
        tuner.refresh()
        self.assertEqual(tuner.current(), 700)

    def test_the_delay_in_use_survives_a_restart_and_a_new_base_starts_over(self):
        path = config(self.folder.name)
        DelayTuner(path).record('queue_full')
        self.assertEqual(DelayTuner(path).current(), 810)
        self.assertTrue((Path(self.folder.name) / STATE_NAME).is_file())
        config(self.folder.name, purchaseDelayMs=900)              # changed on the 运行 page
        self.assertEqual(DelayTuner(path).current(), 900)

    def test_bounds_steps_of_zero_and_unreadable_settings(self):
        # Never tuned below TUNED_FLOOR_MS (the dialog zero must still be seen),
        # and never below a lower value set on the 运行 page itself.
        from purchase_delay import TUNED_FLOOR_MS
        tuner = DelayTuner(config(self.folder.name, purchaseDelayMs=830, queueFullStepMs=1000.0))
        self.assertEqual(tuner.record('queue_full')['after_ms'], TUNED_FLOOR_MS)
        self.assertIsNone(tuner.record('queue_full'))               # already at the floor
        tuner = DelayTuner(config(self.folder.name, purchaseDelayMs=10, queueFullStepMs=25.0))
        self.assertIsNone(tuner.record('queue_full'))
        self.assertEqual(tuner.current(), 10)
        tuner = DelayTuner(config(self.folder.name, queueFullStepMs=0.0))
        self.assertIsNone(tuner.record('queue_full'))
        self.assertIsNone(DelayTuner.for_config(Path(self.folder.name) / 'missing.json'))
        self.assertIsNone(DelayTuner.for_config(config(self.folder.name, queueFullTrigger=0)))


class CycleTunesTests(unittest.TestCase):
    def test_each_attempt_uses_the_delay_tuned_by_the_previous_result(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = config(folder)
            settings = SimpleNamespace(buy=True, follow_limit=600, delay_ms=830.0, config=path)
            used, stop = [], Stop()
            results = [dict(status='blocked', error='PURCHASE_TIMED_OUTCOME:queue_full', confirm_clicks=1,
                            outcome='queue_full'),
                       dict(status='blocked', error='PURCHASE_TIMED_OUTCOME:queue_full', confirm_clicks=1,
                            outcome='queue_full'),
                       dict(status='passed', confirm_clicks=1, outcome='bought')]

            def attempt(s, **kw):
                used.append(s.delay_ms)
                result = results.pop(0)
                if not results:
                    stop.request()
                return result
            counter = iter(range(1, 9))
            overlay = Overlay()
            summary = purchase_cycle.run_cycle(
                SimpleNamespace(config=path), stop, Pool(), overlay, hotkey='F2', monitor_hwnd=1,
                collect=lambda *a, **k: (0, {}), runs_dir=root,
                new_run_directory=lambda runs: (runs / ('c%d' % next(counter))).mkdir() or runs / 'c1',
                attempt_fn=attempt, settings=settings, snapshot={}, root=root, sleep=lambda s: None,
                qpc_ms=lambda: 1_000_000.0, keeper=EvidenceKeeper(pack=lambda f: 0), peek=lambda: None, game_in_front=lambda: False)
            self.assertEqual(used, [830.0, 810.0, 790.0])
            self.assertEqual([c['after_ms'] for c in summary['delay_changes']], [810.0, 790.0])
            self.assertTrue(any('购买延迟 830 → 810 ms' in line for line in overlay.lines))

    def test_a_rehearsal_never_tunes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = config(folder)
            settings = SimpleNamespace(buy=False, follow_limit=600, delay_ms=830.0, config=path)
            stop = Stop()

            def attempt(s, **kw):
                stop.request()
                return dict(status='passed', confirm_clicks=0, outcome='rehearsal')
            purchase_cycle.run_cycle(
                SimpleNamespace(config=path), stop, Pool(), Overlay(), hotkey='F2', monitor_hwnd=1,
                collect=lambda *a, **k: (0, {}), runs_dir=Path(folder),
                new_run_directory=lambda runs: (runs / 'c1').mkdir() or runs / 'c1', attempt_fn=attempt,
                settings=settings, snapshot={}, root=Path(folder), sleep=lambda s: None, qpc_ms=lambda: 1_000_000.0,
                keeper=EvidenceKeeper(pack=lambda f: 0), peek=lambda: None, game_in_front=lambda: False)
            self.assertFalse((Path(folder) / STATE_NAME).exists())

    def test_a_rehearsal_shows_the_tuned_delay(self):
        with tempfile.TemporaryDirectory() as folder:
            path = config(folder)
            DelayTuner(path).record('queue_full')                 # 830 -> 810 from an earlier real press
            settings = SimpleNamespace(buy=False, follow_limit=600, delay_ms=830.0, config=path)
            stop, used = Stop(), []

            def attempt(s, **kw):
                used.append(s.delay_ms)
                stop.request()
                return dict(status='passed', confirm_clicks=0, outcome='rehearsal')
            purchase_cycle.run_cycle(
                SimpleNamespace(config=path), stop, Pool(), Overlay(), hotkey='F2', monitor_hwnd=1,
                collect=lambda *a, **k: (0, {}), runs_dir=Path(folder),
                new_run_directory=lambda runs: (runs / 'c1').mkdir() or runs / 'c1', attempt_fn=attempt,
                settings=settings, snapshot={}, root=Path(folder), sleep=lambda s: None, qpc_ms=lambda: 1_000_000.0,
                keeper=EvidenceKeeper(pack=lambda f: 0), peek=lambda: None, game_in_front=lambda: False)
            self.assertEqual(used, [810.0])
            self.assertEqual(DelayTuner(path).current(), 810.0)        # read only: unchanged


if __name__ == '__main__':
    unittest.main()
