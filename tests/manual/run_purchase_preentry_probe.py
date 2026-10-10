"""Pre-entry rehearsal: open the purchase dialog before the public notice
ends, watch it through the unlock, close it with Esc. Never buys.

User 2026-10-09: "倒计时在10秒之内可以点开…确认购买的页面…公示中的按钮会变成金额，这个
时候需要提前按照设定的倒计时剩余时间提前点进去最后一个购买页，再在这个小购买页判断实时倒计时".
One lease, the first 我的关注 listing:

1. read-only SNTP check; navigate (allowlisted route); the first listing must
   be in its public notice with at most --follow-limit seconds left;
2. follow the countdown (对表) on every frame; in the last seconds the watch
   also ends as soon as the buy button's pixels change (公示中 -> price);
3. once the button shows the price: move the pointer onto it, and click it
   ONCE at --enter-at seconds before display zero (or right away if the
   price appeared later than that);
4. in the purchase dialog, watch its own countdown line on every frame (the
   same native watch on the dialog's line, ending on a change of its price
   button) until a few seconds after the unlock, with local preview images
   before and after;
5. close it with Esc (and, only if still open, a second Esc). The dialog's
   green price button IS the purchase confirmation: it is never clicked.

Importing this module is inert; input is sent only from main().
"""
import argparse
import base64
import json
from pathlib import Path
import re
import time

from collection_paths import project_root
from collection_run_config import snapshot_from_files
from purchase_clock import CountdownClock, qpc_ms, standard_time, system_clock_check, window_from_read
from purchase_observation import BUY_BUTTON_RECT, match_current_watchlist, normalize, read_screen
from run_purchase_countdown_probe import (WATCH_MAX_MS, WATCH_PAGES, countdown_step_permitted, first_card_selected,
                                          listing_identity, only_listing_in_first_slot, same_listing, watch_step,
                                          watch_summary)
from run_purchase_dialog_probe import ESCAPE, VIEWPORT, dialog_closed, dialog_open, dialog_texts
from run_purchase_probe import navigate, return_policy, sample_step, RETURN_POLICIES

BUY_POINT = [BUY_BUTTON_RECT[0] + BUY_BUTTON_RECT[2] // 2, BUY_BUTTON_RECT[1] + BUY_BUTTON_RECT[3] // 2]
# A watch request ends with a full read of its last frame (~0.5-1 s with the
# local reads), then the pointer moves onto the button (~0.25 s motion,
# 0.1 s settle, report writes): the last approach watch stops this long
# before the click.
READ_MARGIN_MS = 1300
HOVER_MARGIN_MS = 450
ENTRY_MARGIN_MS = READ_MARGIN_MS + HOVER_MARGIN_MS
# The session refuses an input on an observation older than 5 s, and a
# watch's age is its whole request: the watch behind the click stays short.
FINAL_WATCH_MAX_MS = 2500
# The approach watches end on a button change from this far before the click.
BUTTON_WATCH_FROM_MS = 15000
# The session refuses input on an observation older than 5000 ms; keep a margin.
AGE_LIMIT_MS = 4500
# Positive evidence for the green price button (user screenshot): share of
# its pixels in the green fill, measured natively on the final frame.
GREEN_FILL_MIN = 0.4
# Give up when the price button has not appeared this long after zero.
BUTTON_LATE_LIMIT_MS = 2500
# The green price button before the unlock (countdown still running) or after.
PRICE_STATES = ('price_ready', 'price_button')


def enter_target(clock, read_seconds, read_frame_ms, enter_at_ms):
    """The click moment on the capture clock: display zero minus the set
    remaining time, from the calibrated clock or else from the single read."""
    zero = clock.estimate()['display_zero']
    if zero is not None:
        return zero['estimate_mono_ms'] - enter_at_ms, zero
    earliest, _ = window_from_read(read_seconds, read_frame_ms, now_qpc_ms=0.0)
    return earliest + 500 - enter_at_ms, None


def approach_watch(lead_ms, button_ready):
    """(watch ms, stop on a button change) for the time left to the click, or
    None when the price button is ready and the click is due. The watch that
    precedes the click is at most FINAL_WATCH_MAX_MS long."""
    room = lead_ms - ENTRY_MARGIN_MS
    if room < 500:
        return None if button_ready else (1000, True)   # due, or waiting for the price
    stop = room <= BUTTON_WATCH_FROM_MS
    if room > FINAL_WATCH_MAX_MS:
        return max(500, min(WATCH_MAX_MS, int(room - FINAL_WATCH_MAX_MS))), stop
    return int(room), stop


def plan_next(lead_ms, button_ready, age_at_click_ms):
    """approach_watch, plus one quick refresh read when the click would rest
    on too old an observation (a display re-sync can move the target after
    the final watch, as live clock05->06). With the button already green the
    refresh watch ends on its first frame (stop on green)."""
    planned = approach_watch(lead_ms, button_ready)
    if planned is None and age_at_click_ms is not None and age_at_click_ms > AGE_LIMIT_MS:
        return 500, True
    return planned


def button_ready_now(after, packet, changed_since_publicity=False):
    """The latest read decides: a price button after the unlock, or one during
    the countdown with positive pixel evidence: the green fill on the same
    frame, or a native button change seen after the last 公示中 read (live
    preentry01: 2824 pixels changed at 4995 ms before zero while the fill rule
    measured 3 %)."""
    if after['button_state'] == 'price_button':
        return True
    if after['button_state'] != 'price_ready':
        return False
    green = packet.get('purchase_countdown_watch', {}).get('final_button_green_fraction')
    return bool(changed_since_publicity) or (type(green) in (int, float) and green >= GREEN_FILL_MIN)


def squash(text):
    return re.sub(r'[\s\-－—一·.]', '', normalize(text or ''))


def dialog_matches(screen, identity):
    """The dialog shows the verified listing: its title (OCR may drop the
    dash) and its exact wear: the wear's fraction digits must end one digit
    run of a dialog line (dialog03 read 0.256413 as "S〔J256413〕07")."""
    lines = [normalize(line['text']) for line in screen['regions']['dialog_text']]
    title = squash(identity.get('product_title'))
    wear = identity.get('selected_detail_precise') or ''
    digits = wear.split('.')[-1] if '.' in wear else ''
    if not title or len(digits) < 5:
        return False
    runs = [run for line in lines for run in re.findall(r'\d{5,}', line)]
    return title in squash(''.join(lines)) and any(run.endswith(digits) for run in runs)


def preentry_step_permitted(step, plan):
    """Reads, the allowlisted navigation and countdown watches; one hover and
    one click, both on the buy button, after the plan armed them."""
    on_button = (step.get('point') == BUY_POINT and step.get('expected_before') in WATCH_PAGES)
    if step.get('kind') == 'hover' and on_button and plan['armed'] and not plan['hovered']:
        plan['hovered'] = True
        return True
    if step.get('kind') == 'click' and on_button and plan['armed'] and not plan['clicked']:
        plan['clicked'] = True  # never a second press, whatever happens next
        return True
    if step.get('kind') in ('hover', 'click') and step.get('point') == BUY_POINT:
        raise ValueError('PURCHASE_PREENTRY_STEP_NOT_ALLOWED')
    if countdown_step_permitted(step):
        return True
    raise ValueError('PURCHASE_PREENTRY_STEP_NOT_ALLOWED')


def entry_allowed(after, identity, packet, changed_since_publicity=False):
    """The last read before the click: still the same listing on the
    watchlist, its button showing the price, no dialog open."""
    if after['page'] not in WATCH_PAGES or after['overlay'] != 'none':
        return 'PAGE'
    # Identical cards (same title, 成色 and price) differ only by their wear.
    if not identity.get('selected_detail_precise'):
        return 'IDENTITY_WEAR_UNREAD'
    if not same_listing(identity, listing_identity(packet)):
        return 'LISTING_CHANGED'
    if after['button_state'] not in PRICE_STATES:
        return 'BUTTON_STATE:' + str(after['button_state'])
    if not button_ready_now(after, packet, changed_since_publicity):
        return 'BUTTON_CHANGE_UNPROVEN'
    if after['signals']:
        return 'DIALOG_ALREADY_OPEN'
    return None


def wait_until(target_qpc_ms):
    """Sleep coarsely, then spin the last milliseconds on the QPC clock."""
    while True:
        left = target_qpc_ms - qpc_ms()
        if left <= 0:
            return
        time.sleep(min(left, 20) / 1000.0 if left > 3 else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--config', type=Path, default=Path.home() / 'AppData/Local/RelinkStudio/RelinkStudio/config.json')
    p.add_argument('--enter-at', type=float, default=5.0, help='seconds before display zero to open the dialog (1..5; the price button appears at 5 s)')
    p.add_argument('--follow-limit', type=int, default=180)
    p.add_argument('--dialog-tail-ms', type=int, default=3000, help='keep reading the dialog this long after zero')
    p.add_argument('--no-ntp', action='store_true')
    p.add_argument('--return-to', choices=sorted(RETURN_POLICIES), default='entry')
    args = p.parse_args()
    # User 2026-10-09: at most 5 s (live preentry01: the price appears 4995 ms before zero).
    if not 1.0 <= args.enter_at <= 5.0:
        raise ValueError('PURCHASE_PREENTRY_ENTER_AT')
    if not 10 <= args.follow_limit <= 600 or not 1000 <= args.dialog_tail_ms <= 8000:
        raise ValueError('PURCHASE_PREENTRY_LIMITS')
    enter_at_ms = args.enter_at * 1000.0
    root = project_root(__file__)
    output = args.output.resolve()
    if not output.is_relative_to(root / 'artifacts'):
        raise ValueError('PURCHASE_PREENTRY_OUTPUT_DIRECTORY')
    snapshot = snapshot_from_files(args.config, root / 'dist/RelinkStudio/catalog/skins.json')
    output.mkdir(parents=True, exist_ok=False)
    result = dict(mode='purchase_preentry_rehearsal', enter_at_s=args.enter_at, price_button_clicks=0,
                  confirm_clicks=0, purchase_actions=0, status='blocked', system_time_changed=False)
    result['system_clock'] = None if args.no_ntp else system_clock_check()
    from collection_live_session import ForegroundSession
    from run_collection_observed import MemoryReviewBackend
    plan = dict(armed=False, hovered=False, clicked=False)

    class PreentrySession(ForegroundSession):
        def perform(self, step, remaining=None):
            preentry_step_permitted(step, plan)
            return super().perform(step, remaining)

    backend = MemoryReviewBackend(root, local_title=True, local_price=True, fast_capture=True, local_hotpath=False,
                                  ready_stream=False, fast_actions=False, parallel_local_price=True,
                                  return_policy=return_policy(args.return_to))
    session = PreentrySession(output / 'session.json', backend=backend, timeout_seconds=args.follow_limit + 120,
                              max_steps=120, numeric_price=True, fast_settle=False)
    clock = CountdownClock()
    watches, dialog_reads, dialog_watches = [], [], []

    def direct_read():
        """One read through the backend: still possible after a failed
        session step (the session then refuses further steps)."""
        step = dict(kind='capture', collection_observation=True, ui_regions=True, purchase_observation=True)
        process = backend.capture(session._command(step), 15)
        return read_screen(json.loads(process.stdout))

    def close_dialog(expect):
        """After a dispatched click one Esc is always sent (a dialog may still
        appear late; on the watchlist Esc only goes back to 典藏首页), then the
        screen is verified; a second Esc follows unless it reads as closed."""
        out = dict(escapes=0, checks=[], verified=None)

        def escape():
            try:
                backend.key(ESCAPE)
                out['escapes'] += 1
            except Exception as error:
                out.setdefault('escape_errors', []).append(str(error) or type(error).__name__)
            time.sleep(.6)

        def check(name):
            try:
                screen = read_dialog(name) if not getattr(session, '_failed', False) else direct_read()
            except Exception as error:
                out['checks'].append(dict(name=name, error=str(error) or type(error).__name__))
                return None
            out['checks'].append(dict(name=name, open=dialog_open(screen), closed=dialog_closed(screen)))
            return screen

        if expect:
            escape()
        screen = check('after_escape' if expect else 'final_check')
        # After an Esc, anything but a positively closed page gets a second
        # Esc (Esc never confirms; verification wf_5f4f9818-db6).
        if (expect and (screen is None or not dialog_closed(screen))) or (not expect and screen is not None and dialog_open(screen)):
            escape()
            screen = check('after_second_escape')
        out['verified'] = None if screen is None else dialog_closed(screen)
        return out

    def read_dialog(name):
        session.perform(dict(kind='capture', collection_observation=True, ui_regions=True,
                             purchase_observation=True, preview=True))
        received = qpc_ms()
        packet = session.previous
        if session.last_preview:
            (output / (name + '.png')).write_bytes(base64.b64decode(session.last_preview))
        screen = read_screen(packet)
        dialog_reads.append(dict(name=name, received_qpc_ms=received, frame_source_mono_ms=screen['source_frame']['source_mono_ms'],
                                 page=screen['page'], overlay=screen['overlay'], button_state=screen['button_state'],
                                 signals=screen['signals'], texts=dialog_texts(screen)))
        return screen

    try:
        with session:
            page = navigate(session)
            if page != 'watchlist_listings':
                raise ValueError('PURCHASE_PREENTRY_WATCHLIST_EMPTY')
            sample_started = qpc_ms()
            session.perform(sample_step(page))
            first = session.previous
            screen = read_screen(first)
            observed = match_current_watchlist(first, snapshot)
            result['listing'] = dict(decision=observed['decision'], candidates=observed['candidates'],
                                     countdown_seconds=screen['countdown_seconds'], button_state=screen['button_state'])
            if first_card_selected(first):
                result['first_card_basis'] = 'selected_first_card'
            elif only_listing_in_first_slot(screen, first):
                result['first_card_basis'] = 'only_listing_in_first_slot'
            else:
                raise ValueError('PURCHASE_PREENTRY_FIRST_CARD_NOT_SELECTED')
            identity = listing_identity(first)
            result['identity'] = identity
            if screen['countdown_seconds'] is None:
                raise ValueError('PURCHASE_PREENTRY_NOT_IN_PUBLIC_NOTICE:' + str(screen['button_state']))
            if screen['countdown_seconds'] > args.follow_limit:
                raise ValueError('PURCHASE_PREENTRY_TOO_EARLY:%d' % screen['countdown_seconds'])
            read_seconds, read_frame = screen['countdown_seconds'], screen['source_frame']['source_mono_ms']
            after, packet, button_ready, changed_since_publicity = screen, first, False, False
            last_watch_start = sample_started
            result['refresh_reads'] = 0
            # Follow and approach: every watch re-calibrates display zero.
            for index in range(200):
                target, zero = enter_target(clock, read_seconds, read_frame, enter_at_ms)
                lead = target - qpc_ms()
                if zero is not None and qpc_ms() > zero['upper_mono_ms'] + BUTTON_LATE_LIMIT_MS and not button_ready:
                    raise ValueError('PURCHASE_PREENTRY_PRICE_BUTTON_NEVER_APPEARED')
                age_at_click = max(target, qpc_ms() + HOVER_MARGIN_MS) - last_watch_start
                planned = plan_next(lead, button_ready, age_at_click)
                if planned is None:
                    break
                if approach_watch(lead, button_ready) is None:
                    result['refresh_reads'] += 1
                ms, stop = planned
                step = watch_step(ms)
                # Approach watches end on a button change, or at once when the
                # button is already green (it may turn between two watches).
                step.update(stop_on_button=stop, stop_on_green=stop)
                last_watch_start = qpc_ms()
                session.perform(step)
                packet = session.previous
                after = read_screen(packet)
                record = dict(requested_ms=ms, stop_on_button=stop, lead_before_ms=lead, final_page=after['page'],
                              final_button_state=after['button_state'], final_countdown_seconds=after['countdown_seconds'],
                              watch=watch_summary(packet), identity=listing_identity(packet),
                              button_events=packet['purchase_countdown_watch'].get('button_events', []),
                              received_qpc_ms=qpc_ms())
                watches.append(record)
                if after['page'] not in WATCH_PAGES or after['overlay'] != 'none':
                    raise ValueError('PURCHASE_PREENTRY_WATCH_PAGE:' + str(after['page']))
                if not same_listing(identity, record['identity']):
                    raise ValueError('PURCHASE_PREENTRY_LISTING_CHANGED')
                record['display_zero'] = clock.add_watch(packet['purchase_countdown_watch'],
                                                         anchor_seconds=after['countdown_seconds'])['display_zero']
                record['button_green_fraction'] = packet['purchase_countdown_watch'].get('final_button_green_fraction')
                record['button_mean_bgr'] = packet['purchase_countdown_watch'].get('final_button_mean_bgr')
                if after['button_state'] == 'publicity':
                    changed_since_publicity = False
                elif record['button_events']:
                    changed_since_publicity = True
                button_ready = button_ready_now(after, packet, changed_since_publicity)
                if button_ready and 'price_button_seen' not in result:
                    result['price_button_seen'] = dict(watch=len(watches) - 1, received_qpc_ms=record['received_qpc_ms'],
                                                       button_events=record['button_events'],
                                                       green_fraction=record['button_green_fraction'])
            refused = entry_allowed(after, identity, packet, changed_since_publicity)
            if refused:
                raise ValueError('PURCHASE_PREENTRY_NOT_ENTERED:' + refused)
            target, zero = enter_target(clock, read_seconds, read_frame, enter_at_ms)
            plan['armed'] = True
            session.perform(dict(kind='hover', point=BUY_POINT, viewport=VIEWPORT, expected_before=after['page']))
            hovered, rest = qpc_ms(), backend._cursor()
            wait_until(target)
            # A user who moved the mouse during the wait is taking over: no click.
            if backend._cursor() != rest:
                raise ValueError('PURCHASE_PREENTRY_POINTER_MOVED')
            click_error = None
            step_started = qpc_ms()
            try:
                session.perform(dict(kind='click', point=BUY_POINT, viewport=VIEWPORT, expected_before=after['page']))
            except Exception as error:
                click_error = str(error) or type(error).__name__
            dispatch = backend.last_dispatch if isinstance(backend.last_dispatch, dict) else None
            dispatched = bool(dispatch and dispatch.get('kind') == 'click' and (dispatch.get('returned_events') or 0) >= 1)
            if not dispatched:
                raise ValueError(click_error or 'PURCHASE_PREENTRY_CLICK_NOT_DISPATCHED')
            result['price_button_clicks'] = 1
            sent = dispatch.get('started_qpc_ms', step_started)
            zero_now = clock.estimate()['display_zero']
            result['entry'] = dict(target_qpc_ms=target, hovered_qpc_ms=hovered, step_started_qpc_ms=step_started,
                                   dispatch_qpc_ms=sent, dispatch_returned_qpc_ms=dispatch.get('returned_qpc_ms'),
                                   late_ms=sent - target,
                                   display_zero_mono_ms=zero_now['estimate_mono_ms'] if zero_now else None,
                                   remaining_at_click_ms=(zero_now['estimate_mono_ms'] - sent) if zero_now else None)
            dialog_clock = CountdownClock()
            dialog_error, dialog_seen = click_error, False
            try:
                if click_error is None:
                    end = (zero_now['upper_mono_ms'] if zero_now else target + enter_at_ms) + 1000 + args.dialog_tail_ms
                    opened = None
                    for attempt in range(3):   # the dialog may take a moment to appear
                        opened = read_dialog('dialog_open_%d' % attempt)
                        if dialog_open(opened):
                            break
                    if not dialog_open(opened):
                        raise ValueError('PURCHASE_PREENTRY_DIALOG_NOT_OPEN')
                    dialog_seen = True
                    if not dialog_matches(opened, identity):
                        raise ValueError('PURCHASE_PREENTRY_DIALOG_LISTING_MISMATCH')
                    for index in range(8):
                        left = end - qpc_ms()
                        if left < 500:
                            break
                        step = watch_step(int(min(WATCH_MAX_MS, left)))
                        step.update(countdown_area='dialog', stop_on_button=True)
                        session.perform(step)
                        packet = session.previous
                        inside = read_screen(packet)
                        if not dialog_open(inside):
                            raise ValueError('PURCHASE_PREENTRY_DIALOG_GONE')
                        record = dict(watch=watch_summary(packet), received_qpc_ms=qpc_ms(),
                                      button_events=packet['purchase_countdown_watch'].get('button_events', []),
                                      button_green_fraction=packet['purchase_countdown_watch'].get('final_button_green_fraction'),
                                      dialog_line=inside['dialog_line'], dialog_countdown_seconds=inside['dialog_countdown_seconds'])
                        record['display_zero'] = dialog_clock.add_watch(packet['purchase_countdown_watch'],
                            anchor_seconds=inside['dialog_countdown_seconds'])['display_zero']
                        dialog_watches.append(record)
                        if dialog_clock.unlock()['observed']:
                            break
                    read_dialog('dialog_end')
            except Exception as error:
                dialog_error = dialog_error or str(error) or type(error).__name__
            finally:
                # Whatever happened after the click: close without confirming.
                # Esc never confirms; the green price button is never clicked.
                result['close'] = close_dialog(expect=dialog_seen or click_error is not None or dialog_error is not None)
            result['dialog_clock'] = dialog_clock.summary()
            if dialog_error:
                raise ValueError(dialog_error)
            if result['close']['verified'] is not True:
                raise ValueError('PURCHASE_PREENTRY_CLOSE_UNVERIFIED')
            dialog_summary = result['dialog_clock']
            if not dialog_watches or (dialog_summary.get('display_zero') is None and not dialog_summary['unlock']['observed']):
                raise ValueError('PURCHASE_PREENTRY_DIALOG_COUNTDOWN_UNREAD')
        result['status'] = 'passed'
    except Exception as error:
        result['error'] = str(error) or type(error).__name__
    result.update(watches=watches, dialog_reads=dialog_reads, dialog_watches=dialog_watches, clock=clock.summary(),
                  ide_restored=session.report.get('ide_restored'), session_passed=session.report.get('passed'),
                  config_sha256=snapshot['config_sha256'])
    result['standard_time'] = standard_time(result['clock'].get('display_zero'), (result['system_clock'] or {}).get('offset_ms'))
    if result['status'] == 'passed' and (result['ide_restored'] is not True or result['session_passed'] is False):
        result.update(status='blocked', error=result.get('error') or 'PURCHASE_PREENTRY_FOREGROUND_NOT_RETURNED')
    path = output / 'result.json'
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    zero = result['clock'].get('display_zero') or {}
    print(json.dumps(dict(status=result['status'], error=result.get('error'), entry=result.get('entry'),
                          close=result.get('close'),
                          price_button_seen=bool(result.get('price_button_seen')), dialog_reads=len(dialog_reads),
                          display_zero_local_time=zero.get('estimate_local_time'), uncertainty_ms=zero.get('uncertainty_ms'),
                          unlock=result['clock'].get('unlock'),
                          dialog_unlock=(result.get('dialog_clock') or {}).get('unlock'),
                          dialog_zero=((result.get('dialog_clock') or {}).get('display_zero') or {}).get('estimate_local_time'),
                          confirm_clicks=0), ensure_ascii=False))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
