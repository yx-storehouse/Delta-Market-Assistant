"""Review 2026-10-09 of the F2 cycle: one runner at a time, bounded evidence,
a stop honoured up to the press, a standby that survives a failed start."""
import ast
import inspect
import json
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import unittest.mock
import zipfile
from pathlib import Path
from types import SimpleNamespace

import evidence_archive
import run_collection_hotkey
import run_exclusive
import run_purchase_timed_buy
from evidence_archive import EvidenceKeeper, pack_sessions
from run_exclusive import exclusive_run

HERE = Path(__file__).resolve().parent


class ExclusiveRunTests(unittest.TestCase):
    def test_a_second_process_is_refused_while_a_run_holds_the_game(self):
        code = textwrap.dedent('''
            import sys; sys.path.insert(0, sys.argv[1])
            from run_exclusive import exclusive_run
            try:
                with exclusive_run():
                    print('ACQUIRED')
            except RuntimeError as error:
                print(error)
        ''')

        def other():
            return subprocess.run([sys.executable, '-B', '-c', code, str(HERE)], capture_output=True, text=True,
                                  timeout=30).stdout.strip()
        with exclusive_run():
            self.assertEqual(other(), 'RUN_ALREADY_ACTIVE')
            with exclusive_run():        # the cycle's own collection inside the cycle
                self.assertEqual(other(), 'RUN_ALREADY_ACTIVE')
            self.assertEqual(other(), 'RUN_ALREADY_ACTIVE')
        self.assertEqual(other(), 'ACQUIRED')

    def test_a_refused_start_tells_the_user_and_runs_nothing(self):
        lines, emitted = [], []
        overlay = SimpleNamespace(show=lambda text, tone='info', ttl_ms=0: lines.append(text))
        ran = []
        with unittest.mock.patch.object(run_collection_hotkey, 'emit_json', emitted.append), \
                unittest.mock.patch.object(run_exclusive, '_held', SimpleNamespace(depth=0)):
            class Busy:
                def acquire(self):
                    return False
            with unittest.mock.patch.object(run_exclusive, 'WindowsMutex', Busy):
                code, result = run_collection_hotkey.run_exclusively(lambda *a, **k: ran.append(1), None, None, None,
                                                                     overlay, hotkey='F2')
        self.assertEqual((code, result['error'], ran), (1, 'RUN_ALREADY_ACTIVE', []))
        self.assertIn('另一个收藏/购买正在运行', lines[-1])


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)

    def run_dir(self, name):
        segment = self.root / name / 'segment_01'
        (segment / 'session.steps').mkdir(parents=True)
        (segment / 'session.json').write_text(json.dumps(dict(steps=['x' * 1000] * 200)), encoding='utf-8')
        for index in range(3):
            (segment / 'session.steps' / ('%06d.json' % index)).write_text('{"step": %d}' % index, encoding='utf-8')
        (self.root / name / 'summary.json').write_text('{}', encoding='utf-8')
        return self.root / name

    def test_sessions_are_packed_whole_and_verified(self):
        run = self.run_dir('r1')
        original = (run / 'segment_01/session.json').read_bytes()
        saved = pack_sessions(run)
        self.assertGreater(saved, 0)
        segment = run / 'segment_01'
        self.assertFalse((segment / 'session.json').exists() or (segment / 'session.steps').exists())
        self.assertTrue((run / 'summary.json').is_file())   # resume reads this; never packed
        with zipfile.ZipFile(segment / 'session.zip') as archive:
            self.assertEqual(archive.read('session.json'), original)
            self.assertEqual(sorted(archive.namelist()), ['session.json'] + ['session.steps/%06d.json' % i for i in range(3)])
        self.assertEqual(pack_sessions(run), 0)            # already packed

    def test_the_newest_runs_stay_unpacked(self):
        packed = []
        keeper = EvidenceKeeper(keep=2, pack=lambda folder: packed.append(folder.name) or 1)
        for name in ('a', 'b', 'c', 'd'):
            keeper.finished_run('collection', self.root / name)
        keeper.finished_run('purchase', self.root / 'p1')
        self.assertEqual(keeper.pending(), 2)
        keeper.pack_older()
        self.assertEqual(packed, ['a', 'b'])
        keeper.pack_older(keep=0)
        self.assertEqual(packed, ['a', 'b', 'c', 'd', 'p1'])

    def test_a_failed_pack_is_recorded_not_raised(self):
        def broken(folder):
            raise OSError('disk')
        keeper = EvidenceKeeper(keep=0, pack=broken)
        keeper.finished_run('purchase', self.root / 'x')
        keeper.pack_older()
        self.assertEqual(len(keeper.errors), 1)


class StopUpToThePressTests(unittest.TestCase):
    def test_the_wait_before_the_press_notices_a_stop(self):
        flag = dict(stop=False)
        started = time.perf_counter()
        with unittest.mock.patch.object(run_purchase_timed_buy, 'qpc_ms', lambda: (time.perf_counter() - started) * 1000):
            def stop():
                if time.perf_counter() - started > .05:
                    flag['stop'] = True
                return flag['stop']
            with self.assertRaisesRegex(RuntimeError, 'COLLECTION_STOP_REQUESTED'):
                run_purchase_timed_buy.wait_until_or_stop(5000, stop)
        self.assertLess(time.perf_counter() - started, 1.0)

    def test_every_input_in_the_dialog_is_preceded_by_a_stop_check(self):
        """Source check of run_attempt: the hover onto the green button, every
        lean watch, the wait and the press itself come right after
        check_stop(stop_requested) (the session's own gate does not see them)."""
        source = textwrap.dedent(inspect.getsource(run_purchase_timed_buy.run_attempt))
        lines = [line.strip() for line in source.splitlines()]

        def preceded(marker):
            found = [i for i, line in enumerate(lines) if line.startswith(marker)]
            return bool(found) and all(any(line.startswith('check_stop(stop_requested)')
                                           for line in lines[max(0, index - 2):index]) for index in found)
        for marker in ('budget.hover(DIALOG_BUY_POINT)', 'budget.confirm(DIALOG_BUY_POINT)'):
            self.assertTrue(preceded(marker), marker)
        # Every press of the green button goes through the one-press budget first.
        clicks = [i for i, line in enumerate(lines) if line.startswith('backend.click(DIALOG_BUY_POINT')]
        self.assertEqual(len(clicks), 2)   # the purchase (--buy) and the early-press probe
        for index in clicks:
            before = index - 1 if lines[index - 1] != 'try:' else index - 2
            self.assertTrue(lines[before].startswith('budget.confirm(DIALOG_BUY_POINT)'))
        # The probe's press re-checks the clock right before SendInput.
        self.assertIn('backend.click(DIALOG_BUY_POINT, before_dispatch=dispatch_guard)', source)
        loops = [i for i, line in enumerate(lines)
                 if line.startswith(('for index in range(20):', 'for index in range(DIALOG_WATCH_ROUNDS):'))]
        self.assertEqual(len(loops), 2)   # the probe's watches and the purchase's
        for loop in loops:
            self.assertTrue(lines[loop + 1].startswith('check_stop(stop_requested)'))
        self.assertNotIn('wait_until(', source.replace('wait_until_or_stop(', ''))
        ast.parse(source)

    def test_after_a_press_the_outcome_is_read_outside_the_stop_gate(self):
        source = inspect.getsource(run_purchase_timed_buy.run_attempt)
        self.assertIn("direct=args.buy)", source)
        self.assertIn("for index in range(AFTER_PRESS_READS):", source)
        self.assertNotIn("time.sleep(.3)", source)   # the ~1.2 s result toast is read back to back
        self.assertIn('record_press()', source)
        # Live buy01: the result came after Esc; the late reads run after the close, before the ledger line.
        close_at = source.index("result['close'] = close_dialog(expect=entry_sent)")
        self.assertIn('late_result()', source[close_at:close_at + 300])


class StandbySurvivesTests(unittest.TestCase):
    def test_a_cycle_that_cannot_start_returns_with_its_reason(self):
        lines, emitted = [], []
        overlay = SimpleNamespace(show=lambda text, tone='info', ttl_ms=0: lines.append(text), monitor=lambda hwnd: None)
        args = SimpleNamespace(config=Path('missing.json'), purchase_rehearsal=False)
        with unittest.mock.patch.object(run_collection_hotkey, 'emit_json', emitted.append), \
                unittest.mock.patch.object(run_purchase_timed_buy, 'settings_from',
                                           side_effect=ValueError('PURCHASE_TIMED_ENTER_TOO_LATE_FOR_CALIBRATION')):
            code, summary = run_collection_hotkey.run_f2_cycle(args, lambda: False, None, overlay, hotkey='F2')
        self.assertEqual(code, 1)
        self.assertEqual(summary['error'], 'PURCHASE_TIMED_ENTER_TOO_LATE_FOR_CALIBRATION')
        self.assertIn('至少要 2 秒', lines[-1])
        self.assertEqual(emitted[-1]['type'], 'result')


if __name__ == '__main__':
    unittest.main()
