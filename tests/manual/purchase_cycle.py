"""The F2 cycle: 我的关注 → collect when empty → purchase → repeat.

User 2026-10-09: "其实收藏就是给购买搭配和的啊…收藏程序启动前先打开了我的收藏么就是进去判断
收藏列表是否为空空了就去先收藏，收藏完了回到这个我的收藏页面开始购买程序"; the purchase is real
("F2 直接真实购买").

Each purchase attempt (run_purchase_timed_buy.run_attempt) opens 我的关注,
clears expired head listings and handles the first listing still in its
public notice. Its result steers the cycle:
- WATCHLIST_EMPTY: run one collection, then look again. Still empty right
  after a collection (nothing collectable, or only listings past their public
  notice, which the cleanup removes again): wait 1, 2, 5, then 15 minutes
  before collecting again (review 2026-10-09: no endless collect/cleanup loop);
- TOO_EARLY: the head unlocks later than the follow limit: wait (banner shows
  the countdown) and try again shortly before;
- a press happened (bought or not): next listing;
- the head changed while followed (withdrawn; before any entry click): look
  again at once, not a failure;
- 三角币不足 (the 充值 prompt instead of the purchase dialog, or as the
  result): stop; 充值 is never clicked;
- anything else: a short pause; three failures without a press in between
  stop the cycle.
F2 again (or closing the program) stops it between or inside steps; an open
purchase dialog is closed by the attempt itself, before any press.
"""
import json
import time
from pathlib import Path

from evidence_archive import MIN_FREE_BYTES, EvidenceKeeper, free_bytes
from purchase_banner import BannerTicker, banner_text, ledger_counts, ledger_path

# Wake up this long before the follow window opens.
WAKE_MARGIN_S = 60
# While waiting, look at the right panel this often (only when the game is in
# front, no input): another listing selected or moved up is followed at once
# (user 2026-10-10: "我改变选中皮肤好久了都没有变化").
LOOK_INTERVAL_S = 15
LOOK_JUMP_MS = 2500
MAX_LOOK_ROUNDS = 1000
# Still empty right after a collection: pause before the next one.
EMPTY_BACKOFF_S = (60, 120, 300, 900)
FAILURE_PAUSE_S = 3
MAX_FAILURES = 3
# Withdrawn heads in a row looked at again before they count as a failure.
MAX_RETRIES = 5
NO_BALANCE = ('insufficient_balance', 'recharge_prompt')
# Someone took the mouse or the window (live buy_cycle02 2026-10-10: the
# collection stopped on CURSOR_INTERFERENCE and the cycle went on to the
# purchase): the cycle stops at once, it never works against the user.
# Every input guard's code for a hand on the mouse or keyboard, or the game
# window switched away or covered (review wf_993b38d0-6b8; collection_status
# STOP_REASONS names the same), and the native capture's own codes for it.
USER_INTERVENTION = ('CURSOR_INTERFERENCE', 'CURSOR_START_CHANGED', 'CURSOR_ENDPOINT', 'CURSOR_MOVED_BEFORE_INPUT',
                     'USER_CHECK_FAILED', 'USER_INPUT_ACTIVE', 'INPUT_GUARD_CHANGED', 'TARGET_CHANGED_BEFORE_INPUT',
                     'TARGET_CHANGED_DURING_MOTION', 'OCCLUDED', 'FOREGROUND_LOST', 'FOREGROUND_NOT_RETURNED',
                     'FOREGROUND_NOT_OWNED', 'E_WINDOW_NOT_FOREGROUND', 'POINTER_MOVED')
# The head unlocks later than the follow window (user 2026-10-10: "这个要11分钟
# 换一把时间短的重新收藏一点"): collect once first, maybe a sooner one comes
# first (按稀有度升序). The collection ends COLLECT_BACK_BEFORE_S before that
# head's unlock (it resumes from its checkpoint next time), and it only starts
# with at least COLLECT_MIN_S of collecting left.
COLLECT_BACK_BEFORE_S = 120
COLLECT_MIN_S = 60
# Packed after the attempt itself once this many finished runs wait (each pack
# takes about a second; otherwise packing happens in waits and after collections).
PACK_BACKLOG = 20


def user_intervened(error):
    return any(marker in str(error or '') for marker in USER_INTERVENTION)


class DeadlineStop:
    """The cycle's stop for one collection, which also ends at deadline_ms (QPC)."""

    def __init__(self, stop, deadline_ms, qpc_ms):
        self.stop, self.deadline_ms, self.qpc_ms, self.expired = stop, deadline_ms, qpc_ms, False

    def __call__(self):
        if self.stop():
            return True
        if self.qpc_ms() >= self.deadline_ms:
            self.expired = True
        return self.expired

    @property
    def reason(self):
        return getattr(self.stop, 'reason', None) or ('collection_deadline' if self.expired else None)


def classify(result):
    """What the cycle does next for one attempt's result."""
    error = str(result.get('error') or '')
    if 'STOP_REQUESTED' in error or result.get('interrupted'):
        return 'stopped'
    # Also after a press: the outcome error hides a session that lost the game
    # (review wf_993b38d0-6b8: the next attempt would pull it back to the front).
    lost_lease = result.get('ide_restored') is False and result.get('lease_entered') is not False
    if user_intervened(error) or user_intervened(result.get('session_error')) or lost_lease:
        return 'user'
    if result.get('outcome') == 'insufficient_balance' or ('WRONG_DIALOG' in error and any(k in error for k in NO_BALANCE)):
        return 'no_balance'
    if 'WATCHLIST_EMPTY' in error:
        return 'collect'
    if 'TOO_EARLY' in error and (result.get('head') or {}).get('countdown_seconds') is not None:
        return 'wait'
    if 'HEAD_TOO_CLOSE' in error and not result.get('price_button_clicks'):
        return 'retry'      # met too close to its zero to calibrate: look again, the next one comes up
    if ('LISTING_CHANGED' in error or 'HEAD_SWITCHED:' in error) and not result.get('price_button_clicks'):
        # The head went away while followed (live early02: withdrawn, the
        # next one moved up): a market event, not a failure; look again now.
        return 'retry'
    if result.get('confirm_clicks') or result.get('status') == 'passed':
        return 'next'
    return 'failed'


def wait_seconds(result, follow_limit, now_qpc_ms):
    """Seconds to wait until shortly before the follow window of the head."""
    head = result['head']
    zero = head['frame_qpc_ms'] + (head['countdown_seconds'] - .5) * 1000.0
    return max(0.0, (zero - now_qpc_ms) / 1000.0 - follow_limit + WAKE_MARGIN_S), zero


def empty_backoff(rounds):
    """Pause before collecting again after `rounds` collections in a row that
    left 我的关注 empty."""
    return EMPTY_BACKOFF_S[min(rounds, len(EMPTY_BACKOFF_S)) - 1] if rounds > 0 else 0


def idle(seconds, stop, sleep=time.sleep):
    """Wait, waking every half second for a stop. True if stopped."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if stop():
            return True
        sleep(min(.5, max(0.0, end - time.monotonic())))
    return bool(stop())


def wait_with_looks(held, settings, peek, pause, qpc_ms, summary, say):
    """Wait until held['deadline'] (QPC ms), looking at the right panel
    every LOOK_INTERVAL_S. A panel whose countdown moved by more than
    LOOK_JUMP_MS (another listing) moves the wait and the banner; a panel
    without a countdown, or one already inside the follow window, ends the
    wait at once. True if stopped."""
    looks = summary.setdefault('looks', dict(count=0, changes=0, errors=0))
    for round_ in range(MAX_LOOK_ROUNDS):   # bounded even if the clock stood still
        left = (held['deadline'] - qpc_ms()) / 1000.0
        if left <= 0:
            return False
        if pause(min(LOOK_INTERVAL_S, left)):
            return True
        if held['deadline'] - qpc_ms() <= 0:
            return False
        try:
            seen = peek()
        except Exception as error:
            looks['errors'] += 1
            looks['last_error'] = str(error) or type(error).__name__
            continue
        if seen is None:
            continue
        looks['count'] += 1
        page, overlay = seen.get('page'), seen.get('overlay')
        if page == 'empty_watchlist' and overlay == 'none':
            looks['ended_by'] = 'empty_watchlist'
            return False
        if page != 'watchlist_listings' or overlay != 'none':
            # The user is elsewhere in the game (market, lobby, a match): no
            # information about 我的关注, no input, the deadline stays.
            looks['elsewhere'] = looks.get('elsewhere', 0) + 1
            held.pop('later', None)
            continue
        if seen.get('countdown_seconds') is None:
            if seen.get('button_state') == 'price_button' and not seen.get('countdown_unread'):
                looks['ended_by'] = seen.get('state')      # the panel's listing is past its notice
                return False
            looks['unread'] = looks.get('unread', 0) + 1
            continue
        zero = seen['frame_qpc_ms'] + (seen['countdown_seconds'] - .5) * 1000.0
        if abs(zero - held['zero']) <= LOOK_JUMP_MS:
            held.pop('later', None)
            continue
        if zero > held['zero']:
            # Later only on two looks that agree (a single misread must not make the wait miss the head).
            earlier = held.get('later')
            held['later'] = zero
            if earlier is None or abs(earlier - zero) > LOOK_JUMP_MS:
                continue
        held.pop('later', None)
        looks['changes'] += 1
        held['zero'] = zero
        held['deadline'] = zero - (settings.follow_limit - WAKE_MARGIN_S) * 1000.0
        say('右边换成了 %s：剩 %d分%d秒' % (seen.get('title') or '另一把', *divmod(int(seen['countdown_seconds']), 60)))
    return False


def attempt_text(result):
    """The banner after an attempt that did not press, while the cycle goes on."""
    from run_purchase_timed_buy import stop_text
    return '这一把没买：%s，继续下一把' % stop_text(result.get('error'))


def default_peek(pool, stop, folder):
    """A read-only look at the right panel when the game is in front, else None."""
    import shutil
    from run_purchase_timed_buy import game_window, peek_panel
    window = game_window()
    if window is None:
        return None
    folder = Path(folder)
    shutil.rmtree(folder, ignore_errors=True)   # the previous look's own record (only the latest is kept)
    folder.mkdir(parents=True, exist_ok=True)
    backend = pool.take()
    if stop():
        getattr(backend, 'discard', lambda: None)()
        return None
    return peek_panel(backend=backend, output=folder, window=window)


DELAY_REASON = dict(decrease='抢购队列已满，减延迟', increase='按早了（订单尚未开放），加延迟')


def run_cycle(args, stop, pool, overlay, *, hotkey, monitor_hwnd, collect, runs_dir, new_run_directory,
              log=None, emit=None, qpc_ms=None, sleep=time.sleep, attempt_fn=None, settings=None, snapshot=None, root=None,
              keeper=None, disk_free=None, peek=None, tuner=None, game_in_front=None):
    """Loop until stopped. `collect(...)` is the existing collection run
    (returns (exit code, result)); `attempt_fn` defaults to run_attempt.
    game_in_front: () -> bool, the game is the foreground window."""
    from purchase_clock import qpc_ms as default_qpc
    qpc_ms = qpc_ms or default_qpc
    if attempt_fn is None or settings is None or snapshot is None or root is None:
        from collection_paths import project_root
        from collection_run_config import snapshot_from_files
        from run_purchase_timed_buy import run_attempt, settings_from
        attempt_fn = attempt_fn or run_attempt
        settings = settings or settings_from(args.config, buy=not getattr(args, 'purchase_rehearsal', False), no_ntp=True)
        snapshot = snapshot or snapshot_from_files(args.config, args.catalog)
        root = root or project_root(__file__)
    keeper = keeper or EvidenceKeeper()
    disk_free = disk_free or (lambda: free_bytes(runs_dir))
    cycle_dir = Path(new_run_directory(runs_dir))
    peek = peek or (lambda: default_peek(pool, stop, cycle_dir / 'look'))
    summary = dict(schema='purchase-cycle-v1', hotkey=hotkey, buy=settings.buy, attempts=[], collections=[],
                   status='running', run_directory=str(cycle_dir))
    say = (lambda text, tone='info', ttl_ms=0: (overlay.show(text, tone, ttl_ms=ttl_ms),
                                                emit and emit(dict(type='status', text=text, tone=tone, log=True))))
    if tuner is None and Path(settings.config).exists():
        # The rehearsal uses the tuned delay too (read only), so it shows the real press moment.
        from purchase_delay import DelayTuner
        tuner, tuner_error = DelayTuner.open(settings.config)
        if tuner_error:
            summary['delay_tuner'] = 'unavailable: ' + tuner_error
            say('自动调延迟暂时不可用（读不到运行设置），先用 %g ms' % settings.delay_ms, 'warn', ttl_ms=8000)

    def save():
        summary['evidence'] = dict(saved_bytes=keeper.saved, errors=keeper.errors[-5:])
        (cycle_dir / 'cycle.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2, default=str) + '\n',
                                              encoding='utf-8')

    if game_in_front is None:
        from run_purchase_timed_buy import game_window

        def game_in_front():
            return game_window() is not None
    # F2 pressed inside the game: from then on the game leaving the front
    # between leases (a wait, a pause) is the user taking over; the next
    # lease would pull it back (review wf_b0b8309e-39d). Started outside the
    # game, every lease brings it to the front as before.
    try:
        started_in_game = bool(game_in_front())
    except Exception:
        started_in_game = False
    summary['started_in_game'] = started_in_game
    left = dict(game=False)
    if started_in_game:
        pool.require_game_in_front = True    # every lease refuses rather than activates (review wf_3215e495-f2d)

    def game_left():
        if not started_in_game or left['game']:
            return left['game']
        try:
            left['game'] = not game_in_front()
        except Exception:
            left['game'] = False
        return left['game']

    def pause(seconds):
        keeper.pack_older()
        return idle(seconds, lambda: stop() or game_left(), sleep)

    def run_collection(collect_stop):
        try:
            code, collected = collect(args, collect_stop, pool, overlay, hotkey=hotkey, monitor_hwnd=monitor_hwnd)
        except Exception as error:
            code, collected = 1, dict(status='blocked', error=str(error) or type(error).__name__)
        summary['collections'].append(dict(exit_code=code, status=collected.get('status'),
                                           error=collected.get('error'),
                                           new_favorites=collected.get('new_favorites'),
                                           run_directory=collected.get('run_directory'),
                                           before_far_head=isinstance(collect_stop, DeadlineStop),
                                           stop_reason=collected.get('stop_reason') or getattr(collect_stop, 'reason', None),
                                           deadline_expired=getattr(collect_stop, 'expired', None)))
        keeper.finished_run('collection', collected.get('run_directory'))
        keeper.pack_older()
        save()
        return code, collected

    failures = attempt_index = empty_rounds = retries = 0
    collected_last = wait_collected = full_pass_done = False
    try:
        while not stop():
            if game_left():
                break
            if disk_free() < MIN_FREE_BYTES:
                keeper.pack_older(keep=0)
                if disk_free() < MIN_FREE_BYTES:
                    summary['error'] = 'CYCLE_DISK_FULL'
                    break
            attempt_index += 1
            output = cycle_dir / ('purchase_%03d' % attempt_index)
            if tuner:
                try:
                    tuner.refresh()
                except Exception as error:
                    summary['delay_tuner_error'] = str(error) or type(error).__name__
                settings.delay_ms = tuner.current()   # 队列已满减延迟 / 公示期加延迟 (purchase_delay)
            try:
                backend = pool.take(on_wait=lambda: say('正在准备截图识别服务…'))
                if stop():
                    getattr(backend, 'discard', lambda: None)()
                    break
                result = attempt_fn(settings, root=root, output=output, snapshot=snapshot, backend=backend,
                                    overlay=overlay, stop_requested=stop, final_banner=False)
            except Exception as error:
                result = dict(status='blocked', error=str(error) or type(error).__name__)
            keeper.finished_run('purchase', output if output.is_dir() else None)
            action = classify(result)
            retries = retries + 1 if action == 'retry' else 0
            if retries > MAX_RETRIES:
                action, retries = 'failed', 0
            summary['attempts'].append(dict(index=attempt_index, action=action, status=result.get('status'),
                                            error=result.get('error'), outcome=result.get('outcome'),
                                            confirm_clicks=result.get('confirm_clicks'), output=str(output),
                                            delay_ms=settings.delay_ms))
            if tuner and settings.buy and result.get('confirm_clicks') == 1:
                try:
                    change = tuner.record(result.get('outcome'), when=time.strftime('%Y-%m-%dT%H:%M:%S'))
                except Exception as error:
                    change = None
                    summary['delay_tuner_error'] = str(error) or type(error).__name__
                if change:
                    summary.setdefault('delay_changes', []).append(change)
                    say('购买延迟 %g → %g ms（%s）' % (change['before_ms'], change['after_ms'], DELAY_REASON[change['rule']]))
            save()
            if log:
                log.info('purchase attempt', index=attempt_index, action=action, error=result.get('error'),
                         outcome=result.get('outcome'))
            if action == 'stopped' or stop():
                break
            if action == 'no_balance':
                summary['error'] = 'CYCLE_INSUFFICIENT_BALANCE'
                break
            if action == 'user':
                summary['error'] = 'CYCLE_USER_INTERVENED:' + str(result.get('error'))
                break
            if action == 'collect':
                empty_rounds = empty_rounds + 1 if collected_last else 0
                collected_last = False
                if empty_rounds:
                    seconds = empty_backoff(empty_rounds)
                    say('收藏后我的关注还是空的（没有可买的公示中皮肤）：%d 分钟后再收藏' % max(1, seconds // 60), 'warn')
                    if pause(seconds):
                        break
                say('我的关注是空的：开始收藏')
                if game_left():
                    break
                code, collected = run_collection(stop)
                full_pass_done = code == 0
                if code == 3 or stop():
                    break
                if code != 0 and user_intervened(collected.get('error')):
                    summary['error'] = 'CYCLE_USER_INTERVENED:' + str(collected.get('error'))
                    break
                if code != 0:
                    failures += 1
                    if failures >= MAX_FAILURES:
                        summary['error'] = 'CYCLE_COLLECTION_FAILED:' + str(collected.get('error'))
                        break
                    if pause(FAILURE_PAUSE_S):
                        break
                    continue
                # A far head right after a full collection is waited for, not collected for again.
                collected_last = wait_collected = True
                continue
            collected_last, empty_rounds = False, 0
            if keeper.pending() >= PACK_BACKLOG:
                keeper.pack_older()
            if action == 'wait':
                seconds, zero = wait_seconds(result, settings.follow_limit, qpc_ms())
                deadline = zero - COLLECT_BACK_BEFORE_S * 1000.0
                if not (wait_collected or full_pass_done) and deadline - qpc_ms() >= COLLECT_MIN_S * 1000.0:
                    wait_collected = True
                    say('第一位还要 %d 分钟才解锁：先收藏一轮，找时间更短的' % max(1, round((zero - qpc_ms()) / 60000.0)))
                    if game_left():
                        break
                    limited = DeadlineStop(stop, deadline, qpc_ms)
                    code, collected = run_collection(limited)
                    full_pass_done = code == 0
                    if stop():
                        break
                    if code != 0 and user_intervened(collected.get('error')):
                        summary['error'] = 'CYCLE_USER_INTERVENED:' + str(collected.get('error'))
                        break
                    if code == 3 and limited.expired:
                        say('收藏到点了（第一位快解锁），回去跟这一把；下次从没收完的地方继续')
                    elif code != 0:
                        failures += 1
                        if failures >= MAX_FAILURES:
                            summary['error'] = 'CYCLE_COLLECTION_FAILED:' + str(collected.get('error'))
                            break
                    continue
                counts = ledger_counts(ledger_path(settings.config))
                held = dict(zero=zero, deadline=qpc_ms() + seconds * 1000.0)
                ticker = BannerTicker(overlay, lambda: banner_text(counts, settings.delay_ms, held['zero'] - qpc_ms(),
                                                                    not settings.buy)).start()
                try:
                    stopped = wait_with_looks(held, settings, peek, pause, qpc_ms, summary, say)
                finally:
                    ticker.stop()
                save()
                if stopped:
                    break
                continue
            if action == 'next':
                failures, wait_collected, full_pass_done = 0, False, False
                continue
            if action == 'retry':
                continue
            wait_collected = False
            failures += 1
            if failures >= MAX_FAILURES:
                summary['error'] = 'CYCLE_REPEATED_FAILURES:' + str(result.get('error'))
                break
            say(attempt_text(result), 'warn', ttl_ms=8000)
            if pause(FAILURE_PAUSE_S):
                break
        if left['game'] and not stop() and not summary.get('error'):
            summary['error'] = 'CYCLE_USER_INTERVENED:GAME_NOT_IN_FRONT'
        summary['status'] = 'stopped' if stop() else ('blocked' if summary.get('error') else 'ended')
    except Exception as error:
        summary.update(status='blocked', error=str(error) or type(error).__name__)
        raise
    finally:
        pool.require_game_in_front = False
        summary['stop_reason'] = getattr(stop, 'reason', None)
        try:
            keeper.pack_older()
        finally:
            save()
            try:
                say(*final_text(summary))
            except Exception as error:
                summary['final_banner_error'] = str(error) or type(error).__name__
    return summary


CYCLE_ERROR_TEXT = dict(CYCLE_INSUFFICIENT_BALANCE='三角币不足，已停止购买（不会点充值）',
                        CYCLE_DISK_FULL='磁盘空间不足 3 GB，已停止',
                        CYCLE_REPEATED_FAILURES='连续 3 把没能完成，已停止',
                        CYCLE_COLLECTION_FAILED='收藏连续失败，已停止',
                        CYCLE_USER_INTERVENED='鼠标、键盘或游戏窗口被操作，已停止（再按 F2 重新开始）')


def final_text(summary):
    """(text, tone, ttl) for the banner when the cycle ends."""
    if summary.get('status') == 'stopped':
        return '收藏+购买已停止', 'info', 8000
    error = str(summary.get('error') or '')
    for key, text in CYCLE_ERROR_TEXT.items():
        if error.startswith(key):
            reason = error[len(key) + 1:]
            if reason and key in ('CYCLE_REPEATED_FAILURES', 'CYCLE_COLLECTION_FAILED', 'CYCLE_USER_INTERVENED'):
                from run_purchase_timed_buy import stop_text
                text += '：' + stop_text(reason)
            return text, 'error', 15000
    return ('收藏+购买已停止：' + error[:60]) if error else '收藏+购买已结束', 'error' if error else 'info', 15000
