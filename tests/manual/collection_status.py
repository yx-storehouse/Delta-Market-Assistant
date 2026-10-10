"""Top-centre status banner for collection runs.

StatusFormatter is pure: trial events in, (text, tone, log) out. OverlayChannel
starts ``RelinkStudio.exe --status-overlay --exit-with-client`` and writes JSON
lines to its pipe from a background thread. The banner is best effort: a
missing, slow or crashed overlay never delays, changes or stops collection.
"""
from __future__ import annotations

from collections import Counter
from decimal import Decimal, InvalidOperation
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

PIPE_NAME = r'\\.\pipe\RelinkStudioStatusOverlay'

_INPUT = '检测到鼠标或 Shift/Ctrl/Alt 按下，未发送点击'
_MOVED = '检测到鼠标被移动，未发送点击'
_FOREGROUND = '游戏不在前台（被切走或最小化）'
STOP_REASONS = {
    'BATCH_FOREGROUND_LOST': _FOREGROUND,
    'CURSOR_FOREGROUND_LOST': _FOREGROUND,
    'CURSOR_INTERFERENCE': _MOVED,
    'CURSOR_START_CHANGED': _MOVED,
    'CURSOR_SET_NOT_APPLIED': '鼠标没有移到位（系统没有执行），未发送点击',
    'CURSOR_ENDPOINT_NOT_REACHED': _MOVED,
    'COLLECTION_CURSOR_ENDPOINT': _MOVED,
    'COLLECTION_CURSOR_MOVED_BEFORE_INPUT': _MOVED,
    'COLLECTION_TARGET_CHANGED_BEFORE_INPUT': '游戏窗口在点击前发生变化，未发送点击',
    'CURSOR_USER_CHECK_FAILED': _INPUT,
    'COLLECTION_USER_INPUT_ACTIVE': _INPUT,
    'COLLECTION_INPUT_GUARD_CHANGED': _INPUT,
    'COLLECTION_TARGET_OCCLUDED': '游戏画面被其他窗口遮挡',
    'COLLECTION_TARGET_CHANGED_DURING_MOTION': '游戏窗口在操作中发生变化',
    'COLLECTION_WINDOW_IDENTITY': '没有找到唯一的游戏窗口，请先打开游戏',
    'COLLECTION_GAME_ELEVATED': '游戏以管理员权限运行，点击会被系统拦截：请右键 RelinkStudio 选“以管理员身份运行”',
    'COLLECTION_ENTRY_VIEWPORT': '游戏窗口尺寸异常',
    'COLLECTION_CLIENT_RECT': '游戏窗口尺寸异常',
    'COLLECTION_RESUME_PENDING_RECONCILIATION': '上次有一次收藏没有确认，需要先核对收藏记录',
    'RUN_CONFIG_NO_ENABLED_TASKS': '没有启用的收藏任务',
    'COLLECTION_NO_ENABLED_TASKS': '没有启用的收藏任务',
    'COLLECTION_STARTUP_PAGE_NOT_CALIBRATED': '当前页面无法识别，请先回到大厅或交易行',
    'COLLECTION_STARTUP_UNEXPECTED_OVERLAY': '当前页面有未知弹窗，请先关闭',
    'COLLECTION_PRODUCT_NOT_FOUND_IN_OBSERVED_CATALOG': '目录里没有找到这把皮肤',
    'COLLECTION_SEASON_NOT_FOUND': '赛季筛选里没有找到对应赛季',
    'COLLECTION_NONZERO_LIMIT_REQUIRES_COUNTER': '任务设置了收藏数量上限，暂只支持不限数量',
    'COLLECTION_CONDITION_PLAN_NOT_CALIBRATED': '任务成色不是 S/A/B/C，暂不支持',
    'COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE': '列表翻页距离没有已测量的安全档位，未翻页',
    'RUN_CATALOG_SCHEMA': '皮肤资料与收藏程序版本不一致：请关闭并重新打开 RelinkStudio 后再按 F2',
    'RUN_TASK_CATALOG_PROJECTION_CONFLICT': '任务里的皮肤资料与皮肤目录不一致：请关闭并重新打开 RelinkStudio 后再按 F2',
    'RUN_CATALOG_GRADE': '皮肤目录里有无法识别的品阶，请在“皮肤资料”中检查',
    'RUN_CATALOG_DISPLAY_CONFLICT': '追加的皮肤资料格式不对，请在“皮肤资料”中检查',
    'BATCH_STEP_FAILED': '画面确认没有通过，未继续操作',
    'BATCH_DEADLINE': '本段运行时间用完',
}


def display_name(row):
    """'S11|AUG突击步枪 - 黑银先锋' -> 'AUG突击步枪-黑银先锋'."""
    name = str(row.get('display_name') or row.get('product_name') or '')
    return name.split('|', 1)[-1].replace(' - ', '-').strip()


def _decimal(value):
    try:
        number = Decimal(str(value))
    except (InvalidOperation, TypeError, ValueError):
        return None
    return number if number.is_finite() else None


def stop_reason(error, hotkey='F2'):
    code = str(error or '').strip()
    if code == 'COLLECTION_STOP_REQUESTED':
        return '已按 %s 停止' % hotkey
    head = code.split(':', 1)[0]
    if head in STOP_REASONS:
        return STOP_REASONS[head]
    return code or '未知原因'


class StatusFormatter:
    """Chinese one-line status per trial event; counters span the whole run."""

    def __init__(self, rows=(), *, hotkey='F2'):
        self.hotkey = hotkey
        self.bind(rows)
        self.row_new = Counter()
        self.row_windows = Counter()
        self.new_total = 0
        self.preserved_total = 0
        self.outside_total = 0
        self.current = None
        self.stop_text = None  # replaces "已按 F2 停止" for a non-hotkey stop

    def bind(self, rows):
        enabled = [row for row in rows if row.get('enabled')]
        self.rows = {row['row_index']: row for row in enabled}
        self.order = [row['row_index'] for row in enabled]

    def _row(self, event):
        index = event.get('row')
        if index is None:
            candidate = event.get('candidate') or {}
            index = candidate.get('row_index')
        return self.rows.get(index), index

    def position(self, index):
        if index in self.order:
            return '[%d/%d] ' % (self.order.index(index) + 1, len(self.order))
        return ''

    def task_line(self, row):
        return ('正在执行收藏任务：筛选皮肤[%s] → 筛选成色[%s] · 价格 %s-%s · 磨损 ≤ %s'
                % (display_name(row), row['condition_label'], row['price_min'], row['price_max'], row['max_wear']))

    @staticmethod
    def mismatch(candidate, row):
        reasons = []
        if row is not None:
            if candidate.get('condition') and candidate.get('condition') != row['condition_label']:
                reasons.append('%s ≠ %s' % (candidate['condition'], row['condition_label']))
            price, low, high = (_decimal(candidate.get('price')), _decimal(row['price_min']),
                                _decimal(row['price_max']))
            if price is not None and high is not None and price > high:
                reasons.append('价格 %s > %s' % (candidate['price'], row['price_max']))
            elif price is not None and low is not None and price < low:
                reasons.append('价格 %s < %s' % (candidate['price'], row['price_min']))
            wear, limit = _decimal(candidate.get('wear')), _decimal(row['max_wear'])
            if wear is not None and limit is not None and wear > limit:
                reasons.append('磨损 %s > %s' % (candidate['wear'], row['max_wear']))
        return '，'.join(reasons) or '不符合任务条件'

    def format(self, event):
        """Return (text, tone, log) or None for events that do not change the banner."""
        name = event.get('event')
        row, index = self._row(event)
        where = self.position(index)
        candidate = event.get('candidate') or {}
        if name == 'startup_identify_current_page':
            return '正在识别当前页面…', 'info', False
        if name == 'rule_begin' and row is not None:
            self.current = index
            return where + self.task_line(row), 'info', True
        if name == 'catalog_filter_ready':
            grade = event.get('game_grade_label')
            return ('已筛选赛季[%s]%s，正在查找皮肤' % (event.get('season', ''),
                    ' · 品阶[%s] · 已拥有+未拥有' % grade if grade and grade != '全部品阶' else '')), 'info', False
        if name == 'catalog_grade_filter_fallback':
            return '品阶筛选下没找到[%s]，改为全部品阶再找' % event.get('product', ''), 'warn', True
        if name == 'product_title_verified':
            current = self.rows.get(self.current)
            label = display_name(current) if current else event.get('product', '')
            return '已打开[%s]，正在筛选成色' % label, 'info', False
        if name == 'listing_top_observed' and row is not None:
            self.row_windows[index] = 1
            return (where + '%s %s：开始逐把检查（价格 ≤ %s，磨损 ≤ %s）'
                    % (display_name(row), row['condition_label'], row['price_max'], row['max_wear'])), 'info', False
        if name == 'favorite_dispatched' and row is not None:
            return ('符合条件：%s 磨损 %s ≤ %s · 价格 %s ∈ %s-%s → 已点收藏'
                    % (candidate.get('condition', row['condition_label']), candidate.get('wear', '?'),
                       row['max_wear'], candidate.get('price', '?'), row['price_min'], row['price_max'])), 'info', False
        if name == 'favorite_confirmed':
            self.row_new[index] += 1
            self.new_total += 1
            label = display_name(row) if row else candidate.get('product', '')
            return ('收藏成功：%s %s · 本条 %d 把 · 本次 %d 把'
                    % (label, candidate.get('price', ''), self.row_new[index], self.new_total)), 'ok', False
        if name == 'favorite_preserved':
            self.preserved_total += 1
            return ('已在关注，跳过：价格 %s 磨损 %s' % (candidate.get('price', '?'), candidate.get('wear', '?'))), 'info', False
        if name == 'candidate_outside_rule':
            self.outside_total += 1
            return '不符合，跳过：' + self.mismatch(candidate, row), 'info', False
        if name == 'listing_scroll_observed' and row is not None:
            self.row_windows[index] = self.row_windows[index] + 1 if self.row_windows[index] else 2
            return (where + '%s：翻到第 %d 屏 · 本条已收藏 %d 把'
                    % (display_name(row), self.row_windows[index], self.row_new[index])), 'info', False
        if name == 'rule_price_boundary' and row is not None:
            return (where + '完成：%s 读到 %s > %s，本条新收藏 %d 把'
                    % (display_name(row), candidate.get('price', '?'), row['price_max'], self.row_new[index])), 'ok', True
        if name == 'collection_rule_cycle_complete':
            return ('全部收藏任务完成：%d 条 · 本次新收藏 %d 把 · 已在关注 %d 把'
                    % (len(self.order), self.new_total, self.preserved_total)), 'ok', True
        if name == 'trial_stopped':
            reason = event.get('reason')
            if str(reason) == 'COLLECTION_SEGMENT_ROLLOVER':
                return None
            user = str(reason) == 'COLLECTION_STOP_REQUESTED'
            text = self.stop_text if user and self.stop_text else stop_reason(reason, self.hotkey)
            if event.get('detail'):
                text += '（%s）' % str(event['detail'])[:80]
            return ('已停止：%s · 本次新收藏 %d 把' % (text, self.new_total)), 'warn' if user else 'error', True
        return None


class OverlayChannel:
    """Fire-and-forget JSON-lines writer to the banner process.

    Consecutive text lines are coalesced to the newest; commands keep their
    order. Connection or write failures only disable the banner.
    """

    def __init__(self, executable=None, *, enabled=True, pipe=PIPE_NAME, monitor_hwnd=None,
                 popen=subprocess.Popen, opener=None, writer=None, closer=None, connect_seconds=5.0,
                 clock=time.monotonic, sleep=time.sleep, extra_arguments=()):
        self.executable = Path(executable) if executable else None
        self.enabled = bool(enabled)
        self.pipe = pipe
        self.monitor_hwnd = monitor_hwnd
        self.extra_arguments = tuple(extra_arguments)
        self.last = None
        self.popen = popen
        self.opener = opener or (lambda path: os.open(path, os.O_WRONLY | getattr(os, 'O_BINARY', 0)))
        self.writer = writer or os.write
        self.closer = closer or os.close
        self.connect_seconds = connect_seconds
        self.clock, self.sleep = clock, sleep
        self.queue = queue.Queue()
        self.handle = None
        self.process = None
        self.launches = 0
        self.sent = 0
        self.dropped = 0
        self.failed = None
        self._thread = None

    def start(self):
        if not self.enabled or self._thread is not None:
            return self
        self._launch()
        self._thread = threading.Thread(target=self._run, name='collection-status-overlay', daemon=True)
        self._thread.start()
        return self

    def _launch(self):
        if self.executable is None or not self.executable.is_file() or self.launches >= 3:
            return False
        command = [str(self.executable), '--status-overlay', '--exit-with-client']
        if self.monitor_hwnd:
            command += ['--monitor-of-hwnd', str(int(self.monitor_hwnd))]
        if self.pipe != PIPE_NAME:
            command += ['--pipe-name', self.pipe]
        command += list(self.extra_arguments)
        try:
            self.process = self.popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL, close_fds=True,
                                      creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except OSError as error:
            self.failed = 'launch:' + str(error)
            return False
        self.launches += 1
        return True

    def _connect(self):
        deadline = self.clock() + self.connect_seconds
        relaunched = False
        while self.clock() < deadline:
            try:
                self.handle = self.opener(self.pipe)
                return True
            except OSError:
                if not relaunched and self.process is not None and self.process.poll() is not None:
                    # A lingering banner that exited between runs: start one.
                    relaunched = self._launch()
                self.sleep(.05)
        self.failed = self.failed or 'connect_timeout'
        return False

    def _send(self, message):
        data = (json.dumps(message, ensure_ascii=False) + '\n').encode('utf-8')
        for attempt in range(2):
            if self.handle is None and not self._connect():
                return False
            try:
                self.writer(self.handle, data)
                self.sent += 1
                return True
            except OSError:
                self._close_handle()
                if attempt == 0:
                    self._launch()
        return False

    def _close_handle(self):
        if self.handle is not None:
            try:
                self.closer(self.handle)
            except OSError:
                pass
            self.handle = None

    def _run(self):
        while True:
            item = self.queue.get()
            batch = [item]
            while True:
                try:
                    batch.append(self.queue.get_nowait())
                except queue.Empty:
                    break
            closing = False
            pending_text = None
            for message in batch:
                if message is None:
                    closing = True
                    continue
                if 'text' in message:
                    if pending_text is not None:
                        self.dropped += 1
                    pending_text = message
                    continue
                if pending_text is not None:
                    self._send(pending_text)
                    pending_text = None
                self._send(message)
            if pending_text is not None:
                self._send(pending_text)
            if closing:
                self._close_handle()
                return

    def show(self, text, tone='info', ttl_ms=0):
        if self.enabled:
            self.last = (str(text)[:400], tone)
            self.queue.put(dict(text=self.last[0], tone=tone, ttl_ms=int(ttl_ms)))

    def linger(self, ttl_ms):
        """Keep the newest line for ttl_ms more, then hide the banner."""
        if self.enabled and self.last is not None:
            self.queue.put(dict(text=self.last[0], tone=self.last[1], ttl_ms=int(ttl_ms)))

    def monitor(self, hwnd):
        if self.enabled and hwnd:
            self.queue.put(dict(cmd='monitor', hwnd=int(hwnd)))

    @property
    def started(self):
        return self._thread is not None

    def close(self, timeout=2.0, quit_after_ms=8000):
        """Flush, ask the banner to exit after quit_after_ms, and disconnect."""
        if self._thread is None:
            return
        self.queue.put(dict(cmd='quit', after_ms=int(quit_after_ms)))
        self.queue.put(None)
        self._thread.join(timeout)
        self._thread = None

    def metadata(self):
        return dict(enabled=self.enabled, launches=self.launches, sent=self.sent,
                    coalesced=self.dropped, failed=self.failed)
