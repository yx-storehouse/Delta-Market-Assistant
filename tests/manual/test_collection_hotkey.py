"""Hotkey run, stop request and status banner; fakes only, no game or window."""
import copy
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import collection_live_session as live
from collection_live_session import ForegroundSession
from collection_status import OverlayChannel, PIPE_NAME, StatusFormatter, display_name, stop_reason
import run_collection_hotkey as hotkey
from run_collection_trial import BUDGET_ERRORS
import test_collection_live_session as fixtures
import test_collection_pipeline_receipts as receipts


def rows():
    return [dict(row_index=12, enabled=True, display_name='S11|AUG突击步枪 - 黑银先锋', product_name='AUG突击步枪-黑银先锋',
                 condition_label='成色S', price_min='10', price_max='300', max_wear='5'),
            dict(row_index=13, enabled=False, display_name='S11|M4A1突击步枪 - 黑银先锋', product_name='M4A1突击步枪-黑银先锋',
                 condition_label='成色S', price_min='10', price_max='300', max_wear='5'),
            dict(row_index=14, enabled=True, display_name='S11|AS Val突击步枪 - 黑银先锋', product_name='ASVal突击步枪-黑银先锋',
                 condition_label='成色S', price_min='10', price_max='300', max_wear='5')]


def candidate(price='230', wear='0.357399', condition='成色S', row_index=14):
    return dict(product='ASVal突击步枪-黑银先锋', condition=condition, price=price, wear=wear, row_index=row_index)


class FormatterTests(unittest.TestCase):
    def setUp(self):
        self.status = StatusFormatter(rows())

    def text(self, **event):
        line = self.status.format(event)
        return line and line[0]

    def test_rule_line_names_skin_condition_price_and_wear_like_the_original_banner(self):
        text, tone, log = self.status.format(dict(event='rule_begin', row=14))
        self.assertEqual(text, '[2/2] 正在执行收藏任务：筛选皮肤[AS Val突击步枪-黑银先锋] → 筛选成色[成色S] · 价格 10-300 · 磨损 ≤ 5')
        self.assertEqual((tone, log), ('info', True))
        self.assertEqual(display_name(rows()[0]), 'AUG突击步枪-黑银先锋')

    def test_candidate_lines_show_the_matched_values_and_running_counts(self):
        self.status.format(dict(event='rule_begin', row=14))
        self.assertEqual(self.text(event='favorite_dispatched', row=14, candidate=candidate()),
                         '符合条件：成色S 磨损 0.357399 ≤ 5 · 价格 230 ∈ 10-300 → 已点收藏')
        self.assertEqual(self.status.format(dict(event='favorite_confirmed', row=14, candidate=candidate()))[:2],
                         ('收藏成功：AS Val突击步枪-黑银先锋 230 · 本条 1 把 · 本次 1 把', 'ok'))
        self.status.format(dict(event='favorite_confirmed', row=14, candidate=candidate(price='231')))
        self.status.format(dict(event='listing_top_observed', row=14))
        self.assertEqual(self.text(event='listing_scroll_observed', row=14),
                         '[2/2] AS Val突击步枪-黑银先锋：翻到第 2 屏 · 本条已收藏 2 把')
        self.assertEqual(self.text(event='rule_price_boundary', row=14, candidate=candidate(price='308')),
                         '[2/2] 完成：AS Val突击步枪-黑银先锋 读到 308 > 300，本条新收藏 2 把')

    def test_outside_rule_line_explains_which_limit_failed(self):
        self.assertEqual(self.text(event='candidate_outside_rule', row=14, candidate=candidate(price='308')),
                         '不符合，跳过：价格 308 > 300')
        self.assertEqual(self.text(event='candidate_outside_rule', row=14, candidate=candidate(wear='6.1')),
                         '不符合，跳过：磨损 6.1 > 5')
        self.assertEqual(self.text(event='candidate_outside_rule', row=14, candidate=candidate(condition='成色A')),
                         '不符合，跳过：成色A ≠ 成色S')
        self.assertEqual(self.text(event='favorite_preserved', row=14, candidate=candidate()),
                         '已在关注，跳过：价格 230 磨损 0.357399')

    def test_stop_lines_and_rollover(self):
        self.assertIsNone(self.status.format(dict(event='trial_stopped', reason='COLLECTION_SEGMENT_ROLLOVER')))
        text, tone, log = self.status.format(dict(event='trial_stopped', reason='COLLECTION_STOP_REQUESTED'))
        self.assertEqual((text, tone, log), ('已停止：已按 F2 停止 · 本次新收藏 0 把', 'warn', True))
        self.status.stop_text = 'RelinkStudio 已关闭'
        self.assertIn('RelinkStudio 已关闭', self.text(event='trial_stopped', reason='COLLECTION_STOP_REQUESTED'))
        text, tone, _ = self.status.format(dict(event='trial_stopped', reason='CURSOR_INTERFERENCE'))
        self.assertEqual((text, tone), ('已停止：检测到鼠标被移动，未发送点击 · 本次新收藏 0 把', 'error'))
        self.assertEqual(stop_reason('COLLECTION_STARTUP_PAGE_NOT_CALIBRATED:warehouse'), '当前页面无法识别，请先回到大厅或交易行')
        self.assertEqual(stop_reason('E_SOMETHING_NEW'), 'E_SOMETHING_NEW')
        self.assertIn('全部收藏任务完成：2 条', self.text(event='collection_rule_cycle_complete'))
        self.assertEqual(self.text(event='trial_stopped', reason='BATCH_STEP_FAILED', detail='E_DIAGNOSTIC_ARGUMENTS'),
                         '已停止：画面确认没有通过，未继续操作（E_DIAGNOSTIC_ARGUMENTS） · 本次新收藏 0 把')
        recorded = SimpleNamespace(steps=[dict(kind='capture', capture_attempts=[
            dict(result=dict(error='E_DIAGNOSTIC_ARGUMENTS'))])])
        self.assertEqual(hotkey.step_failure_detail(recorded), 'E_DIAGNOSTIC_ARGUMENTS')
        self.assertIsNone(hotkey.step_failure_detail(SimpleNamespace(steps=[])))
        poisoned = SimpleNamespace(steps=[dict(kind='capture', capture_attempts=[dict(result=dict(
            error=None, local_title_error='LOCAL_PRICE_CONDITION_UNPROVEN', collection_observation=dict(regions=[
                dict(kind='product_title', ok=True, error=''),
                dict(kind='selected_detail_precise', ok=False, error='E_OCR_SESSION_POISONED')])))])])
        self.assertEqual(hotkey.step_failure_detail(poisoned),
                         'LOCAL_PRICE_CONDITION_UNPROVEN / selected_detail_precise:E_OCR_SESSION_POISONED')
        self.assertIsNone(self.status.format(dict(event='listing_window_coverage_proven', row=14)))


class FakePipe:
    def __init__(self, failures=0):
        self.failures = failures
        self.opened = 0
        self.lines = []
        self.closed = 0

    def open(self, path):
        assert path == PIPE_NAME
        if self.failures:
            self.failures -= 1
            raise OSError('busy')
        self.opened += 1
        return 7

    def write(self, handle, data):
        self.lines.extend(json.loads(line) for line in data.decode('utf-8').splitlines())

    def close(self, handle):
        self.closed += 1


class OverlayChannelTests(unittest.TestCase):
    def channel(self, pipe, **options):
        launched = []
        def popen(command, **kwargs):
            launched.append((command, kwargs))
            return SimpleNamespace(poll=lambda: None)
        with tempfile.NamedTemporaryFile(suffix='.exe', delete=False) as exe:
            path = exe.name
        self.addCleanup(lambda: Path(path).unlink())
        channel = OverlayChannel(path, popen=popen, opener=pipe.open, writer=pipe.write, closer=pipe.close,
                                 sleep=lambda seconds: None, **options)
        return channel, launched

    def test_banner_process_is_click_through_overlay_mode_and_lines_arrive_in_order(self):
        pipe = FakePipe(failures=2)
        channel, launched = self.channel(pipe, monitor_hwnd=4660)
        channel.start()
        channel.show('第一行')
        channel.monitor(99)
        channel.show('第二行', 'ok')
        channel.close()
        command, options = launched[0]
        self.assertEqual(command[1:], ['--status-overlay', '--exit-with-client', '--monitor-of-hwnd', '4660'])
        self.assertTrue(options['creationflags'] & 0x08000000 or not hasattr(__import__('subprocess'), 'CREATE_NO_WINDOW'))
        texts = [line.get('text') or line.get('cmd') for line in pipe.lines]
        self.assertEqual(texts[-2:], ['第二行', 'quit'])
        self.assertEqual(pipe.lines[-1]['after_ms'], 8000)
        self.assertIn('monitor', texts)
        self.assertLess(texts.index('monitor'), texts.index('第二行'))
        self.assertEqual(pipe.closed, 1)

    def test_burst_of_text_lines_is_coalesced_to_the_newest(self):
        pipe = FakePipe()
        channel, _ = self.channel(pipe)
        for index in range(50):
            channel.show('行 %d' % index)
        channel.start()
        channel.close()
        self.assertEqual([line.get('text') or line['cmd'] for line in pipe.lines], ['行 49', 'quit'])
        self.assertEqual(channel.metadata()['coalesced'], 49)

    def test_unreachable_banner_never_raises_or_blocks_collection(self):
        pipe = FakePipe(failures=10 ** 9)
        clock = iter(range(0, 10 ** 6))
        channel, _ = self.channel(pipe, clock=lambda: next(clock), connect_seconds=3)
        channel.start()
        channel.show('不会显示')
        channel.close()
        self.assertEqual(pipe.lines, [])
        self.assertEqual(channel.metadata()['failed'], 'connect_timeout')

    def test_standby_banner_arguments_and_final_line_linger(self):
        pipe = FakePipe()
        channel, launched = self.channel(pipe, extra_arguments=('--idle-exit-seconds', '86400'))
        channel.start()
        channel.linger(10000)
        channel.show('已停止：已按 F2 停止', 'warn')
        channel.linger(10000)
        channel.close()
        self.assertEqual(launched[0][0][-2:], ['--idle-exit-seconds', '86400'])
        self.assertEqual(pipe.lines[0], dict(text='已停止：已按 F2 停止', tone='warn', ttl_ms=10000))

    def test_disabled_channel_launches_nothing(self):
        pipe = FakePipe()
        channel, launched = self.channel(pipe, enabled=False)
        channel.start()
        channel.show('x')
        channel.close()
        self.assertEqual((launched, pipe.lines), ([], []))


class CommandTests(unittest.TestCase):
    def test_raw_line_reader_and_stop_flag(self):
        read, write = os.pipe()
        lines, closed = [], threading.Event()
        os.write(write, 'start 12 F3\nstop\npartial'.encode())
        os.close(write)
        hotkey.read_lines(read, lines.append, closed.set)
        os.close(read)
        self.assertEqual(lines, ['start 12 F3', 'stop'])
        self.assertTrue(closed.is_set())
        flag = hotkey.StopFlag()
        flag.request('hotkey')
        flag.request('parent_closed')
        self.assertEqual((flag(), flag.reason), (True, 'hotkey'))

    def test_listener_on_an_open_stdin_pipe_never_stalls_native_imports(self):
        """Live failure 2026-10-08: numpy import waited behind a blocking stdin read."""
        import subprocess
        child = """
import sys, threading, time
sys.path.insert(0, sys.argv[1])
import run_collection_hotkey as h
got = threading.Event()
threading.Thread(target=h.read_lines, daemon=True,
                 args=(0, lambda line: line == 'stop' and got.set(), got.set)).start()
time.sleep(.3)
started = time.monotonic()
import numpy
print('imported %.2f' % (time.monotonic() - started), flush=True)
print('stop' if got.wait(10) else 'no-stop', flush=True)
"""
        process = subprocess.Popen([sys.executable, '-B', '-c', child, str(Path(__file__).parent)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        import queue
        lines = queue.Queue()
        threading.Thread(target=lambda: [lines.put(line.decode()) for line in iter(process.stdout.readline, b'')],
                         daemon=True).start()
        try:
            first = lines.get(timeout=20)  # the old blocking listener stalled here until a line arrived
            process.stdin.write(b'stop\n')
            process.stdin.flush()
            second = lines.get(timeout=20)
        finally:
            process.kill()
            process.wait(10)
            for stream in (process.stdin, process.stdout, process.stderr):
                stream.close()
        self.assertTrue(first.startswith('imported'), first)
        self.assertLess(float(first.split()[1]), 15)
        self.assertEqual(second.strip(), 'stop')

    def test_start_while_loading_is_queued_and_a_stop_cancels_it(self):
        events = []
        channel = hotkey.CommandChannel(on_queued=lambda: events.append('queued'),
                                        on_cancelled=lambda: events.append('cancelled'))
        channel.feed('start 77 F3')
        channel.feed('start 78 F4')
        self.assertEqual(channel.pending, (77, 'F3'))
        channel.feed('stop')
        self.assertIsNone(channel.pending)
        self.assertEqual(events, ['queued', 'cancelled'])
        channel.feed('start 79 F99')
        item, stop = channel.take()
        self.assertEqual(item, (79, None))
        channel.feed('start 80 F2')
        self.assertIsNone(channel.pending)
        channel.feed('stop')
        self.assertEqual((stop(), stop.reason), (True, 'hotkey'))
        channel.end()

    def test_program_close_stops_the_run_and_ends_standby(self):
        channel = hotkey.CommandChannel()
        channel.feed('start 1 F2')
        item, stop = channel.take()
        channel.close()
        self.assertEqual(stop.reason, 'parent_closed')
        channel.end()
        self.assertIsNone(channel.take())


class FakeBackend:
    built = []

    def __init__(self, recognizers, fail=False):
        self.options = dict(recognizers=recognizers)
        self.prepared = self.discarded = False
        self.fail = fail
        FakeBackend.built.append(self)

    def prepare(self):
        if self.fail:
            raise RuntimeError('PREPARE_FAILED')
        self.prepared = True
        return self

    def discard(self):
        self.discarded = True

    def recognizers(self):
        return ('title', 'price', True)


class BackendPoolTests(unittest.TestCase):
    def setUp(self):
        FakeBackend.built = []

    def test_engines_load_once_and_the_next_backend_is_prepared_ahead(self):
        pool = hotkey.BackendPool(FakeBackend, refill_delay=0).load()
        first = pool.take()
        self.assertTrue(first.prepared and first.options['recognizers'] is None)
        self.assertTrue(pool.ready.wait(2))
        self.assertTrue(pool.spare.prepared)
        second = pool.take()
        self.assertIs(second, FakeBackend.built[1])
        self.assertEqual(second.options['recognizers'], ('title', 'price', True))
        pool.close()
        self.assertTrue(FakeBackend.built[-1].discarded)

    def test_a_spare_whose_capture_service_died_is_replaced_before_use(self):
        pool = hotkey.BackendPool(FakeBackend, refill_delay=60).load()
        dead = pool.spare
        dead.healthy = lambda: False
        waited = []
        backend = pool.take(on_wait=lambda: waited.append(True))
        self.assertIsNot(backend, dead)
        self.assertTrue(dead.discarded and backend.prepared)
        self.assertEqual((pool.replaced, waited), (1, [True]))
        pool.close()

    def test_capture_client_reports_whether_it_can_still_serve(self):
        from capture_worker_client import CaptureWorkerClient
        state = dict(code=None)
        process = SimpleNamespace(pid=1, stdout=io.BytesIO(b''), stderr=io.BytesIO(b''), stdin=io.BytesIO(),
                                  poll=lambda: state['code'], wait=lambda timeout=None: 0, returncode=0)
        with tempfile.NamedTemporaryFile(suffix='.exe', delete=False) as exe:
            path = Path(exe.name)
        self.addCleanup(path.unlink)
        client = CaptureWorkerClient(path, process_factory=lambda command, **options: process)
        self.assertTrue(client.alive())
        client.process = process
        self.assertTrue(client.alive())
        state['code'] = 1
        self.assertFalse(client.alive())
        state['code'] = None
        client._responses.put(('error', 'COLLECTION_CAPTURE_WORKER_EOF'))
        self.assertFalse(client.alive())

    def test_take_does_not_wait_for_the_refill_delay(self):
        pool = hotkey.BackendPool(FakeBackend, refill_delay=60).load()
        pool.take()
        waited = []
        started = time.monotonic()
        backend = pool.take(on_wait=lambda: waited.append(True))
        self.assertLess(time.monotonic() - started, 5)
        self.assertTrue(backend.prepared)
        pool.close()

    def test_failed_background_prepare_is_retried_when_needed(self):
        attempts = []
        def factory(recognizers):
            attempts.append(recognizers)
            return FakeBackend(recognizers, fail=len(attempts) == 2)
        pool = hotkey.BackendPool(factory, refill_delay=0).load()
        pool.take()
        self.assertTrue(pool.ready.wait(2))
        self.assertTrue(pool.take().prepared)
        self.assertGreaterEqual(len(attempts), 3)  # load, failed refill, retry (then the next refill)
        pool.close()


class CaptureWarmupTests(unittest.TestCase):
    def test_capture_service_starts_once_before_any_request(self):
        from capture_worker_client import CaptureWorkerClient
        started = []
        def factory(command, **options):
            started.append(command)
            return SimpleNamespace(pid=1, stdout=io.BytesIO(b''), stderr=io.BytesIO(b''), stdin=io.BytesIO(),
                                   poll=lambda: None, wait=lambda timeout=None: 0, returncode=0)
        with tempfile.NamedTemporaryFile(suffix='.exe', delete=False) as exe:
            path = Path(exe.name)
        self.addCleanup(path.unlink)
        client = CaptureWorkerClient(path, process_factory=factory)
        client.start()
        client.start()
        self.assertEqual(started, [[str(path.resolve()), '--live-capture-server']])
        self.assertEqual(client.metadata()['request_count'], 0)
        client.close()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_CAPTURE_WORKER_CLOSED'):
            client.start()


class StandbyTests(unittest.TestCase):
    """--standby: preload, then start/stop/EOF on the command pipe."""

    def test_queued_start_runs_after_loading_stop_reaches_the_run_and_eof_exits(self):
        loaded, began = threading.Event(), threading.Event()
        calls = []

        class Pool:
            recognizers = load_ms = None
            def load(self):
                self.recognizers, self.load_ms = ('t', 'p', True), 1200
                loaded.wait(5)
                return self
            def close(self):
                calls.append('closed')

        def fake_run(args, stop, pool, overlay, *, hotkey, monitor_hwnd=0):
            calls.append((hotkey, monitor_hwnd))
            began.set()
            self.assertTrue(stop.event.wait(5))
            calls.append(stop.reason)
            return 3, {}

        read, write = os.pipe()
        output = io.StringIO()
        args = hotkey.build_parser().parse_args(['--config', 'c.json', '--no-overlay', '--standby'])
        logs = tempfile.TemporaryDirectory()
        self.addCleanup(logs.cleanup)
        self.addCleanup(setattr, hotkey, '_LOG', hotkey._NullLog())
        with patch.object(hotkey, 'run_collection', fake_run), patch.object(hotkey, 'run_f2_cycle', fake_run), patch('sys.stdout', output),              patch.object(hotkey, 'LOGS', Path(logs.name)):
            worker = threading.Thread(target=lambda: calls.append(('exit', hotkey.standby(args, pool=Pool(),
                                                                                         stdin_fd=read))))
            worker.start()
            os.write(write, b'start 4660 F3\n')
            time.sleep(.2)
            self.assertFalse(began.is_set())
            loaded.set()
            self.assertTrue(began.wait(5))
            os.write(write, b'stop\n')
            time.sleep(.2)
            os.close(write)
            worker.join(5)
        os.close(read)
        lines = [json.loads(line) for line in output.getvalue().splitlines()]
        states = [line['state'] for line in lines if line['type'] == 'state']
        self.assertEqual(states, ['loading', 'ready', 'running', 'ready'])
        self.assertIn('识别模型还在加载，加载完自动开始收藏', [line.get('text') for line in lines])
        self.assertTrue(any('已就绪（识别模型加载 1.2 秒）' in line.get('text', '') for line in lines))
        self.assertEqual(calls, [('F3', 4660), 'hotkey', 'closed', ('exit', 0)])


class SessionStopTests(unittest.TestCase):
    setUp = receipts.PipelinedSessionTests.setUp
    session = receipts.PipelinedSessionTests.session
    collect_a = receipts.PipelinedSessionTests.collect_a
    journal_records = receipts.PipelinedSessionTests.journal_records

    def test_stop_sends_no_further_input_and_still_settles_the_in_flight_receipt(self):
        flag = []
        confirmed = []
        session = self.session(stop_requested=lambda: bool(flag))
        session.receipt_listener = confirmed.append
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_STOP_REQUESTED'):
            with session:
                self.collect_a(session, lambda basis: receipts.pixel_reply(basis, 'c'))
                action = session.perform(fixtures.collect())
                self.assertEqual(action['receipt_mode'], 'pipelined_pixel')
                clicks = sum(1 for e in self.backend.events if isinstance(e, tuple) and e[0] == 'click')
                flag.append(True)
                session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
        self.assertEqual(sum(1 for e in self.backend.events if isinstance(e, tuple) and e[0] == 'click'), clicks)
        self.assertEqual(len(confirmed), 1)
        self.assertEqual([r['status'] for r in self.journal_records()], ['confirmed'])
        self.assertIsNone(session.pending)
        self.assertEqual(session.report['error'], 'COLLECTION_STOP_REQUESTED')
        self.assertEqual(self.backend.events.count('leave'), 1)


class FakeUser:
    def __init__(self, foreground, roots, pids):
        self.foreground, self.roots, self.pids = foreground, roots, pids
        self.activated = []
    def GetForegroundWindow(self):
        return self.foreground
    def GetAncestor(self, hwnd, flag):
        return self.roots.get(hwnd, hwnd)
    def GetWindowThreadProcessId(self, hwnd, pid):
        pid.value = self.pids.get(hwnd, 0)
        return 1
    def GetClientRect(self, hwnd, rect):
        rect.right, rect.bottom = 800, 600
        return hwnd in self.pids


class ReturnPolicyTests(unittest.TestCase):
    def backend(self, user):
        backend = object.__new__(live.WindowsBackend)
        backend.u = user
        backend.c = SimpleNamespace(byref=lambda value: value)
        backend.w = SimpleNamespace(DWORD=lambda: SimpleNamespace(value=0),
                                    RECT=lambda: SimpleNamespace(left=0, top=0, right=0, bottom=0))
        backend.return_policy = 'entry_foreground'
        backend.stay_in_target = False
        backend._metadata = {}
        backend.identities = {}
        backend.transitions = []
        backend.capture_worker = backend.watcher = backend.dpi = backend.pointer = None
        backend.halt = SimpleNamespace(set=lambda: None)
        backend.activate = lambda hwnd, pid: user.activated.append(hwnd)
        return backend

    def bind(self, backend, foreground, levels=(0x2000, 0x2000), games=None):
        with patch('collection_live_session.subprocess.run', side_effect=AssertionError('no PowerShell')), \
             patch('collection_live_session.find_game_windows', return_value={10: 100} if games is None else games), \
             patch('collection_live_session.process_integrity', side_effect=lambda pid=None: levels[pid is None]):
            backend._bind_entry_foreground(foreground)

    def test_game_window_must_be_unique(self):
        for games in ({}, {10: 100, 11: 101}):
            with self.assertRaisesRegex(RuntimeError, 'COLLECTION_WINDOW_IDENTITY'):
                self.bind(self.backend(FakeUser(100, {}, {100: 10})), 100, games=games)

    def test_native_lookup_matches_dotnet_main_window_rules(self):
        own = live.find_game_windows('python.exe')
        self.assertTrue(all(type(pid) is int and type(hwnd) is int for pid, hwnd in own.items()))
        self.assertEqual(live.find_game_windows('no-such-image.exe'), {})

    def test_elevated_game_is_refused_before_any_activation(self):
        user = FakeUser(100, {}, {100: 10})
        backend = self.backend(user)
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_GAME_ELEVATED'):
            self.bind(backend, 100, levels=(0x3000, 0x2000))
        self.assertEqual(user.activated, [])
        self.assertTrue(stop_reason('COLLECTION_GAME_ELEVATED').startswith('游戏以管理员权限运行'))
        backend = self.backend(user)
        self.bind(backend, 100, levels=(None, 0x2000))
        self.assertTrue(backend.stay_in_target)

    def test_own_integrity_level_is_readable(self):
        self.assertIn(live.process_integrity(), (0x1000, 0x2000, 0x2100, 0x3000, 0x4000))
        self.assertIsNone(live.process_integrity(0x7FFFFFF0))

    def test_hotkey_pressed_inside_the_game_stays_in_the_game(self):
        user = FakeUser(100, {}, {100: 10})
        backend = self.backend(user)
        self.bind(backend, 100)
        self.assertTrue(backend.stay_in_target)
        self.assertEqual(backend.identities['return_hwnd'], 100)
        self.assertTrue(backend.leave())
        self.assertEqual(user.activated, [])

    def test_hotkey_pressed_elsewhere_returns_to_that_window(self):
        user = FakeUser(301, {301: 300}, {100: 10, 300: 30})
        backend = self.backend(user)
        backend._move_screen = lambda *args, **kwargs: None
        backend.pointer = SimpleNamespace(x=1, y=2)
        self.bind(backend, 301)
        self.assertFalse(backend.stay_in_target)
        self.assertEqual((backend.identities['return_hwnd'], backend.identities['return_pid']), (300, 30))
        user.foreground = 300
        self.assertTrue(backend.leave())
        self.assertEqual(user.activated, [300])

    def test_unknown_policy_is_rejected(self):
        with self.assertRaisesRegex(Exception, 'COLLECTION_RETURN_POLICY'):
            live.WindowsBackend(Path('.'), return_policy='somewhere')

    def test_hotkey_capture_commands_send_no_return_window(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = fixtures.Clock()
            backend = fixtures.FakeBackend(clock)
            backend.identities = dict(target_hwnd=100, target_pid=10, return_hwnd=100, return_pid=10)
            session = ForegroundSession('artifacts/hotkey/run.json', root=Path(directory), backend=backend,
                                        now=clock.now, wait=clock.wait)
            ide = session._command(dict(kind='capture'))
            self.assertIn('--return-hwnd', ide)
            backend.return_policy = 'entry_foreground'
            for command in (session._command(dict(kind='capture', collection_observation=True)),
                            session._pixel_receipt_command(dict(card_id='x'))):
                self.assertEqual(command[command.index('--target-hwnd') + 1], '100')
                self.assertNotIn('--return-hwnd', command)
                self.assertNotIn('--return-pid', command)
                self.assertEqual(command[command.index('--focus-policy') + 1], 'caller-owned')

    def test_session_skips_activation_settle_when_already_in_the_game(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = fixtures.Clock()
            backend = fixtures.FakeBackend(clock)
            backend.stay_in_target = True
            session = ForegroundSession('artifacts/hotkey/run.json', root=Path(directory), backend=backend,
                                        now=clock.now, wait=clock.wait)
            with session:
                pass
            self.assertLess(session.report['entry_settle_ms'], 1)


class TrialTests(unittest.TestCase):
    setUp = receipts.PipelinedTrialTests.setUp
    make_session = receipts.PipelinedTrialTests.make_session

    def test_pipelined_star_click_is_announced_before_its_receipt(self):
        from test_collection_trial import TestTrial, row, snapshot
        session = self.make_session([{'price': 230}] * 6 + [{'price': 601, 'eligible': False}])
        events = []
        trial = TestTrial(session, snapshot(), 'artifacts/mock/input.json', emit=events.append,
                          scroll_profile=dict(schema='mock', delta=-480))
        def plan(packet, rule, records, **options):
            return dict(steps=[dict(kind='scroll_visible_list', lease=dict(delta=options['delta'])),
                               dict(kind='capture', expected_page='skin_listings', collection_layout=True)])
        with patch('collection_scroll.build_scroll_coverage', return_value=dict(schema='mock'), create=True), \
             patch('run_collection_trial.scroll_plan', side_effect=plan):
            trial.scan_row(row())
        names = [e['event'] for e in events]
        self.assertEqual(names.count('favorite_dispatched'), 6)
        self.assertLess(names.index('favorite_dispatched'), names.index('favorite_confirmed'))

    def test_a_lease_ends_at_a_card_boundary_before_the_hard_step_limit(self):
        from test_collection_trial import FakeSession, TestTrial, row, snapshot
        session = FakeSession([{'price': 230}] * 3)
        session.max_steps = 100
        session.steps = [{}] * 60
        trial = TestTrial(session, snapshot(), 'artifacts/mock/input.json')
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_STEP_BUDGET'):
            trial.scan_row(row())
        self.assertIn('COLLECTION_STEP_BUDGET', BUDGET_ERRORS)
        self.assertFalse(any(step.get('kind') == 'collect_selected' for step in session.steps[60:]))

    def test_user_stop_and_rollover_statuses(self):
        from test_collection_trial import FakeSession, TestTrial, snapshot
        self.assertIn('COLLECTION_SEGMENT_ROLLOVER', BUDGET_ERRORS)
        trial = TestTrial(FakeSession([]), snapshot(), 'artifacts/mock/input.json')
        trial.stopped(RuntimeError('COLLECTION_STOP_REQUESTED'))
        self.assertEqual(trial.summary['status'], 'stopped_by_user')
        trial = TestTrial(FakeSession([]), snapshot(), 'artifacts/mock/input.json')
        trial.startup = lambda: None
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SEGMENT_ROLLOVER'):
            trial.run(row_gate=lambda row: False)
        trial.stopped(RuntimeError('COLLECTION_SEGMENT_ROLLOVER'))
        self.assertEqual(trial.summary['status'], 'segment_budget_exhausted')


def snapshot_doc():
    return dict(schema='collection-run-snapshot-v1', ready=True, mode='collect_only', purchase_phase_enabled=False,
                source_kind='current_config', source_sha256='a' * 64, config_sha256='a' * 64, enabled_count=2,
                rows=[dict(r, limit_raw=0, task_id='t%d' % r['row_index']) for r in rows()])


class ResumeFinderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.runs = Path(self.temporary.name)

    def summary(self, name, **values):
        directory = self.runs / name
        directory.mkdir()
        document = dict(snapshot_sha256='b' * 64, rows=[dict(row_index=12)], task_file_fully_completed=False)
        document.update(values)
        (directory / 'summary.json').write_text(json.dumps(document), 'utf-8')
        return directory

    def test_newest_same_snapshot_run_is_resumed_only_when_unfinished(self):
        self.summary('20261008-100000')
        self.summary('20261008-110000', snapshot_sha256='c' * 64)
        with patch('run_collection_trial.validate_resume', return_value=([], {})) as validate:
            found = hotkey.find_resume(self.runs, snapshot_doc(), 'b' * 64)
        self.assertEqual(Path(found[1]['path']).parent.name, '20261008-100000')
        self.assertEqual(validate.call_count, 1)
        self.summary('20261008-120000', task_file_fully_completed=True)
        self.assertIsNone(hotkey.find_resume(self.runs, snapshot_doc(), 'b' * 64))

    def test_invalid_progress_starts_fresh(self):
        self.summary('20261008-100000')
        with patch('run_collection_trial.validate_resume', side_effect=ValueError('X')):
            self.assertIsNone(hotkey.find_resume(self.runs, snapshot_doc(), 'b' * 64))
        self.assertIsNone(hotkey.find_resume(self.runs / 'missing', snapshot_doc(), 'b' * 64))


class RunnerLoopTests(unittest.TestCase):
    """main(): segments roll over at rule boundaries; stops end the run."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.backends = []
        self.trials = []

    def run_main(self, scripts):
        test = self
        scripts = list(scripts)

        class Backend:
            def __init__(self, root, **options):
                self.options = options
                self.identities = dict(target_hwnd=100)
                self.stay_in_target = True
                test.backends.append(self)
            def prepare(self):
                return self
            def discard(self):
                pass
            def recognizers(self):
                return ('title', 'price', True)

        class Session:
            def __init__(self, record, **options):
                self.report_path = Path(record)
                self.options = options
                self.steps = []
                self.pending = self.pending_geometry = None
                self.report = dict(ide_restored=True)
            def __enter__(self):
                return self
            def __exit__(self, *error):
                return False

        class Trial:
            def __init__(self, session, snapshot, path, emit, **options):
                self.session, self.emit, self.options = session, emit, options
                resume = options.get('resume')
                self.summary = dict(rows=list(resume['rows']) if resume else [], status='ready', error=None,
                                    confirmed_new=0, already_favorited=0, unmatched=0, cycle_totals={},
                                    next_row_index=None)
                self.started = 0
                test.trials.append(self)
            def run(self, row_gate=None):
                scripts.pop(0)(self, row_gate)
            def stopped(self, error):
                self.summary.update(status='stopped', error=str(error))
                self.emit(dict(event='trial_stopped', reason=str(error)))
            def persist(self):
                pass

        snapshot = snapshot_doc()
        output = io.StringIO()
        self.addCleanup(setattr, hotkey, '_LOG', hotkey._NullLog())
        with patch.object(hotkey, 'RUNS', self.root / 'runs'), patch.object(hotkey, 'LOGS', self.root / 'logs'), \
             patch('collection_run_config.snapshot_from_files', return_value=snapshot), \
             patch('run_collection_observed.MemoryReviewBackend', Backend), \
             patch('collection_live_session.ForegroundSession', Session), \
             patch('run_collection_trial.CollectionTrial', Trial), \
             patch('run_collection_trial.select_scroll_configuration', return_value=(None, None, None)), \
             patch.object(hotkey.Checkpoint, '__call__', lambda self, summary, force=False: None), \
             patch('sys.stdout', output):
            code = hotkey.main(['--config', str(self.root / 'config.json'), '--no-overlay', '--collect-only'])
        lines = [json.loads(line) for line in output.getvalue().splitlines()]
        return code, lines, lines[-1]

    def test_rollover_continues_in_a_new_lease_reusing_the_models(self):
        def first(trial, gate):
            trial.summary['rows'].append(dict(row_index=12))
            raise RuntimeError('COLLECTION_SEGMENT_ROLLOVER')
        def second(trial, gate):
            self.assertEqual(trial.options['resume']['rows'], [dict(row_index=12)])
            trial.summary['rows'].append(dict(row_index=14))
        code, lines, result = self.run_main([first, second])
        self.assertEqual(code, 0)
        self.assertEqual(result['status'], 'completed')
        self.assertEqual([s['index'] for s in result['segments']], [1, 2])
        self.assertIsNone(self.backends[0].options['recognizers'])
        self.assertEqual(self.backends[1].options['recognizers'], ('title', 'price', True))
        self.assertTrue(all(b.options['return_policy'] == 'entry_foreground' for b in self.backends))
        self.assertEqual(result['purchase_actions'], 0)
        self.assertTrue(lines[0]['text'].startswith('收到 F2'))
        self.assertTrue((Path(result['run_directory']) / 'result.json').is_file())

    def test_user_stop_ends_the_run_without_another_lease(self):
        def stopped(trial, gate):
            raise RuntimeError('COLLECTION_STOP_REQUESTED')
        code, lines, result = self.run_main([stopped])
        self.assertEqual((code, result['status'], len(result['segments'])), (3, 'stopped', 1))
        self.assertTrue(any(l.get('text', '').startswith('已停止：已按 F2 停止') for l in lines))

    def test_budget_without_a_finished_rule_does_not_loop(self):
        def exhausted(trial, gate):
            raise RuntimeError('COLLECTION_TRIAL_TIME_BUDGET')
        code, lines, result = self.run_main([exhausted])
        self.assertEqual((code, len(result['segments'])), (1, 1))

    def test_row_gate_rolls_over_only_after_progress_and_budget(self):
        seen = []
        def gated(trial, gate):
            seen.append(gate(dict(row_index=12)))
            trial.summary['rows'].append(dict(row_index=12))
            seen.append(gate(dict(row_index=14)))
            trial.session.steps.extend([{}] * hotkey.ROLLOVER_STEPS)
            seen.append(gate(dict(row_index=14)))
        self.run_main([gated])
        self.assertEqual(seen, [True, True, False])


class ReceiptRecheckTests(unittest.TestCase):
    """Hotkey run 2026-10-08 22:27: star confirmed, list unchanged, one price read at < .99."""

    def setUp(self):
        fixture = Path(__file__).resolve().parents[1] / 'fixtures/hotkey_recheck_price_unread.json'
        self.recorded = json.loads(fixture.read_text('utf-8'))
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def recheck(self, mutate=None):
        clock = fixtures.Clock()
        self.count = getattr(self, 'count', 0) + 1
        session = ForegroundSession('artifacts/recheck/run%d.json' % self.count, root=Path(self.temporary.name),
                                    backend=fixtures.FakeBackend(clock), now=clock.now, wait=clock.wait)
        result = copy.deepcopy(self.recorded['recheck'])
        if mutate:
            mutate(result)
        def capture(step, remaining):
            session.previous = result
            return dict(kind='capture', passed=False, result=result, exit_status=0)
        session._capture = capture
        return session._recheck_layout(dict(packet=copy.deepcopy(self.recorded['basis']))), session

    def test_unread_price_does_not_turn_an_unchanged_list_into_a_layout_change(self):
        same, session = self.recheck()
        self.assertTrue(same)
        self.assertEqual(session.receipt_settlements[-1],
                         dict(mode='layout_recheck_geometry_only_price_unread', layout_unchanged=True, price_accepted=False))

    def test_other_failures_and_real_movement_still_stop(self):
        def page_mismatch(result):
            result['page_match_passed'] = False
        def moved(result):
            for card in result['collection_layout']['cards']:
                card['bounds'][1] -= 87
        def other_selected_frame(result):
            result['collection_selected_card']['frame_id'] = 'dxgi:other'
        def geometry_error(result):
            result['collection_geometry_error'] = 'COLLECTION_REOBSERVATION_REQUIRED'
        for mutate in (page_mismatch, moved, other_selected_frame, geometry_error):
            same, session = self.recheck(mutate)
            self.assertFalse(same, mutate.__name__)
            self.assertFalse(session.receipt_settlements[-1]['layout_unchanged'])


class ConditionUnreadTests(unittest.TestCase):
    """Hotkey run 2026-10-08 22:45: selected "成色S" read as zero words once."""

    def setUp(self):
        fixture = Path(__file__).resolve().parents[1] / 'fixtures/hotkey_condition_unread.json'
        self.recorded = json.loads(fixture.read_text('utf-8'))

    def verdict(self, mutate=None, pending=None):
        result = copy.deepcopy(self.recorded['result'])
        if mutate:
            mutate(result)
        return live.local_price_readiness_waiting(result, pending or copy.deepcopy(self.recorded['pending_geometry']))

    def test_empty_condition_read_gets_a_fresh_frame_never_a_decision(self):
        verdict = self.verdict()
        self.assertEqual(verdict['reason'], 'local_condition_unread_pending')
        self.assertEqual((verdict['collection_allowed'], verdict['price_accepted'], verdict['scope']),
                         (False, False, 'readiness_retry_only'))

    def test_any_other_condition_or_page_problem_still_stops(self):
        def stray_word(result):
            result['local_price_ocr']['native_observation']['words'] = [dict(text='成色A', x=130, y=1130, width=40, height=20)]
        def counted_word(result):
            for attempt in result['local_price_ocr']['native_observation']['field_attempts']:
                if attempt['field'] == 'condition_bounds':
                    attempt['word_count'] = 1
        def page(result):
            result['page_match_passed'] = False
        def other_error(result):
            result['local_title_error'] = result['local_price_ocr']['error'] = 'LOCAL_PRICE_CARD_BINDING'
        def other_frame(result):
            result['local_price_ocr']['source_frame_id'] = 'dxgi:other'
        for mutate in (stray_word, counted_word, page, other_error, other_frame):
            self.assertIsNone(self.verdict(mutate), mutate.__name__)
        scroll = dict(self.recorded['pending_geometry'], kind='scroll')
        self.assertIsNone(self.verdict(pending=scroll))


if __name__ == '__main__':
    unittest.main()
