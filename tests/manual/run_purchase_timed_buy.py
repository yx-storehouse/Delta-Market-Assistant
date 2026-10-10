"""Timed purchase of the first 我的关注 listing at its unlock. Without --buy it
is a rehearsal: everything but the final press.

User 2026-10-09: "倒计时还剩5秒之内的时候选择一个我设定的时间比如倒计时3s需要在3s的时候点开
这个小弹窗…确认抢的时候要在倒计时归零那一刻准确按下左键，在这之前需要把鼠标提前移动到购买按钮
的按钮范围上…归零后多少延迟之后准确的点击购买…开小窗之后最重要需要持续识别校准的也只有倒计时".

0. Remove expired listings from the head of the watchlist first (取消收藏 +
   刷新, purchase_watchlist_cleanup), so the first listing is one whose
   public notice still runs ("他俩需要配合").
1. Follow the watchlist countdown (对表); the watchlist price button appears
   5 s before zero (live preentry01/02).
2. At the set remaining time (run_settings.enterBeforeSeconds; --buy needs
   2..5 so that the dialog can still be calibrated) click the watchlist
   price button once, while the countdown still runs, to open 外观购买.
3. Only a 外观购买 dialog with its countdown line is accepted (an
   insufficient-balance dialog puts 充值 where the green button is): pointer
   onto the dialog's green price button right away.
4. Keep calibrating the dialog's own countdown frame by frame (lean native
   watches, no full OCR) through its zero ("0分0秒"; live preentry02: the
   dialog unlocked 985-1011 ms later).
5. At zero + run_settings.purchaseDelayMs: with --buy, press the green button
   ONCE, only if the zero was calibrated (two agreeing ticks or the zero seen),
   agrees with the watchlist clock, and the moment is hit within 15 ms;
   without --buy, record the same decision and keep watching to the unlock.
6. Read the result screens, close with Esc. 充值 is never clicked; there is
   never a second confirm press.

Importing this module is inert; input is sent only from main().
"""
import argparse
import base64
import json
import math
from pathlib import Path
import time
from types import SimpleNamespace

from collection_paths import project_root
from collection_run_config import snapshot_from_files
from purchase_clock import CountdownClock, _zero_interval, qpc_ms, standard_time, system_clock_check, ticks_from_watch
from purchase_observation import final_result_phrases, normalize, read_screen
from run_purchase_countdown_probe import (WATCH_MAX_MS, WATCH_PAGES, first_card_selected, listing_identity,
                                          only_listing_in_first_slot, same_listing, watch_step, watch_summary)
from run_purchase_dialog_probe import ESCAPE, VIEWPORT, dialog_closed, dialog_open, dialog_texts
from run_purchase_preentry_probe import (BUY_POINT, HOVER_MARGIN_MS, button_ready_now, dialog_matches, enter_target,
                                         plan_next, preentry_step_permitted, wait_until)
from run_purchase_probe import navigate, return_policy, sample_step, RETURN_POLICIES
from purchase_watchlist_cleanup import clean_expired_head, cleanup_step_permitted, identity_readable
from purchase_watchlist_sort import ensure_rarity_sort, sort_step_permitted
from collection_status import OverlayChannel
from purchase_banner import (BannerTicker, banner_text, dialog_clock_note, grade_key, grade_of, ledger_append,
                             ledger_counts, ledger_path)

# Its own banner pipe: the F2 standby runner keeps the collection banner's.
# The banner accepts only names under its own prefix (status_overlay.cpp;
# live timed03: "RelinkStudioPurchaseOverlay" made it exit, connect_timeout).
PURCHASE_PIPE = r'\\.\pipe\RelinkStudioStatusOverlay-purchase'
STOP_TEXT = {
    'NOT_IN_PUBLIC_NOTICE': '第一位不在公示期', 'TOO_EARLY': '第一位离解锁还早（超过 10 分钟）',
    'WATCHLIST_EMPTY': '我的关注是空的', 'FIRST_CARD_NOT_SELECTED': '没有选中第一位',
    'LISTING_CHANGED': '第一位换了皮肤', 'PRICE_BUTTON_NEVER_APPEARED': '价格按钮没有出现',
    'NOT_ENTERED': '没能进入购买小窗', 'POINTER_MOVED': '检测到鼠标被移动，未点击',
    'DIALOG_NOT_OPEN': '购买小窗没有打开', 'WRONG_DIALOG': '弹出的不是购买小窗（可能三角币不足）',
    'DIALOG_COUNTDOWN_LINE': '小窗里没读到倒计时', 'NO_PRESS': '校准不够，未按购买',
    'REHEARSAL_WOULD_NOT_PRESS': '演练：这次不会按（校准不够或错过时刻）',
    'CLOSE_UNVERIFIED': '没能确认小窗已关闭', 'OUTCOME': '购买结果',
    'CLEANUP': '清理过期时停止', 'OCCLUDED': '游戏画面被其他窗口遮挡', 'CURSOR_INTERFERENCE': '检测到鼠标被移动，未点击',
    'CURSOR_START_CHANGED': '检测到鼠标被移动，未点击', 'FOREGROUND_LOST': '游戏窗口被切走',
    'CURSOR_SET_NOT_APPLIED': '鼠标没有移到位（系统没有执行），未点击', 'NOT_FOREGROUND': '游戏窗口被切走',
    'COLLECTION_WINDOW_IDENTITY': '没有找到唯一的游戏窗口，请先打开游戏',
    'COLLECTION_GAME_ELEVATED': '游戏以管理员权限运行，点击会被系统拦截：请右键 RelinkStudio 选“以管理员身份运行”',
    'GAME_NOT_IN_FRONT': '游戏窗口被切走', 'SESSION_FAILED': '按下后的一次读图失败（结果已读到）',
    'USER_INPUT_ACTIVE': '检测到鼠标或 Shift/Ctrl/Alt 按下，未点击', 'INPUT_GUARD_CHANGED': '检测到鼠标或键盘操作，未点击',
    'STOP_REQUESTED': '已按 F2 停止', 'ENTER_TOO_LATE': '“提前进入”至少要 2 秒才能校准',
    'STEP_BUDGET': '这一把的识别次数用完了', 'RUN_SETTINGS': '运行设置读不懂',
}
OUTCOME_TEXT = dict(bought='购买成功', lottery_pool='已加入抢购池，等待抽签', lottery_lost='抽签未中',
                    sold_or_removed='已被买走或下架', still_publicity='还在公示期，没买到',
                    not_open_yet='订单尚未开放购买（按早了）', queue_full='抢购队列已满',
                    too_many_buyers='抢购人数过多', insufficient_balance='三角币不足（不会点充值）',
                    result_publicity='购买结果公示中', unknown='结果未识别')


def stop_text(error):
    """A short Chinese reason for the banner; the code stays in result.json."""
    code = str(error or '')
    for key, text in STOP_TEXT.items():
        if key in code:
            return text
    return code[:60] or '未知原因'

# The dialog's green price button (dialog03, user screenshot): the purchase.
DIALOG_BUY_RECT = (1428, 896, 397, 76)
DIALOG_BUY_POINT = [DIALOG_BUY_RECT[0] + DIALOG_BUY_RECT[2] // 2, DIALOG_BUY_RECT[1] + DIALOG_BUY_RECT[3] // 2]
# preentry02: the dialog's countdown reached zero 107 ms before the
# watchlist's. One sample only: the prior plans watches, it never times a press.
DIALOG_PHASE_PRIOR_MS = -107
# The dialog's zero must lie this close to the watchlist's. Live 2026-10-10:
# it fell -1167, -880, -191, -150, +58..+131, +824 and +827 ms from it
# (rehearse10: 34 footer ticks against 4 dialog ticks): the dialog syncs its
# clock when it opens, anywhere within about a second. A sanity bound only:
# misreads are caught by the ticks themselves (agreeing ticks, the 1->0
# frame, the frame bounds).
DIALOG_ZERO_WINDOW_MS = (-1500, 1500)
# A lean watch returns within a few ms of its end; keep it this far before the press.
LEAN_MARGIN_MS = 40
LEAN_MIN_MS = 100
# A dialog line showing s seconds at frame time t reaches 0分0秒 at
# t + (s-1) s at the soonest; this much less for seconds that run short.
ZERO_BOUND_SLACK_MS = 50
# Lean dialog watches of one attempt (short ones while the zero is still unplaced).
DIALOG_WATCH_ROUNDS = 40
# The watchlist's button turns into the price this long before its zero
# (live preentry01: 4995 ms). While it is awaited, one follow watch spans
# that moment (ending on the change, or BUTTON_SPAN_MS after it), and the
# watch before must end FOLLOW_READ_GAP_MS ahead of it: the full read
# between two follow watches takes about a second (live rehearse09 at
# 提前进入 4.8 s: the change fell into a 1.1 s gap, never proven, no entry).
PRICE_BUTTON_LEAD_MS = 4995
BUTTON_SPAN_MS = 600
FOLLOW_READ_GAP_MS = 1500
SPANNING_WATCH_MAX_MS = 2500
# A follow watch whose final full-frame read the page reader cannot place
# (live buy_cycle03 2026-10-10: 'unknown' on the frame the button turned to
# the price, 5 s before the unlock; the attempt gave up) is followed by plain
# full-frame reads (300-400 ms live; review wf_993b38d0-6b8: a re-watch never
# ended early, the live price button's green share is about 0.03) while the
# entry moment leaves this much time.
PAGE_UNKNOWN_REREADS = 2
PAGE_REREAD_MIN_LEAD_MS = 600
# The press goes out at most this long after its planned moment, never before.
PRESS_LATE_LIMIT_MS = 15
# The coarse wait for a real press ends this much early; the backend spins
# the rest right before SendInput, after its checks.
PRESS_SPIN_LEAD_MS = 3
# A real press holds the button this long: down before the moment, the
# release (which the game acts on; user 2026-10-10) at it. The press-down's
# slow hook pass (2-4 ms) then falls before the moment; the hold is also
# visible ("我都看不到按下的动作了").
PRESS_HOLD_MS = 80


def press_hold_ms(delay_ms):
    """The hold for a real press: PRESS_HOLD_MS, shorter for small delays so
    the dialog's zero is still seen before the button goes down."""
    return float(min(PRESS_HOLD_MS, max(0.0, delay_ms - 200.0)))
# Session steps of one attempt (a 10 min follow is about 70 watches, cleanup up
# to 5 x 11), and the steps kept back for the dialog phase before the entry.
ATTEMPT_MAX_STEPS = 300
AFTER_PRESS_READS = 5
# A real press (live buy01, 2026-10-10 08:39): the result toast "抢购队列已满，
# 无法购买" came only about 2.6 s after the press, after Esc, on 我的关注; five
# dialog reads up to 1.4 s saw nothing. So: a few reads in the dialog (its
# green button is live under the pointer after the unlock), Esc, then reads
# on the watchlist until a result shows or LATE_RESULT_READS are done.
BUY_DIALOG_READS = 3
LATE_RESULT_READS = 10
# After a real press the toast region is watched frame by frame (user
# 2026-10-10: when the answer shows measures this machine's server latency;
# live buy_cycle05: 订单尚未开放购买 within about 0.45 s, 抢购队列已满 only
# about 1.9-2.3 s after the press, after Esc). The dialog stays open
# meanwhile; if nothing showed, the watchlist is watched once more after Esc.
TOAST_WATCH_MS = 3500
LATE_TOAST_WATCH_MS = 3000
# Change events this close (frame gaps) belong to one toast sliding and fading in.
TOAST_BURST_GAP_MS = 100
# Agent probe only (user 2026-10-09 "再测一下小窗里按早了的提示"): press the
# dialog's green button this long before ITS zero, to read the toast for a
# press that came too early ("订单尚未开放购买"). The dialog unlocks about 1 s
# after its zero (985-1011 ms in every live run), so with at least
# EARLY_PRESS_MIN_LEAD_MS left nothing can be bought yet. Never in the F2 cycle,
# never together with --buy.
EARLY_PRESS_MIN_LEAD_MS = 1200
# The last watch ends LEAN_MARGIN_MS before the press, well inside a second
# (1240..1840 ms before the zero), so the seconds it shows are unambiguous.
EARLY_PRESS_MAX_LEAD_MS = 1800
# Never closer to the zero than this (the unlock follows about 1 s after it).
EARLY_PRESS_MIN_AHEAD_MS = 1150
# The probe enters at least this early: the dialog must be read and its line
# watched across a second boundary before the press.
EARLY_PRESS_MIN_ENTER_S = 4.0
# Until the dialog's zero is calibrated, watches end this much earlier, so a
# prior zero that is off by up to this much still leaves the planned moment ahead.
EARLY_FIRST_WATCH_SLACK_MS = 250
# The line read the press relies on is at most this old (the last watch stops
# LEAN_MARGIN_MS before the press, or up to LEAN_MIN_MS earlier).
EARLY_LINE_MAX_AGE_MS = LEAN_MARGIN_MS + LEAN_MIN_MS + 40
# The game must still be presenting frames (re-verify wf_27d3331b-2d5: a hung
# game would take the queued click after the unlock): the watch's last frame
# at most this old when it returned, and no gap longer than this in it (live
# watches: at most 36 ms).
EARLY_FRAME_MAX_AGE_MS = 80
EARLY_MAX_FRAME_GAP_MS = 100
# Re-checked right before SendInput (after the pointer positioning).
EARLY_DISPATCH_LATE_LIMIT_MS = 60
DIALOG_RESERVED_STEPS = 20
STOP = 'COLLECTION_STOP_REQUESTED'


# User 2026-10-10: "不要在意收藏夹里面的皮肤是不是什么选中皮肤…直接右边有延迟就识别到时间了
# 就点就行了": the right panel's countdown is followed, whichever listing it
# shows. When another listing takes its place while followed (live early02:
# the head was withdrawn and the next moved up), the new one is followed.
HEAD_SWITCH_JUMP_S = 2.5
MAX_HEAD_SWITCHES = 5


def head_switched(identity, new_identity, after, expected_seconds):
    """True when the right panel now shows another listing: both exact
    wears read and different (the card it sits on does not matter), or its
    countdown jumped away from the followed clock."""
    wears = [str((i or {}).get('selected_detail_precise') or '').strip() for i in (identity, new_identity)]
    if all(identity_readable(i or {}) for i in (identity, new_identity)) and wears[0] != wears[1]:
        return True
    seconds = after['countdown_seconds']
    return seconds is not None and expected_seconds is not None and abs(seconds - expected_seconds) > HEAD_SWITCH_JUMP_S


def switch_evidence(identity, new_identity, after, watch, zero_ms, watch_anchor_seconds=None):
    """(wear_changed, jumped) for the listing the right panel shows now.
    A countdown jump alone counts only when the watch's own ticks, verified
    against its full-frame reading, also put the zero more than
    HEAD_SWITCH_JUMP_S away (review wf_9d655fbd-4e6: a single misread is an
    anchor mismatch for the followed clock, not a new listing)."""
    frame_ms = after['source_frame']['source_mono_ms']
    wear_changed = head_switched(identity, new_identity, dict(after, countdown_seconds=None), None)
    jumped = head_switched(None, None, after, (zero_ms - frame_ms) / 1000.0)
    if jumped and (watch or {}).get('ended_by') == 'jump':
        # The watch's own line reading jumped and the final full-frame
        # reading confirms it: two independent reads of another listing.
        return wear_changed, True
    if ((watch or {}).get('ended_by') == 'jump' and ((watch.get('jump_seen') or {}).get('to') == -2)
            and after.get('countdown_seconds') is None and after.get('button_state') == 'price_button'):
        # Another listing past its notice selected: its 剩余 line and price button.
        return wear_changed, True
    if jumped and not wear_changed:
        anchor = after['countdown_seconds'] if watch_anchor_seconds is None else watch_anchor_seconds
        own = CountdownClock().add_watch(watch, anchor_seconds=anchor)['display_zero']
        jumped = bool(own and own.get('verified') and abs(own['estimate_mono_ms'] - zero_ms) > HEAD_SWITCH_JUMP_S * 1000)
    return wear_changed, jumped


def entry_page_problem(after, identity, current_identity):
    """None if the dialog may be opened from this read: 我的关注 itself, or
    the very listing that was followed there (same exact wear). A market
    list is never bought from (review wf_9d655fbd-4e6: the user might open
    one while F2 follows a countdown)."""
    if after['page'] == 'watchlist_listings':
        return None
    followed = str((identity or {}).get('selected_detail_precise') or '').strip()
    now = str((current_identity or {}).get('selected_detail_precise') or '').strip()
    return None if followed and now == followed else 'PAGE_NOT_WATCHLIST:' + str(after['page'])


def game_window():
    """(pid, hwnd) of the game when it is the foreground window, else None
    (a look never activates it: the user may be working elsewhere)."""
    from collection_live_session import find_game_windows
    from navigate_lobby_to_warehouse import u
    games = find_game_windows()
    if len(games) != 1:
        return None
    (pid, hwnd), = games.items()
    window = u.GetForegroundWindow()
    return (int(pid), int(hwnd)) if window and int(u.GetAncestor(window, 3) or 0) == int(hwnd) else None


def game_in_front():
    return game_window() is not None


def peek_panel(*, backend, output, window):
    """One read of 我的关注's right panel without a foreground lease: a single
    caller-owned capture of the game window (no activation, no pointer, no
    input; the native side refuses a covered window). Returns dict(state,
    countdown_seconds, frame_qpc_ms, wear, title): state 'counting' with a
    countdown, else the page/button state seen. The backend is released."""
    from collection_live_session import ForegroundSession
    pid, hwnd = window
    try:
        backend.identities = dict(target_hwnd=hwnd, target_pid=pid)
        builder = ForegroundSession(Path(output) / 'session.json', backend=backend, timeout_seconds=20, max_steps=1,
                                    numeric_price=True, fast_settle=False)
        command = builder._command(dict(kind='capture', collection_observation=True, ui_regions=True,
                                        collection_layout=True, collection_numeric_price=True,
                                        purchase_observation=True))
        packet = json.loads(backend.capture(command, 15).stdout)
    finally:
        getattr(backend, 'discard', lambda: None)()
    screen = read_screen(packet)
    identity = listing_identity(packet)
    state = ('counting' if screen['page'] == 'watchlist_listings' and screen['countdown_seconds'] is not None
             else '%s/%s' % (screen['page'], screen['button_state']))
    return dict(state=state, countdown_seconds=screen['countdown_seconds'], page=screen['page'],
                overlay=screen['overlay'], button_state=screen['button_state'],
                countdown_unread=bool(screen['countdown_unread']),
                frame_qpc_ms=screen['source_frame']['source_mono_ms'],
                wear=identity.get('selected_detail_precise'), title=identity.get('product_title'))


def check_stop(stop_requested):
    """F2 (or the program closing) between any two steps: no further input but Esc."""
    if stop_requested is not None and stop_requested():
        raise RuntimeError(STOP)


def wait_until_or_stop(target_qpc_ms, stop_requested):
    """wait_until that notices a stop every few ms (up to the last 3 ms spin)."""
    while True:
        check_stop(stop_requested)
        left = target_qpc_ms - qpc_ms()
        if left <= 0:
            return
        time.sleep(min(left, 10) / 1000.0 if left > 3 else 0)
# Dialogs that are not the purchase: 充值 sits where the green button is.
WRONG_DIALOG_KINDS = frozenset(('insufficient_balance', 'recharge_prompt', 'confirm_dialog', 'sold_or_removed',
                                'queue_full', 'still_publicity'))
OUTCOMES = (('success_text', 'bought'), ('lottery_pool', 'lottery_pool'), ('lottery_lost', 'lottery_lost'),
            ('sold_or_removed', 'sold_or_removed'), ('still_publicity', 'still_publicity'),
            ('not_open_yet', 'not_open_yet'),
            ('queue_full', 'queue_full'), ('too_many_buyers', 'too_many_buyers'),
            ('insufficient_balance', 'insufficient_balance'), ('recharge_prompt', 'insufficient_balance'),
            ('result_publicity', 'result_publicity'))


def timed_entry_allowed(after, packet, changed_since_publicity):
    """Before opening the dialog: the watchlist, its button showing the price
    while the countdown still runs (pixel change proven), no dialog yet. The
    listing is not matched (user: "完全一致购买不需要，只需要买就行")."""
    if after['page'] not in WATCH_PAGES or after['overlay'] != 'none':
        return 'PAGE'
    if after['button_state'] != 'price_ready':
        return 'BUTTON_STATE:' + str(after['button_state'])
    if not button_ready_now(after, packet, changed_since_publicity):
        return 'BUTTON_CHANGE_UNPROVEN'
    if after['signals']:
        return 'DIALOG_ALREADY_OPEN'
    return None


def purchase_dialog_problem(screen):
    """None for an open 外观购买 dialog showing its countdown line; otherwise
    the reason (e.g. the insufficient-balance dialog with 充值)."""
    kinds = {signal['kind'] for signal in screen['signals']}
    wrong = sorted(kinds & WRONG_DIALOG_KINDS)
    if wrong:
        return 'WRONG_DIALOG:' + ','.join(wrong)
    if 'purchase_dialog' not in kinds:
        return 'NOT_A_PURCHASE_DIALOG'
    if not screen['dialog_line'] or '后解锁购买' not in screen['dialog_line']:
        return 'DIALOG_COUNTDOWN_LINE:' + str(screen['dialog_line'])
    return None


def read_delay_settings(config_path):
    """(enter_before_s, purchase_delay_ms) from the program's run settings."""
    data = json.loads(Path(config_path).read_text(encoding='utf-8'))
    run = data.get('run_settings', {}) if isinstance(data, dict) else {}
    enter = run.get('enterBeforeSeconds', 3)
    delay = run.get('purchaseDelayMs', 830)
    # One decimal (user 2026-10-10: 4.8 s, the dialog then shows more boundaries).
    if (type(enter) not in (int, float) or not 1 <= enter <= 5 or abs(enter * 10 - round(enter * 10)) > 1e-9
            or type(delay) is not int or not 0 <= delay <= 60000):
        raise ValueError('PURCHASE_TIMED_RUN_SETTINGS')
    return float(enter), delay


def zero_frame_seen(dialog_clock, zero_ms):
    """The watch's own read of the line changing to 0分0秒 at the estimated
    zero, where the last readable value before it (unreadable reads skipped,
    as the native stop-on-zero does) allows that zero: s seconds shown from
    frame time t means a zero in (t + (s-1) s, t + s s]. A tick outside the
    allowed window, or a 0 one second early (review wf_993b38d0-6b8: '0分1秒'
    misread as '0分0秒'), is no sight of the zero."""
    for w in dialog_clock.watches:
        if w['anchor_mismatch']:
            continue
        base = w['baseline']
        last = (base['seconds'], base['source_mono_ms']) if base['state'] == 'countdown' else None
        for t in w['ticks']:
            if t['state'] != 'countdown':
                continue
            mid = sum(_zero_interval(t)) / 2
            if (t['seconds'] == 0 and last is not None and last[0] >= 1 and not t.get('outside_window')
                    and abs(mid - zero_ms) <= 60
                    and last[1] + (last[0] - 1) * 1000.0 - ZERO_BOUND_SLACK_MS < mid <= last[1] + last[0] * 1000.0 + 1):
                return True
            last = (t['seconds'], t['after_mono_ms'])
    return False


def dialog_zero(dialog_clock, prior_zero_ms, footer_zero_ms, footer_calibrated, bounds=None):
    """The dialog's zero: estimate, whether its 0分0秒 frame was seen, how many
    agreeing ticks, and whether it may time a press. bounds: (earliest,
    latest) zero from the watchlist and from every dialog frame still
    counting (earliest_dialog_zero); an estimate outside them is a misread."""
    estimate = dialog_clock.estimate()['display_zero']
    if estimate is None:
        return dict(zero_ms=prior_zero_ms, source='prior', observed=False, ticks_used=0, calibrated=False)
    zero_ms = estimate['estimate_mono_ms']
    observed = zero_frame_seen(dialog_clock, zero_ms)
    low, high = DIALOG_ZERO_WINDOW_MS
    uncertainty = estimate['uncertainty_ms']
    in_bounds = bounds is None or (zero_ms + uncertainty >= bounds[0] and zero_ms - uncertainty <= bounds[1])
    agrees = (low <= zero_ms - footer_zero_ms <= high) if footer_calibrated else in_bounds
    calibrated = (estimate['ticks_used'] >= 2 or observed) and agrees and in_bounds
    return dict(zero_ms=zero_ms, source='dialog', observed=observed, ticks_used=estimate['ticks_used'],
                uncertainty_ms=uncertainty, offset_from_footer_ms=zero_ms - footer_zero_ms,
                agrees_with_footer=agrees, in_bounds=in_bounds, calibrated=calibrated)


def earliest_dialog_zero(earliest_ms, watch, latest_ms):
    """The earliest moment the dialog's zero can still come: at first
    DIALOG_ZERO_WINDOW_MS before the watchlist's earliest zero, then raised by
    each lean watch whose last frame still counted (s seconds at frame time t:
    0分0秒 from t + (s-1) s at the soonest); an estimate before it is a
    misread. A reading that puts it past the latest zero the watchlist allows
    is a misread itself and changes nothing."""
    baseline, ticks = ticks_from_watch(watch)
    last = watch.get('last_source_mono_ms')
    readings = [baseline] + ticks
    for index, reading in enumerate(readings):
        # Each value is on screen up to the frame before the next change, the
        # last one up to the watch's last frame (review wf_3215e495-f2d: a 0分1秒
        # shown right up to the zero bounds an estimate a second early).
        until = readings[index + 1]['before_mono_ms'] if index + 1 < len(readings) else last
        seconds = reading.get('seconds')
        if reading['state'] != 'countdown' or seconds is None or seconds < 1 or type(until) not in (int, float):
            continue
        bound = float(until) + (seconds - 1) * 1000.0 - ZERO_BOUND_SLACK_MS
        if bound <= latest_ms:
            earliest_ms = max(earliest_ms, bound)
    return earliest_ms


def next_dialog_watch(now_ms, zero, latest_zero_ms, delay_ms):
    """(ms, stop_on_zero) for the next lean dialog watch, or None: time to
    press. Until the dialog's 0分0秒 is seen, one watch runs through it and
    ends on its frame (native --purchase-countdown-stop-on-zero; review
    wf_b0b8309e-39d: a zero between two watches was only inferred, and a
    misread then moved the press), at the latest when the press for the
    latest zero the watchlist allows is due, or the press its ticks already
    place. Then a last watch up to shortly before the press."""
    if zero['observed']:
        end = zero['zero_ms'] + delay_ms - LEAN_MARGIN_MS
    else:
        end = latest_zero_ms + delay_ms - LEAN_MARGIN_MS
        if zero['calibrated']:
            end = min(end, zero['zero_ms'] + delay_ms - LEAN_MARGIN_MS)
    ms = int(end - now_ms)
    if ms < LEAN_MIN_MS:
        return None
    return min(WATCH_MAX_MS, ms), not zero['observed']


def follow_plan(lead_ms, button_ready, age_ms, enter_ms):
    """plan_next's (watch ms, stop on a button change) or None; while the
    price button is awaited, the watches are fitted around the moment it is
    due (PRICE_BUTTON_LEAD_MS before the zero, i.e. enter_ms - that after the
    entry target) so that one of them spans it."""
    planned = plan_next(lead_ms, button_ready, age_ms)
    if planned is None or button_ready:
        return planned
    until_change = lead_ms + enter_ms - PRICE_BUTTON_LEAD_MS
    if until_change <= -BUTTON_SPAN_MS:
        return planned     # long past: it was missed, the next reads decide
    if until_change + BUTTON_SPAN_MS <= SPANNING_WATCH_MAX_MS:
        return max(500, int(until_change + BUTTON_SPAN_MS)), True
    cap = int(until_change - FOLLOW_READ_GAP_MS)
    if cap < planned[0]:
        return max(500, cap), planned[1]
    return planned


def button_change_since_publicity(changed, watch_events, states, previous_state=None):
    """A native button change counts as proof of the price only when seen
    after the last 公示中 read: the watch's events come before its final read,
    and every later read showing 公示中 clears them (review wf_993b38d0-6b8).
    Two full reads, 公示中 before the watch and the price at its end, prove
    the change too (it can come between two watches; a false one at worst
    presses 公示中: only the 订单尚未开放购买 toast, no dialog)."""
    if watch_events or (previous_state == 'publicity' and states and states[0] in ('price_ready', 'price_button')):
        changed = True
    if 'publicity' in states:
        changed = False
    return changed


def toast_events(watch, dispatched_ms):
    """The toast region's line reads in a watch after the press: when each
    change showed (the frame before it and its frame, ms after the press),
    its text and the result kinds in it."""
    from purchase_observation import result_kinds
    out = []
    for event in (watch or {}).get('events') or []:
        source, previous = event.get('source_mono_ms'), event.get('previous_source_mono_ms')
        text = normalize(event.get('text') or '')
        out.append(dict(kind=event.get('kind'), text=text, kinds=result_kinds(text), changed_px=event.get('changed_px'),
                        after_press_ms=None if type(source) not in (int, float) else source - dispatched_ms,
                        frame_before_after_press_ms=None if type(previous) not in (int, float) else previous - dispatched_ms))
    return out


def result_shown_after_press(result, read=False):
    """[frame before, frame] in ms after the press when the first result toast
    showed (read=True: when its text first read), from the dialog's watch or
    the one after Esc; None if neither saw one."""
    keys = ('result_read_frame_before_ms', 'result_read_after_press_ms') if read else \
        ('result_frame_before_ms', 'result_after_press_ms')
    for name in ('press_result', 'press_result_late'):
        timing = result.get(name) or {}
        if timing.get(keys[1]) is not None:
            return [timing.get(keys[0]), timing[keys[1]]]
    return None


def toast_timing(events):
    """When the toast with the first result showed: the first change of the
    unbroken burst of changes that ends in the read of its text (it slides
    and fades in over some 50-80 ms before its text reads; review
    wf_cf31784b-664), as the frame before it and its frame; and when its text
    first read. Also the region's first change at all."""
    change = next((e for e in events if e['kind'] == 'change'), None)
    index = next((i for i, e in enumerate(events) if e['kinds']), None)
    result = events[index] if index is not None else None
    onset = result
    if result is not None and result['kind'] == 'change':
        for earlier in reversed(events[:index]):
            if (earlier['kind'] != 'change' or onset['frame_before_after_press_ms'] is None
                    or earlier['after_press_ms'] is None
                    or onset['frame_before_after_press_ms'] - earlier['after_press_ms'] > TOAST_BURST_GAP_MS):
                break
            onset = earlier
    return dict(first_change_after_press_ms=change and change['after_press_ms'],
                first_change_frame_before_ms=change and change['frame_before_after_press_ms'],
                result_after_press_ms=onset and onset['after_press_ms'],
                result_frame_before_ms=onset and (None if onset['kind'] == 'baseline' else onset['frame_before_after_press_ms']),
                result_read_after_press_ms=result and result['after_press_ms'],
                result_read_frame_before_ms=result and result['frame_before_after_press_ms'],
                result_seen_at_watch_start=bool(result and onset['kind'] == 'baseline'),
                kinds=result['kinds'] if result else [], text=result['text'] if result else None)


def page_reread_due(after, rereads, lead_ms):
    """Read once more before judging the page: the last read could not place it."""
    return after['page'] == 'unknown' and rereads < PAGE_UNKNOWN_REREADS and lead_ms >= PAGE_REREAD_MIN_LEAD_MS


def last_line_state(watch):
    """What the dialog line showed at the end of a lean watch."""
    baseline, ticks = ticks_from_watch(watch)
    final = ticks[-1] if ticks else baseline
    return final['state']


def press_decision(zero, line_state, waited_from_ms, click_ms, now_ms, enter_at_ms, lead_ms=0):
    """None if the press may go out now (lead_ms: up to that much before its
    moment, the backend then waits the rest); otherwise why not."""
    if not zero['calibrated']:
        return 'DIALOG_ZERO_UNCALIBRATED'
    if line_state not in ('countdown', 'unlocked'):
        return 'DIALOG_LINE_NOT_READ'
    if click_ms - waited_from_ms > enter_at_ms + 2000:
        return 'PRESS_TOO_FAR'
    late = now_ms - click_ms
    if late < -lead_ms or late > PRESS_LATE_LIMIT_MS:
        return 'PRESS_WINDOW_MISSED:%.1f' % late
    return None


def last_line_seconds(watch):
    """The seconds the dialog line showed at the end of a lean watch, or None."""
    baseline, ticks = ticks_from_watch(watch)
    final = ticks[-1] if ticks else baseline
    return final.get('seconds') if final['state'] == 'countdown' else None


def probe_zero_ready(zero, footer_calibrated):
    """The probe's calibration: the dialog's own ticks (two, or one that
    agrees with a calibrated watchlist clock within DIALOG_ZERO_WINDOW_MS).
    A single tick misread by a whole second lands outside that window when
    late; when early it only moves the press further from the unlock."""
    return bool(footer_calibrated and zero['source'] == 'dialog' and zero.get('agrees_with_footer')
                and (zero['calibrated'] or zero.get('ticks_used', 0) >= 1))


def early_press_decision(zero, line_state, line_seconds, now_ms, lead_ms, *, footer_calibrated, watch_end_ms,
                         frame_ms=None, max_gap_ms=None):
    """None if the probe's single early press may go out now; otherwise why
    not (review 2026-10-09 wf_8755a34e-3bd: bounded on both sides, against a
    live reading).
    - the dialog's zero (its first 0分0秒 frame; the unlock follows about 1 s
      later) is calibrated against the watchlist clock and still ahead;
    - the last lean watch ended at most EARLY_LINE_MAX_AGE_MS ago, its last
      frame fresh and its frames without a long gap (the game not hung),
      still counting, showing exactly the seconds that zero implies (a zero
      a whole second late, or a constantly misread minute digit, does not);
    - the press is 0..PRESS_LATE_LIMIT_MS after its planned moment
      zero - lead_ms, and at least EARLY_PRESS_MIN_AHEAD_MS before the zero."""
    if not footer_calibrated:
        return 'FOOTER_UNCALIBRATED'
    if not probe_zero_ready(zero, footer_calibrated):
        return 'DIALOG_ZERO_UNCALIBRATED'
    if zero.get('observed'):
        return 'DIALOG_ZERO_ALREADY_SEEN'
    if line_state != 'countdown' or line_seconds is None:
        return 'DIALOG_LINE:%s/%s' % (line_state, line_seconds)
    if watch_end_ms is None or not 0 <= now_ms - watch_end_ms <= EARLY_LINE_MAX_AGE_MS:
        return 'LINE_READ_STALE:%s' % (None if watch_end_ms is None else round(now_ms - watch_end_ms))
    if (type(frame_ms) not in (int, float) or type(max_gap_ms) not in (int, float)
            or not 0 <= watch_end_ms - frame_ms <= EARLY_FRAME_MAX_AGE_MS or max_gap_ms > EARLY_MAX_FRAME_GAP_MS):
        return 'FRAMES_STALE:%s/%s' % (None if type(frame_ms) not in (int, float) else round(watch_end_ms - frame_ms),
                                       max_gap_ms)
    if line_seconds != math.ceil((zero['zero_ms'] - watch_end_ms) / 1000.0):
        return 'DIALOG_LINE_DISAGREES:%s' % line_seconds
    late = now_ms - (zero['zero_ms'] - lead_ms)
    if not 0 <= late <= PRESS_LATE_LIMIT_MS:
        return 'PRESS_WINDOW_MISSED:%.1f' % late
    if zero['zero_ms'] - now_ms < EARLY_PRESS_MIN_AHEAD_MS:
        return 'TOO_CLOSE_TO_ZERO:%.0f' % (zero['zero_ms'] - now_ms)
    return None


def outcome_of(screens):
    """The first purchase result shown after the press, or 'unknown'."""
    kinds = {signal['kind'] for screen in screens for signal in screen['signals']}
    for kind, outcome in OUTCOMES:
        if kind in kinds:
            return outcome
    return 'unknown'


class InputBudget:
    """Inputs outside the session: one pointer move onto the dialog button,
    at most one confirm press (only with --buy, or the early-press probe's
    single press before the unlock), and Esc presses."""

    def __init__(self, buy, early=False):
        if buy and early:
            raise ValueError('PURCHASE_TIMED_EARLY_PROBE_NEVER_BUYS')
        self.buy, self.early, self.hovered, self.confirmed = buy, early, False, False

    def hover(self, point):
        if point != DIALOG_BUY_POINT or self.hovered:
            raise ValueError('PURCHASE_TIMED_HOVER_NOT_ALLOWED')
        self.hovered = True

    def confirm(self, point):
        if not (self.buy or self.early) or point != DIALOG_BUY_POINT or self.confirmed or not self.hovered:
            raise ValueError('PURCHASE_TIMED_CONFIRM_NOT_ALLOWED')
        self.confirmed = True  # never a second press, whatever happens next


def settings_from(config_path, *, buy, enter_at=None, delay_ms=None, follow_limit=600, no_ntp=False,
                  cleanup_only=False, early_press_ms=None):
    """The attempt's settings: run_settings from the program's config, with
    optional overrides; validated."""
    if buy and cleanup_only:
        raise ValueError('PURCHASE_TIMED_CLEANUP_ONLY_NEVER_BUYS')
    enter_s, delay = read_delay_settings(config_path)
    if enter_at is not None:
        enter_s = enter_at
    if delay_ms is not None:
        delay = delay_ms
    if not 1.0 <= enter_s <= 5.0 or not 0 <= delay <= 60000 or not 10 <= follow_limit <= 600:
        raise ValueError('PURCHASE_TIMED_LIMITS')
    if buy and enter_s < 2.0:
        # Opening and reading the dialog takes about a second: with 1 s left
        # its countdown could not be calibrated before the press.
        raise ValueError('PURCHASE_TIMED_ENTER_TOO_LATE_FOR_CALIBRATION')
    if early_press_ms is not None:
        if buy or cleanup_only:
            raise ValueError('PURCHASE_TIMED_EARLY_PROBE_NEVER_BUYS')
        if (type(early_press_ms) is not int
                or not EARLY_PRESS_MIN_LEAD_MS <= early_press_ms <= EARLY_PRESS_MAX_LEAD_MS):
            raise ValueError('PURCHASE_TIMED_EARLY_PROBE_LEAD')
        if enter_s < EARLY_PRESS_MIN_ENTER_S:
            # Opening and reading the dialog, then a second boundary, come first.
            raise ValueError('PURCHASE_TIMED_EARLY_PROBE_ENTER_TOO_LATE')
    return SimpleNamespace(config=Path(config_path), buy=bool(buy), enter_s=float(enter_s), delay_ms=float(delay),
                           follow_limit=int(follow_limit), no_ntp=bool(no_ntp), cleanup_only=bool(cleanup_only),
                           early_press_ms=early_press_ms)


def run_attempt(settings, *, root, output, snapshot, backend, overlay, stop_requested=None, final_banner=True):
    """One purchase attempt in one foreground lease on the given backend:
    cleanup, follow, enter, calibrate, press (settings.buy) or rehearse.
    The banner `overlay` is the caller's (it is not closed here). Returns the
    result, also written to output/result.json. For the F2 cycle the result
    says when the watchlist was empty (WATCHLIST_EMPTY) or the head is still
    far from its unlock (TOO_EARLY, with result['head']). With final_banner
    False (the cycle) a failed attempt leaves the banner to the caller.
    `stop_requested` is checked before every input, also inside the dialog,
    up to the press itself; after a stop only Esc is sent."""
    args = settings
    enter_s, delay_ms = settings.enter_s, settings.delay_ms
    enter_at_ms = enter_s * 1000.0
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    early_ms = getattr(settings, 'early_press_ms', None)
    if early_ms is not None and args.buy:
        raise ValueError('PURCHASE_TIMED_EARLY_PROBE_NEVER_BUYS')
    result = dict(mode=('purchase_timed_buy' if args.buy else
                        'purchase_early_press_probe' if early_ms is not None else 'purchase_timed_rehearsal'),
                  buy=args.buy, enter_before_s=enter_s, purchase_delay_ms=delay_ms, price_button_clicks=0,
                  confirm_clicks=0, early_clicks=0, early_press_ms=early_ms, status='blocked', system_time_changed=False)
    from collection_live_session import ForegroundSession, WindowsBackend
    plan = dict(armed=False, hovered=False, clicked=False)
    cleanup_plan = dict(star_armed=False, refresh_armed=False)
    sort_plan = dict(sort_armed=False)
    budget = InputBudget(args.buy, early=early_ms is not None)

    class TimedSession(ForegroundSession):
        def perform(self, step, remaining=None):
            if not (sort_step_permitted(step, sort_plan) or cleanup_step_permitted(step, cleanup_plan)):
                preentry_step_permitted(step, plan)
            return super().perform(step, remaining)

    session = None
    clock, dialog_clock = CountdownClock(), CountdownClock()
    mode_text = '定时购买' if args.buy else '小窗按早测试' if early_ms is not None else '购买演练'
    banner = dict(zero_ms=None, ticker=None, counts=None)

    def start_banner(counts):
        banner['counts'] = counts
        banner['ticker'] = BannerTicker(overlay, lambda: banner_text(
            banner['counts'], delay_ms, None if banner['zero_ms'] is None else banner['zero_ms'] - qpc_ms(),
            not args.buy, note=banner.get('note'))).start()

    def stop_banner():
        if banner['ticker'] is not None:
            banner['ticker'].stop()
            result['banner_errors'] = banner['ticker'].errors
            banner['ticker'] = None
    overlay.show('%s：准备中…' % mode_text)
    watches, dialog_reads, dialog_watches, cleaned = [], [], [], []

    def read_dialog(name, preview=True, direct=False):
        if direct:
            packet = direct_packet()
            received = qpc_ms()
        else:
            session.perform(dict(kind='capture', collection_observation=True, ui_regions=True,
                                 purchase_observation=True, preview=preview))
            received = qpc_ms()
            packet = session.previous
            if session.last_preview:
                (output / (name + '.png')).write_bytes(base64.b64decode(session.last_preview))
        screen = read_screen(packet)
        dialog_reads.append(dict(name=name, received_qpc_ms=received, page=screen['page'], overlay=screen['overlay'],
                                 signals=screen['signals'], dialog_line=screen['dialog_line'],
                                 dialog_countdown_seconds=screen['dialog_countdown_seconds'], texts=dialog_texts(screen)))
        return screen

    def direct_packet():
        """A read outside the session's steps: no stop or step gate (Esc
        verification after a stop, the outcome after a press)."""
        step = dict(kind='capture', collection_observation=True, ui_regions=True, purchase_observation=True)
        process = backend.capture(session._command(step), 15)
        return json.loads(process.stdout)

    def direct_read():
        return read_screen(direct_packet())

    def lean_dialog_watch(ms, stop_on_zero=False, area='dialog', stop_on_text=None):
        """The dialog's countdown line (or the result toast) on every frame,
        no full OCR: returns within a few ms of its end (or of its 0分0秒
        frame). Read-only, outside the session's steps."""
        command = [str(root / 'dist/RelinkStudio/RelinkStudio.exe'), '--live-capture-check', '--focus-policy', 'caller-owned']
        command += session._identity_arguments()
        command += ['--frames', '1', '--purchase-countdown-watch', str(int(ms)), '--purchase-countdown-area', area,
                    '--purchase-countdown-lean', '--reuse-capture-resources', '--collection-ready-stream']
        if stop_on_zero:
            command.append('--purchase-countdown-stop-on-zero')
        if stop_on_text:
            command += ['--purchase-countdown-stop-on-text', '|'.join(stop_on_text)]
        process = WindowsBackend.capture(backend, command, max(2.0, ms / 1000.0 + 3.0))
        packet = json.loads(process.stdout)
        if process.returncode != 0 or packet.get('capture_passed') is not True or 'purchase_countdown_watch' not in packet:
            raise ValueError('PURCHASE_TIMED_DIALOG_WATCH:' + str(packet.get('error')))
        return packet['purchase_countdown_watch']

    def close_dialog(expect):
        out = dict(escapes=0, checks=[], verified=None)

        def escape():
            try:
                backend.key(ESCAPE)
                out['escapes'] += 1
            except Exception as error:
                out.setdefault('escape_errors', []).append(str(error) or type(error).__name__)
            time.sleep(.6)

        def check(name):
            screen, errors = None, []
            gated = not (getattr(session, '_failed', False) or (stop_requested is not None and stop_requested()))
            for read in ((lambda: read_dialog(name)) if gated else None, direct_read):
                if read is None:
                    continue
                try:
                    screen = read()
                    break
                except Exception as error:
                    errors.append(str(error) or type(error).__name__)
            if screen is None:
                out['checks'].append(dict(name=name, error=';'.join(errors)))
                return None
            out['checks'].append(dict(name=name, open=dialog_open(screen), closed=dialog_closed(screen),
                                      signals=[s['kind'] for s in screen['signals']]))
            return screen

        if expect:
            escape()
        screen = check('after_escape' if expect else 'final_check')
        for attempt in range(2):
            if not ((expect and (screen is None or not dialog_closed(screen)))
                    or (not expect and screen is not None and dialog_open(screen))):
                break
            escape()
            screen = check('after_escape_%d' % (attempt + 2))
        out['verified'] = None if screen is None else dialog_closed(screen)
        return out

    def watch_toast(name, ms, screens):
        """The toast region frame by frame after the press (no input); its
        result kinds join `screens`. Recorded under result[name]."""
        dispatched = (result.get('confirm') or {}).get('dispatch_qpc_ms')
        try:
            # Ends two frames after a final result reads (the dialog closes, the next listing waits).
            watch = lean_dialog_watch(ms, area='toast', stop_on_text=final_result_phrases())
            events = toast_events(watch, dispatched or 0.0)
            result[name] = dict(toast_timing(events), watch_ms=ms, events=events[:40],
                                first_frame_after_press_ms=(watch.get('first_source_mono_ms') or 0) - (dispatched or 0),
                                last_frame_after_press_ms=(watch.get('last_source_mono_ms') or 0) - (dispatched or 0),
                                frames=watch.get('frames_examined'), max_frame_gap_ms=watch.get('max_frame_gap_ms'),
                                line_reads=watch.get('line_reads'), ended_by=watch.get('ended_by'))
            screens.append(dict(signals=[dict(kind=k) for e in events for k in e['kinds']]))
        except Exception as error:
            result[name] = dict(error=str(error) or type(error).__name__)

    def late_result():
        """After a real press and Esc: the result may come late (buy01). Read
        the watchlist, no input, until a result shows; then write the ledger."""
        try:
            kinds = [k for check in ((result.get('close') or {}).get('checks') or []) for k in check.get('signals', [])]
            outcome = result.get('outcome') or 'unknown'
            if outcome == 'unknown':
                outcome = outcome_of([dict(signals=[dict(kind=k) for k in kinds])])
            if outcome == 'unknown' and not (stop_requested is not None and stop_requested()):
                # Frame by frame first: when the answer shows matters (server latency).
                late = []
                watch_toast('press_result_late', LATE_TOAST_WATCH_MS, late)
                outcome = outcome_of(late)
            for index in range(LATE_RESULT_READS):
                if outcome != 'unknown' or (stop_requested is not None and stop_requested()):
                    break
                time.sleep(.2)
                try:
                    screen = read_dialog('late_result_%d' % index, direct=True)
                except Exception as error:
                    result.setdefault('outcome_read_errors', []).append(str(error) or type(error).__name__)
                    continue
                outcome = outcome_of([screen])
            result['outcome'] = outcome
            overlay.show(OUTCOME_TEXT.get(outcome, outcome), 'ok' if outcome == 'bought' else 'warn')
        finally:
            record_press()

    def record_press():
        """Every press that went out gets its ledger line (已抢 counts it when bought)."""
        if result.get('ledger_entry') is not None or result['confirm_clicks'] != 1:
            return
        title = (result.get('identity') or {}).get('product_title')
        grade = grade_of(title, snapshot)
        result['ledger_entry'] = ledger_append(ledger_path(settings.config), dict(
            title=title, grade=grade, grade_key=grade_key(grade), outcome=result.get('outcome') or 'unknown',
            delay_ms=delay_ms,
            wear=(result.get('identity') or {}).get('selected_detail_precise'),
            press_late_ms=(result.get('confirm') or {}).get('late_ms'),
            # When the answer showed after the press (server latency): the frame before it and its frame;
            # and when its text first read.
            result_after_press_ms=result_shown_after_press(result),
            result_read_after_press_ms=result_shown_after_press(result, read=True)))

    def read_after_press(direct):
        """The result toast shows for about 1.2 s (live toast04: read at 0 and
        0.58 s, gone at 1.18 s): read back to back, no pause."""
        screens = []
        for index in range(AFTER_PRESS_READS):
            try:
                screens.append(read_dialog('after_press_%d' % index, preview=index < 3 and not direct, direct=direct))
            except Exception as error:
                result.setdefault('outcome_read_errors', []).append(str(error) or type(error).__name__)
        return screens

    def early_press_phase(prior, footer_zero_ms, footer_calibrated, dialog_rest):
        """The probe: calibrate the dialog's zero with lean watches that end
        before the planned early press, press ONCE at zero - early_ms, read
        the toast. Refuses (no press) on any doubt."""
        if not footer_calibrated:
            raise ValueError('PURCHASE_EARLY_NO_PRESS:FOOTER_UNCALIBRATED')
        line_state, line_seconds, watch_end, frame_end, max_gap = 'unread', None, None, None, None
        for index in range(20):
            check_stop(stop_requested)
            zero = dialog_zero(dialog_clock, prior, footer_zero_ms, footer_calibrated)
            if zero['observed']:
                break
            # Watch to just before the press; until calibrated, a little earlier.
            slack = 0 if probe_zero_ready(zero, footer_calibrated) else EARLY_FIRST_WATCH_SLACK_MS
            ms = int(zero['zero_ms'] - early_ms - LEAN_MARGIN_MS - slack - qpc_ms())
            if ms < LEAN_MIN_MS:
                break   # at the press (calibrated), or past the last boundary before it (refused below)
            watch = lean_dialog_watch(min(WATCH_MAX_MS, ms))
            watch_end = qpc_ms()
            frame_end, max_gap = watch.get('last_source_mono_ms'), watch.get('max_frame_gap_ms')
            estimate = dialog_clock.add_watch(watch)
            line_state, line_seconds = last_line_state(watch), last_line_seconds(watch)
            dialog_watches.append(dict(requested_ms=ms, line_state=line_state, line_seconds=line_seconds,
                received_qpc_ms=qpc_ms(), display_zero=estimate['display_zero'],
                watch=dict(events=[dict(text=e.get('text'), source_mono_ms=e.get('source_mono_ms'),
                                        previous_source_mono_ms=e.get('previous_source_mono_ms')) for e in watch.get('events', [])],
                           covered_ms=watch.get('covered_ms'), ended_by=watch.get('ended_by'))))
        zero = dialog_zero(dialog_clock, prior, footer_zero_ms, footer_calibrated)
        target = zero['zero_ms'] - early_ms
        result['dialog_zero'] = dict(zero, prior_mono_ms=prior, early_press_planned_qpc_ms=target,
                                     probe_ready=probe_zero_ready(zero, footer_calibrated))
        left = target - qpc_ms()
        if left > EARLY_LINE_MAX_AGE_MS:
            # The last watch must end right before the press: never wait long on an old reading.
            raise ValueError('PURCHASE_EARLY_NO_PRESS:PRESS_TOO_FAR:%.0f' % left)
        wait_until_or_stop(target, stop_requested)
        check_stop(stop_requested)
        now = qpc_ms()
        refused = early_press_decision(zero, line_state, line_seconds, now, early_ms,
                                       footer_calibrated=footer_calibrated, watch_end_ms=watch_end,
                                       frame_ms=frame_end, max_gap_ms=max_gap)
        if refused is None and backend._cursor() != dialog_rest:
            refused = 'POINTER_MOVED'
        result['early_press'] = dict(allowed=refused is None, reason=refused, lead_ms=early_ms,
                                     ahead_of_zero_ms=zero['zero_ms'] - now, line_state=line_state,
                                     line_seconds=line_seconds,
                                     line_age_ms=None if watch_end is None else now - watch_end,
                                     frame_age_ms=None if None in (watch_end, frame_end) else watch_end - frame_end,
                                     max_frame_gap_ms=max_gap)
        if refused:
            raise ValueError('PURCHASE_EARLY_NO_PRESS:' + refused)

        def dispatch_guard():
            # Right before SendInput: still on time, still far from the zero.
            t = qpc_ms()
            if t - target > EARLY_DISPATCH_LATE_LIMIT_MS or zero['zero_ms'] - t < EARLY_PRESS_MIN_AHEAD_MS:
                raise ValueError('PURCHASE_EARLY_NO_PRESS:DISPATCH_LATE:%.0f' % (t - target))
        check_stop(stop_requested)
        budget.confirm(DIALOG_BUY_POINT)
        backend.click(DIALOG_BUY_POINT, before_dispatch=dispatch_guard)
        sent = backend.last_dispatch if isinstance(backend.last_dispatch, dict) else {}
        result['early_clicks'] = 1 if (sent.get('returned_events') or 0) >= 1 else 0
        dispatched = sent.get('started_qpc_ms') or now
        result['early_press'].update(dispatch_qpc_ms=dispatched, dispatched_ahead_of_zero_ms=zero['zero_ms'] - dispatched)
        stop_banner()
        overlay.show('小窗按早测试：归零前 %d ms 已按下' % round(zero['zero_ms'] - dispatched), 'warn')
        screens = read_after_press(direct=False)   # with preview images
        result['outcome'] = outcome_of(screens)
        result['toasts_after_press'] = sorted({t['text'] for screen in screens for t in screen['toast']})
        if result['early_clicks'] != 1:
            raise ValueError('PURCHASE_EARLY_PRESS_NOT_DISPATCHED')
        if result['outcome'] not in ('not_open_yet', 'still_publicity', 'unknown'):
            # Must never happen: then it is on record and the run says so.
            title = (result.get('identity') or {}).get('product_title')
            grade = grade_of(title, snapshot)
            result['ledger_entry'] = ledger_append(ledger_path(settings.config), dict(
                title=title, grade=grade, grade_key=grade_key(grade), outcome=result['outcome'], early_probe=True,
                wear=(result.get('identity') or {}).get('selected_detail_precise')))
            overlay.show('小窗按早测试：结果是 %s，请检查' % OUTCOME_TEXT.get(result['outcome'], result['outcome']),
                         'error', ttl_ms=15000)
            raise ValueError('PURCHASE_EARLY_PROBE_UNEXPECTED_OUTCOME:' + result['outcome'])

    def dialog_phase(footer_zero_ms, footer_calibrated, footer_window):
        """After the dialog opened: verify it, pointer onto the green button,
        calibrate, decide, press (only --buy). Raises on a refusal.
        footer_window: (earliest, latest) watchlist zero."""
        opened = None
        for attempt in range(3):
            opened = read_dialog('dialog_open_%d' % attempt, preview=attempt == 0)
            if dialog_open(opened):
                break
        if not dialog_open(opened):
            raise ValueError('PURCHASE_TIMED_DIALOG_NOT_OPEN')
        problem = purchase_dialog_problem(opened)
        if problem:
            raise ValueError('PURCHASE_TIMED_' + problem)
        result['dialog_matches_listing'] = dialog_matches(opened, result.get('identity', {}))  # recorded only
        check_stop(stop_requested)
        budget.hover(DIALOG_BUY_POINT)
        backend.hover(DIALOG_BUY_POINT)
        dialog_rest = backend._cursor()
        prior = footer_zero_ms + DIALOG_PHASE_PRIOR_MS
        hold = press_hold_ms(delay_ms)
        line_state = 'unread'
        if early_ms is not None:
            return early_press_phase(prior, footer_zero_ms, footer_calibrated, dialog_rest)
        earliest = footer_window[0] + DIALOG_ZERO_WINDOW_MS[0]
        latest = footer_window[1] + DIALOG_ZERO_WINDOW_MS[1]
        for index in range(DIALOG_WATCH_ROUNDS):
            check_stop(stop_requested)
            zero = dialog_zero(dialog_clock, prior, footer_zero_ms, footer_calibrated, (earliest, latest))
            # A real press goes down the hold before its moment: the last watch ends before that.
            planned = next_dialog_watch(qpc_ms(), zero, latest, delay_ms - (hold if args.buy else 0.0))
            if planned is None:
                break
            ms, stop_on_zero = planned
            watch = lean_dialog_watch(ms, stop_on_zero)
            estimate = dialog_clock.add_watch(watch, window=(earliest, latest))
            earliest = earliest_dialog_zero(earliest, watch, latest)
            line_state = last_line_state(watch)
            seen = dialog_zero(dialog_clock, prior, footer_zero_ms, footer_calibrated, (earliest, latest))
            if seen['source'] == 'dialog':
                banner['zero_ms'] = seen['zero_ms']  # 倒计时监控 follows the dialog's own countdown
                banner['note'] = dialog_clock_note(seen.get('offset_from_footer_ms') if footer_calibrated else None)
            dialog_watches.append(dict(requested_ms=ms, stop_on_zero=stop_on_zero, line_state=line_state,
                received_qpc_ms=qpc_ms(), earliest_zero_ms=earliest, frames=watch.get('frames_examined'),
                max_frame_gap_ms=watch.get('max_frame_gap_ms'), time_rounding=watch.get('time_rounding'), watch=dict(events=[dict(text=e.get('text'), source_mono_ms=e.get('source_mono_ms'),
                                        previous_source_mono_ms=e.get('previous_source_mono_ms')) for e in watch.get('events', [])],
                           covered_ms=watch.get('covered_ms'), ended_by=watch.get('ended_by')),
                display_zero=estimate['display_zero']))
            if dialog_clock.unlock()['observed']:
                break
        zero = dialog_zero(dialog_clock, prior, footer_zero_ms, footer_calibrated, (earliest, latest))
        click_ms = zero['zero_ms'] + delay_ms
        result['dialog_zero'] = dict(zero, prior_mono_ms=prior, click_planned_qpc_ms=click_ms,
                                     earliest_zero_ms=earliest, latest_zero_ms=latest)
        waited_from = qpc_ms()
        lead = (hold + PRESS_SPIN_LEAD_MS) if args.buy else 0
        if click_ms - waited_from <= enter_at_ms + 2000:
            wait_until_or_stop(click_ms - lead, stop_requested)
        check_stop(stop_requested)
        now = qpc_ms()
        refused = press_decision(zero, line_state, waited_from, click_ms, now, enter_at_ms, lead_ms=lead)
        if refused is None and backend._cursor() != dialog_rest:
            refused = 'POINTER_MOVED'
        result['press_decision'] = dict(allowed=refused is None, reason=refused, late_ms=now - click_ms)
        if args.buy:
            if refused:
                raise ValueError('PURCHASE_TIMED_NO_PRESS:' + refused)

            def press_guard():
                # After the pointer and window checks: still on time, and the rest is a short spin.
                late = qpc_ms() - click_ms
                if not -lead - 1 <= late <= PRESS_LATE_LIMIT_MS:
                    result['press_decision'].update(allowed=False, reason='PRESS_WINDOW_MISSED:%.1f' % late,
                                                    dispatch_late_ms=late)
                    raise ValueError('PURCHASE_TIMED_NO_PRESS:PRESS_WINDOW_MISSED:%.1f' % late)
            check_stop(stop_requested)
            budget.confirm(DIALOG_BUY_POINT)
            try:
                backend.click(DIALOG_BUY_POINT, before_dispatch=press_guard, at_qpc_ms=click_ms,
                              late_limit_ms=PRESS_LATE_LIMIT_MS, hold_ms=hold)
            except RuntimeError as error:
                if not str(error).startswith('PURCHASE_PRESS_'):
                    raise
                # The backend's own last timing gate refused it (review wf_7aabd0b3-24a): no input went out.
                late = qpc_ms() - click_ms
                result['press_decision'].update(allowed=False, reason='PRESS_WINDOW_MISSED:%.1f' % late,
                                                dispatch_late_ms=late, backend_code=str(error))
                raise ValueError('PURCHASE_TIMED_NO_PRESS:PRESS_WINDOW_MISSED:%.1f' % late) from error
            stop_banner()
            overlay.show('已点击购买（归零后 %g ms 松开）' % delay_ms, 'ok')
            sent = backend.last_dispatch if isinstance(backend.last_dispatch, dict) else {}
            result['confirm_clicks'] = 1 if (sent.get('returned_events') or 0) >= 1 else 0
            dispatched = sent.get('started_qpc_ms') or now
            returned = sent.get('returned_qpc_ms')
            result['confirm'] = dict(planned_qpc_ms=click_ms, dispatch_qpc_ms=dispatched, late_ms=dispatched - click_ms,
                                     after_dialog_zero_ms=dispatched - zero['zero_ms'], returned_qpc_ms=returned,
                                     returned_events=sent.get('returned_events'),
                                     sendinput_ms=None if returned is None else returned - dispatched,
                                     down_ms=None if None in (sent.get('down_returned_qpc_ms'), sent.get('down_started_qpc_ms'))
                                     else sent['down_returned_qpc_ms'] - sent['down_started_qpc_ms'],
                                     hold_ms=sent.get('hold_ms'),
                                     up_ms=None if None in (returned, sent.get('up_started_qpc_ms'))
                                     else returned - sent['up_started_qpc_ms'],
                                     zero_uncertainty_ms=zero.get('uncertainty_ms'), zero_ticks_used=zero.get('ticks_used'))
            if result['confirm_clicks'] != 1:
                raise ValueError('PURCHASE_TIMED_PRESS_NOT_DISPATCHED')
        else:
            stop_banner()
            overlay.show('演练：到点%s（偏差 %.1f ms）' % ('可以按' if refused is None else '不会按', now - click_ms),
                         'ok' if refused is None else 'warn')
            # Rehearsal: keep watching to the unlock, where the press would have landed.
            left = zero['zero_ms'] + 1600 - qpc_ms()
            if left >= LEAN_MIN_MS and not dialog_clock.unlock()['observed']:
                dialog_clock.add_watch(lean_dialog_watch(min(WATCH_MAX_MS, int(left))))
        screens = []
        if args.buy:
            watch_toast('press_result', TOAST_WATCH_MS, screens)
        try:
            # The result toast shows for about 1.2 s (live toast04: read at 0 and
            # 0.58 s, gone at 1.18 s): read back to back, no pause.
            for index in range(BUY_DIALOG_READS if args.buy else AFTER_PRESS_READS):
                if args.buy and outcome_of(screens) != 'unknown':
                    break
                try:
                    screens.append(read_dialog('after_press_%d' % index, preview=not args.buy and index < 3,
                                               direct=args.buy))
                except Exception as error:
                    if not args.buy:
                        raise
                    result.setdefault('outcome_read_errors', []).append(str(error) or type(error).__name__)
                if args.buy and outcome_of(screens) != 'unknown':
                    break
        finally:
            result['outcome'] = outcome_of(screens) if args.buy else 'rehearsal'
            # The ledger line is written once the late reads after Esc are done (or in run_attempt's finally).
        if not args.buy and refused:
            raise ValueError('PURCHASE_TIMED_REHEARSAL_WOULD_NOT_PRESS:' + refused)

    try:
        if stop_requested is not None and stop_requested():
            getattr(backend, 'discard', lambda: None)()
            raise RuntimeError(STOP)
        result['system_clock'] = None if args.no_ntp else system_clock_check()
        session = TimedSession(output / 'session.json', backend=backend, timeout_seconds=args.follow_limit + 120,
                               max_steps=ATTEMPT_MAX_STEPS, numeric_price=True, fast_settle=False,
                               stop_requested=stop_requested)
        with session:
            overlay.monitor(backend.identities.get('target_hwnd'))
            page = navigate(session)
            if page != 'watchlist_listings':
                result['head'] = dict(state='empty' if page == 'empty_watchlist' else page)
                raise ValueError('PURCHASE_TIMED_WATCHLIST_EMPTY')
            # 0. 按稀有度升序, so the listings that unlock soonest come first (user 2026-10-10).
            def sort_read():
                session.perform(dict(kind='capture', collection_observation=True, ui_regions=True,
                                     purchase_observation=True))
                return read_screen(session.previous), session.previous
            sort_record = result['sort'] = {}
            sort_record['outcome'] = ensure_rarity_sort(
                session, backend, sort_read, sort_plan, sort_record,
                on_event=lambda kind: overlay.show('我的关注：排序切换为按稀有度升序'),
                check_stop=lambda: check_stop(stop_requested))
            # Expired listings off the head: 刷新 (then 取消收藏 + 刷新).
            read_started = [qpc_ms()]

            def head_read():
                read_started[0] = qpc_ms()
                session.perform(dict(kind='capture', collection_observation=True, ui_regions=True, collection_layout=True,
                                     collection_numeric_price=True, purchase_observation=True))
                return read_screen(session.previous), session.previous

            def cleanup_event(kind, identity):
                title = identity.get('product_title') or '第一位'
                if kind == 'refresh':
                    overlay.show('清理过期：%s 已过公示期，刷新' % title, 'warn')
                elif kind == 'unfavorite':
                    overlay.show('清理过期：%s 已过公示期，取消收藏并刷新' % title, 'warn')
                elif kind == 'refresh_only':
                    overlay.show('清理过期：%s 已取消收藏，刷新移除' % title, 'warn')
                else:
                    overlay.show('已移除过期：%s' % title)

            head, screen, first = clean_expired_head(
                session, head_read, listing_identity, same_listing, cleanup_plan, cleaned, on_event=cleanup_event,
                first_card=lambda s, p: first_card_selected(p) or only_listing_in_first_slot(s, p))
            result['cleanup'] = dict(head=head, removed=cleaned)
            result['head'] = dict(state=head)
            if args.cleanup_only and head not in ('counting', 'empty'):
                raise ValueError('PURCHASE_CLEANUP_HEAD:' + head)
            if not args.cleanup_only:
                if head == 'empty':
                    raise ValueError('PURCHASE_TIMED_WATCHLIST_EMPTY_AFTER_CLEANUP')
                if head != 'counting':
                    raise ValueError('PURCHASE_TIMED_HEAD_STATE:' + head)
                sample_started = read_started[0]
                identity = listing_identity(first)   # recorded and shown; the right panel is what counts
                result['identity'] = identity
                if screen['countdown_seconds'] is None:
                    raise ValueError('PURCHASE_TIMED_NOT_IN_PUBLIC_NOTICE:' + str(screen['button_state']))
                result['head'].update(countdown_seconds=screen['countdown_seconds'],
                                      frame_qpc_ms=screen['source_frame']['source_mono_ms'],
                                      title=identity.get('product_title'))
                if screen['countdown_seconds'] > args.follow_limit:
                    raise ValueError('PURCHASE_TIMED_TOO_EARLY:%d' % screen['countdown_seconds'])
                read_seconds, read_frame = screen['countdown_seconds'], screen['source_frame']['source_mono_ms']
                after, packet, button_ready, changed = screen, first, False, False
                banner['zero_ms'] = read_frame + (read_seconds - .5) * 1000.0
                start_banner(ledger_counts(ledger_path(settings.config)))
                result['grade'] = grade_of(identity.get('product_title'), snapshot)
                last_watch_start = sample_started
                # 1. Follow the watchlist countdown until the set remaining time.
                for index in range(200):
                    target, zero = enter_target(clock, read_seconds, read_frame, enter_at_ms)
                    lead = target - qpc_ms()
                    if zero is not None and qpc_ms() > zero['upper_mono_ms'] + 2500 and not button_ready:
                        raise ValueError('PURCHASE_TIMED_PRICE_BUTTON_NEVER_APPEARED')
                    planned = follow_plan(lead, button_ready, max(target, qpc_ms() + HOVER_MARGIN_MS) - last_watch_start,
                                          enter_at_ms)
                    if planned is None:
                        break
                    ms, stop = planned
                    step = watch_step(ms)
                    # stop_on_jump: another listing in the panel ends the watch at once (user 2026-10-10),
                    # also when it was selected between two watches (against the followed zero).
                    followed = clock.estimate()['display_zero']
                    step.update(stop_on_button=stop, stop_on_green=stop, stop_on_jump=True,
                                expect_zero_ms=(followed['estimate_mono_ms'] if followed
                                                else read_frame + (read_seconds - .5) * 1000.0))
                    last_watch_start = qpc_ms()
                    state_before = after['button_state']
                    session.perform(step)
                    packet = session.previous
                    after = read_screen(packet)
                    record = dict(requested_ms=ms, final_button_state=after['button_state'],
                                  final_countdown_seconds=after['countdown_seconds'], watch=watch_summary(packet),
                                  button_events=packet['purchase_countdown_watch'].get('button_events', []),
                                  button_mean_bgr=packet['purchase_countdown_watch'].get('final_button_mean_bgr'))
                    watches.append(record)
                    watch, watch_after, rereads = packet['purchase_countdown_watch'], after, []
                    while page_reread_due(after, len(rereads), target - qpc_ms()):
                        session.perform(dict(kind='capture', collection_observation=True, ui_regions=True,
                                             purchase_observation=True))
                        packet = session.previous
                        after = read_screen(packet)
                        rereads.append(after)
                        watches.append(dict(page_reread=True, page=after['page'], final_button_state=after['button_state'],
                                            final_countdown_seconds=after['countdown_seconds']))
                    if rereads:
                        result['page_rereads'] = result.get('page_rereads', 0) + len(rereads)
                    if after['page'] not in WATCH_PAGES or after['overlay'] != 'none':
                        raise ValueError('PURCHASE_TIMED_WATCH_PAGE:' + str(after['page']))
                    estimate = clock.estimate()['display_zero']
                    frame_ms = after['source_frame']['source_mono_ms']
                    zero_ms = estimate['estimate_mono_ms'] if estimate else read_frame + (read_seconds - .5) * 1000.0
                    new_identity = listing_identity(packet)
                    wear_changed, jumped = switch_evidence(identity, new_identity, after, watch, zero_ms,
                                                           watch_anchor_seconds=watch_after['countdown_seconds'])
                    if wear_changed or jumped:
                        # Another listing in the panel: follow that one from its own reading.
                        switches = result.setdefault('head_switches', [])
                        switches.append(dict(previous=identity, now=new_identity, page=after['page'],
                                             wear_changed=wear_changed, jumped=jumped,
                                             countdown_seconds=after['countdown_seconds'], frame_qpc_ms=frame_ms))
                        if after['page'] != 'watchlist_listings':
                            # Possibly not 我的关注 at all (a market list): never followed.
                            raise ValueError('PURCHASE_TIMED_HEAD_SWITCHED:PAGE')
                        if after['countdown_seconds'] is None:
                            raise ValueError('PURCHASE_TIMED_HEAD_SWITCHED:' + str(after['button_state']))
                        if len(switches) > MAX_HEAD_SWITCHES:
                            raise ValueError('PURCHASE_TIMED_HEAD_SWITCHED_TOO_OFTEN')
                        identity = new_identity
                        result['identity'] = identity
                        result['head'].update(countdown_seconds=after['countdown_seconds'], frame_qpc_ms=frame_ms,
                                              title=identity.get('product_title'))
                        # The new one needs the follow window and the session's time like any head.
                        new_zero = frame_ms + (after['countdown_seconds'] - .5) * 1000.0
                        needed_s = (new_zero - qpc_ms()) / 1000.0 + enter_s + delay_ms / 1000.0 + 15
                        if after['countdown_seconds'] > args.follow_limit or needed_s > session.remaining_seconds():
                            raise ValueError('PURCHASE_TIMED_TOO_EARLY:%d' % after['countdown_seconds'])
                        clock = CountdownClock()
                        read_seconds, read_frame = after['countdown_seconds'], frame_ms
                        banner['zero_ms'] = read_frame + (read_seconds - .5) * 1000.0
                        button_ready, changed = False, False
                        overlay.show('第一位换成了 %s，继续跟这一把' % (identity.get('product_title') or '另一把'), 'warn')
                        continue
                    clock.add_watch(watch, anchor_seconds=watch_after['countdown_seconds'])
                    estimate = clock.estimate()['display_zero']
                    if estimate is not None:
                        banner['zero_ms'] = estimate['estimate_mono_ms']
                    changed = button_change_since_publicity(changed, record['button_events'],
                                                            [watch_after['button_state']] + [r['button_state'] for r in rereads],
                                                            previous_state=state_before)
                    button_ready = button_ready_now(after, packet, changed)
                refused = timed_entry_allowed(after, packet, changed) or entry_page_problem(
                    after, identity, listing_identity(packet))
                if refused:
                    raise ValueError('PURCHASE_TIMED_NOT_ENTERED:' + refused)
                if len(session.steps) > ATTEMPT_MAX_STEPS - DIALOG_RESERVED_STEPS:
                    raise ValueError('PURCHASE_TIMED_STEP_BUDGET:%d' % len(session.steps))
                # 2. Open the dialog at the set remaining time.
                target, zero = enter_target(clock, read_seconds, read_frame, enter_at_ms)
                plan['armed'] = True
                session.perform(dict(kind='hover', point=BUY_POINT, viewport=VIEWPORT, expected_before=after['page']))
                rest = backend._cursor()
                wait_until_or_stop(target, stop_requested)
                if backend._cursor() != rest:
                    raise ValueError('PURCHASE_TIMED_POINTER_MOVED')
                footer_zero = clock.estimate()['display_zero']
                footer_zero_ms = footer_zero['estimate_mono_ms'] if footer_zero else target + enter_at_ms
                # Uncalibrated: the single read's window (enter_target plans from its middle).
                footer_window = ((footer_zero['lower_mono_ms'], footer_zero['upper_mono_ms']) if footer_zero
                                 else (footer_zero_ms - 500, footer_zero_ms + 501))
                dialog_error, entry_sent = None, False
                try:
                    try:
                        session.perform(dict(kind='click', point=BUY_POINT, viewport=VIEWPORT, expected_before=after['page']))
                    finally:
                        dispatch = backend.last_dispatch if isinstance(backend.last_dispatch, dict) else None
                        entry_sent = bool(dispatch and dispatch.get('kind') == 'click' and (dispatch.get('returned_events') or 0) >= 1)
                        if entry_sent:
                            result['price_button_clicks'] = 1
                            result['entry'] = dict(target_qpc_ms=target, dispatch_qpc_ms=dispatch.get('started_qpc_ms'),
                                                   remaining_at_entry_ms=footer_zero_ms - (dispatch.get('started_qpc_ms') or target))
                    if not entry_sent:
                        raise ValueError('PURCHASE_TIMED_ENTRY_NOT_DISPATCHED')
                    dialog_phase(footer_zero_ms, footer_zero is not None, footer_window)
                except BaseException as error:
                    dialog_error = str(error) or type(error).__name__
                    if not isinstance(error, Exception):
                        result['interrupted'] = True
                finally:
                    # After an entry click, always close (Esc never confirms; 充值 is never clicked).
                    result['close'] = close_dialog(expect=entry_sent) if entry_sent else None
                    if args.buy and result['confirm_clicks'] == 1:
                        late_result()
                summary = dialog_clock.summary()
                result['dialog_clock'] = summary
                if summary['unlock'].get('observed') and result.get('dialog_zero'):
                    unlock = summary['unlock']
                    result['unlock_after_dialog_zero_ms'] = dict(lower=unlock['before_mono_ms'] - result['dialog_zero']['zero_ms'],
                                                                 upper=unlock['after_mono_ms'] - result['dialog_zero']['zero_ms'])
                if dialog_error:
                    raise (RuntimeError if dialog_error == STOP else ValueError)(dialog_error)
                if result['close'] is None or result['close']['verified'] is not True:
                    close = result['close'] or {}
                    errors = list(close.get('escape_errors') or []) + [c['error'] for c in close.get('checks') or []
                                                                       if c.get('error')]
                    raise ValueError('PURCHASE_TIMED_CLOSE_UNVERIFIED' + (':' + ';'.join(errors)[:300] if errors else ''))
                if args.buy and result.get('outcome') != 'bought':
                    raise ValueError('PURCHASE_TIMED_OUTCOME:' + str(result.get('outcome')))
        result['status'] = 'passed'
    except BaseException as error:
        result['error'] = result.get('error') or str(error) or type(error).__name__
        if not isinstance(error, Exception):
            result['interrupted'] = True
    finally:
        stop_banner()
        if result['status'] == 'passed':
            if args.cleanup_only:
                final = '清理完成：移除 %d 把过期' % len(cleaned)
            elif args.buy:
                final = '定时购买结束：%s' % OUTCOME_TEXT.get(result.get('outcome'), result.get('outcome'))
            elif early_ms is not None:
                final = '小窗按早测试：%s' % ('，'.join(result.get('toasts_after_press') or []) or '没读到顶部提示')
            else:
                late = (result.get('press_decision') or {}).get('late_ms') or 0.0
                unlock = result.get('unlock_after_dialog_zero_ms') or {}
                final = '演练完成：到点可按（偏差 %.1f ms）' % late
                if unlock:
                    final += '，解锁在归零后约 %d ms' % round(unlock['lower'])
            overlay.show(final, 'ok', ttl_ms=10000)
        elif final_banner:
            overlay.show('已停止：%s' % stop_text(result.get('error')), 'error', ttl_ms=10000)
        if args.buy:
            try:
                record_press()
            except Exception as error:
                result['ledger_error'] = str(error) or type(error).__name__
        result['overlay'] = overlay.metadata()
        result['cleanup'] = dict(result.get('cleanup') or {}, removed=cleaned)
        report = session.report if session is not None else {}
        if report.get('error') and str(report['error']) not in str(result.get('error') or ''):
            result['session_error'] = report['error']   # e.g. the game lost the front after the press
        result.update(watches=watches, dialog_reads=dialog_reads, dialog_watches=dialog_watches, clock=clock.summary(),
                      ide_restored=report.get('ide_restored'), session_passed=report.get('passed'),
                      config_sha256=snapshot['config_sha256'])
        result['standard_time'] = standard_time(result['clock'].get('display_zero'), (result.get('system_clock') or {}).get('offset_ms'))
        # The lease bound the game (review wf_b0b8309e-39d: a game closed or
        # elevated never entered, and is no user at the window).
        result['lease_entered'] = report.get('entry_ms') is not None
        if result['status'] == 'passed' and result['ide_restored'] is not True:
            result.update(status='blocked', error=result.get('error') or 'PURCHASE_TIMED_FOREGROUND_NOT_RETURNED')
        elif result['status'] == 'passed' and result['session_passed'] is False:
            # A read failed after the press (e.g. E_DXGI_ACQUIRE), the outcome still came from a direct read.
            result.update(status='blocked', error=result.get('error') or 'PURCHASE_TIMED_SESSION_FAILED:%s' % report.get('error'))
        path = output / 'result.json'
        path.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False, default=str) + '\n', encoding='utf-8')
    return result


def summary_line(result):
    return dict(status=result['status'], error=result.get('error'), buy=result.get('buy'), cleanup=result.get('cleanup'),
                entry=result.get('entry'), dialog_zero=result.get('dialog_zero'), press_decision=result.get('press_decision'),
                confirm=result.get('confirm'), confirm_clicks=result['confirm_clicks'], outcome=result.get('outcome'),
                unlock_after_dialog_zero_ms=result.get('unlock_after_dialog_zero_ms'), close=result.get('close'),
                early_press=result.get('early_press'), toasts_after_press=result.get('toasts_after_press'))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--config', type=Path, default=Path.home() / 'AppData/Local/RelinkStudio/RelinkStudio/config.json')
    p.add_argument('--buy', action='store_true', help='press the green button (a real purchase); default: rehearsal')
    p.add_argument('--enter-at', type=float, help='override run_settings.enterBeforeSeconds (1..5)')
    p.add_argument('--delay-ms', type=int, help='override run_settings.purchaseDelayMs (0..60000)')
    p.add_argument('--follow-limit', type=int, default=600)
    p.add_argument('--no-ntp', action='store_true')
    p.add_argument('--cleanup-only', action='store_true', help='only remove expired head listings, then stop')
    p.add_argument('--early-press-ms', type=int,
                   help='probe: press the dialog button once this long before its zero (%d..%d), to read the '
                        'too-early toast; never with --buy' % (EARLY_PRESS_MIN_LEAD_MS, EARLY_PRESS_MAX_LEAD_MS))
    p.add_argument('--no-overlay', action='store_true', help='no status banner at the top of the game')
    p.add_argument('--return-to', choices=sorted(RETURN_POLICIES), default='entry')
    args = p.parse_args()
    from purchase_delay import DelayTuner
    settings = settings_from(args.config, buy=args.buy, enter_at=args.enter_at, delay_ms=args.delay_ms,
                             follow_limit=args.follow_limit, no_ntp=args.no_ntp, cleanup_only=args.cleanup_only,
                             early_press_ms=args.early_press_ms)
    tuner = None
    root = project_root(__file__)
    output = args.output.resolve()
    if not output.is_relative_to(root / 'artifacts'):
        raise ValueError('PURCHASE_TIMED_OUTPUT_DIRECTORY')
    snapshot = snapshot_from_files(args.config, root / 'dist/RelinkStudio/catalog/skins.json')
    from run_collection_observed import MemoryReviewBackend
    overlay = OverlayChannel(root / 'dist/RelinkStudio/RelinkStudio.exe', enabled=not args.no_overlay, pipe=PURCHASE_PIPE,
                             extra_arguments=('--idle-exit-seconds', '900')).start()
    from run_exclusive import exclusive_run
    try:
        # One game run at a time: never next to the program's F2 cycle (review 2026-10-09).
        with exclusive_run():
            # The tuned delay (队列已满减延迟 / 公示期加延迟), read and counted under the lock.
            if args.delay_ms is None and settings.early_press_ms is None:
                tuner = DelayTuner.for_config(args.config)
                if tuner is not None:
                    settings.delay_ms = tuner.current()
            backend = MemoryReviewBackend(root, local_title=True, local_price=True, fast_capture=True,
                                          local_hotpath=False, ready_stream=False, fast_actions=False,
                                          parallel_local_price=True, return_policy=return_policy(args.return_to))
            result = run_attempt(settings, root=root, output=output, snapshot=snapshot, backend=backend,
                                 overlay=overlay)
            if tuner is not None and args.buy and result.get('confirm_clicks') == 1:
                try:
                    result['delay_change'] = tuner.record(result.get('outcome'),
                                                          when=time.strftime('%Y-%m-%dT%H:%M:%S'))
                except Exception as error:
                    result['delay_tuner_error'] = str(error) or type(error).__name__
    except RuntimeError as error:
        if str(error) != 'RUN_ALREADY_ACTIVE':
            raise
        overlay.show('另一个收藏/购买正在运行，这次没有开始', 'error', ttl_ms=10000)
        print(json.dumps(dict(status='blocked', error=str(error)), ensure_ascii=False))
        return 1
    finally:
        overlay.close(quit_after_ms=12000)
    line = summary_line(result)
    line['delay_change'] = result.get('delay_change')
    print(json.dumps(line, ensure_ascii=False, default=str))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
