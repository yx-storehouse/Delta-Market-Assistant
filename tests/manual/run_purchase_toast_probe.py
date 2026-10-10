"""Read the top result toast after pressing a buy button too early.

User 2026-10-09: "他会有一个小的弹出提示这个是后续需要用到的重要判断信息，是否进入抢购池，
点快了点慢了都需要看他…就选一个还在公式中的皮肤点下购买按钮弹出的这个订单还未开放购买来
尝试一次识别功能看看能不能识别到这个小弹出提示文字".

One foreground lease:
1. 我的关注; pick a listing still in its public notice with at least
   MIN_FOOTER_S left (the selected first card, or another visible card,
   clicked once to select it);
2. press its footer button ONCE (公示中: the order cannot be bought for
   another minute or more) and read the screen several times quickly: the
   toast, any dialog;
3. only if that opened the 外观购买 dialog and its own countdown still shows at
   least MIN_DIALOG_S: press the dialog's green button ONCE (same reason: not
   open yet) and read again;
4. Esc only while a dialog is seen open; the toast goes away by itself.
No other input. Nothing can be bought: every press is a minute or more before
the unlock (30 s for the dialog), re-checked on a fresh read right before it.
"""
import argparse
import base64
import json
from pathlib import Path
import time

from collection_paths import project_root
from purchase_clock import qpc_ms
from purchase_observation import read_screen
from run_purchase_dialog_probe import ESCAPE, VIEWPORT, dialog_closed, dialog_open
from run_purchase_preentry_probe import BUY_POINT
from run_purchase_probe import RETURN_POLICIES, allowed_step, navigate, return_policy
from run_purchase_timed_buy import DIALOG_BUY_POINT, purchase_dialog_problem
from run_purchase_countdown_probe import first_card_selected, listing_identity, only_listing_in_first_slot, same_listing
from purchase_watchlist_cleanup import REFRESH_POINT, clean_expired_head, cleanup_step_permitted, settle

# Seconds of public notice left, at least, before each press (footer, dialog).
MIN_FOOTER_S = 60
MIN_DIALOG_S = 30
# The read that permits a press is at most this old when the press goes out.
MAX_READ_AGE_MS = 3000
FAST_READS = 6
# Looking for a listing in its public notice: select cards one by one, and
# scroll the list down (3 notches) when the visible ones are all past it.
MAX_CARDS_TRIED = 30
# User 2026-10-09 21:50 (every visible listing was past its notice): "你直接刷新啊把时间到了的清了呀".
# First a refresh, then the expired head listings off (取消收藏 + 刷新), at most this many.
MAX_CLEANED = 30
MAX_SCROLLS = 8
SCROLL_POINT = [552, 900]
SCROLL_DELTA = -360


def toast_step_permitted(step, plan):
    """Navigation and captures (run_purchase_probe), one armed card click to
    select a listing, one armed press of the footer button; nothing else."""
    if allowed_step(step):
        return True
    if plan.get('cleanup') is not None and cleanup_step_permitted(step, plan['cleanup']):
        return True
    if step.get('kind') not in ('click', 'scroll') or step.get('expected_before') != 'watchlist_listings':
        return False
    if step.get('kind') == 'click' and step.get('point') in (SCROLL_POINT,):
        return False
    if plan.get('card_point') is not None and step.get('point') == plan['card_point']:
        plan['card_point'] = None
        plan['card_clicks'] = plan.get('card_clicks', 0) + 1
        return True
    if (step.get('kind') == 'scroll' and plan.get('scroll_armed') and step.get('point') == SCROLL_POINT
            and step.get('delta') == SCROLL_DELTA):
        plan['scroll_armed'] = False
        plan['scrolls'] = plan.get('scrolls', 0) + 1
        return True
    if plan.get('footer_armed') and step.get('point') == BUY_POINT:
        plan['footer_armed'] = False
        plan['footer_clicks'] = plan.get('footer_clicks', 0) + 1
        return True
    return False


def press_permitted(screen, now_ms, minimum_s, *, dialog):
    """None if a press is far enough ahead of the unlock; otherwise why not."""
    age = now_ms - screen['source_frame']['source_mono_ms']
    if not 0 <= age <= MAX_READ_AGE_MS:
        return 'READ_AGE:%.0f' % age
    if dialog:
        problem = purchase_dialog_problem(screen)
        if problem:
            return problem
        seconds = screen['dialog_countdown_seconds']
    else:
        if screen['page'] != 'watchlist_listings' or screen['overlay'] != 'none' or screen['signals']:
            return 'PAGE:%s/%s' % (screen['page'], screen['overlay'])
        if screen['button_state'] != 'publicity':
            return 'BUTTON_STATE:' + str(screen['button_state'])
        seconds = screen['countdown_seconds']
    if seconds is None or seconds < minimum_s:
        return 'TOO_CLOSE_TO_UNLOCK:' + str(seconds)
    return None


def card_points(packet):
    """(centre, bounds) of the fully visible, unselected cards, in reading order."""
    layout = packet.get('collection_layout') or {}
    if layout.get('complete') is not True:
        return []
    cards = [c for c in layout.get('cards', []) if not c.get('selected') and c['bounds'][3] >= 240]
    cards.sort(key=lambda c: (c['bounds'][1], c['bounds'][0]))
    return [([c['bounds'][0] + c['bounds'][2] // 2, c['bounds'][1] + c['bounds'][3] // 2], c['bounds']) for c in cards]


def selected_bounds(packet):
    card = packet.get('collection_selected_card') or {}
    return card.get('bounds') if card.get('selected') is True and card.get('same_frame') is True else None


def same_bounds(a, b, tolerance=4):
    return bool(a and b and len(a) == len(b) and all(abs(x - y) <= tolerance for x, y in zip(a, b)))


def summary(name, screen, received):
    return dict(name=name, received_qpc_ms=received, frame_qpc_ms=screen['source_frame']['source_mono_ms'],
                page=screen['page'], overlay=screen['overlay'], toast=screen['toast'],
                signals=[dict(kind=s['kind'], text=s['text'], region=s.get('region')) for s in screen['signals']],
                countdown_seconds=screen['countdown_seconds'], button_state=screen['button_state'],
                dialog_line=screen['dialog_line'], dialog_countdown_seconds=screen['dialog_countdown_seconds'])


def run(session, backend, output, result, plan, sleep=time.sleep):
    budget = dict(dialog_press=False)
    state = dict(dialog_seen=False)

    def read(name, *, layout, preview=False, direct=False):
        step = dict(kind='capture', collection_observation=True, ui_regions=True, purchase_observation=True,
                    preview=preview and not direct)
        if layout:
            step.update(collection_layout=True, collection_numeric_price=True)
        if direct:
            packet = json.loads(backend.capture(session._command(step), 15).stdout)
        else:
            session.perform(step)
            packet = session.previous
            if preview and session.last_preview:
                (output / (name + '.png')).write_bytes(base64.b64decode(session.last_preview))
        received = qpc_ms()
        screen = read_screen(packet)
        result['reads'].append(summary(name, screen, received))
        if dialog_open(screen):
            state['dialog_seen'] = True
        return screen, packet

    def fast_reads(prefix):
        return [read('%s_%d' % (prefix, index), layout=False, preview=index < 3)[0] for index in range(FAST_READS)]

    def close():
        """Esc only while the dialog is seen open; at most three."""
        out = dict(escapes=0, verified=None)
        for attempt in range(4):
            try:
                screen = read('close_check_%d' % attempt, layout=False,
                              direct=getattr(session, '_failed', False))[0]
            except Exception as error:
                out['read_error'] = str(error) or type(error).__name__
                return out
            if not dialog_open(screen):
                out['verified'] = dialog_closed(screen)
                return out
            if out['escapes'] >= 3:
                return out
            backend.key(ESCAPE)
            out['escapes'] += 1
            sleep(.6)
        return out

    try:
        if navigate(session) != 'watchlist_listings':
            raise ValueError('PURCHASE_TOAST_WATCHLIST_EMPTY')
        read('watchlist', layout=True, preview=True)
        # Refresh (the list reloads with its first card selected), then clear
        # the expired head listings.
        plan['cleanup']['refresh_armed'] = True
        session.perform(dict(kind='click', point=REFRESH_POINT, viewport=VIEWPORT, expected_before='watchlist_listings'))
        plan['cleanup']['refresh_armed'] = False
        settle(lambda: read('after_refresh', layout=True),
               lambda s, p: s['page'] in ('watchlist_listings', 'empty_watchlist'))
        cleaned = result['cleaned'] = []
        head, screen, packet = clean_expired_head(
            session, lambda: read('head', layout=True), listing_identity, same_listing, plan['cleanup'], cleaned,
            first_card=lambda s, p: first_card_selected(p) or only_listing_in_first_slot(s, p), max_expired=MAX_CLEANED)
        result['head_after_cleanup'] = head
        if head == 'empty':
            raise ValueError('PURCHASE_TOAST_WATCHLIST_EMPTY_AFTER_CLEANUP')
        if head != 'counting':
            raise ValueError('PURCHASE_TOAST_HEAD_STATE:' + head)
        chosen = screen['countdown_seconds'] is not None and screen['countdown_seconds'] >= MIN_FOOTER_S
        tried = result['cards_tried'] = []
        for scroll in range(MAX_SCROLLS + 1):
            if chosen or len(tried) >= MAX_CARDS_TRIED:
                break
            if scroll:
                before = (packet.get('collection_layout') or {}).get('scrollbar')
                plan['scroll_armed'] = True
                session.perform(dict(kind='scroll', point=SCROLL_POINT, delta=SCROLL_DELTA, viewport=VIEWPORT,
                                     expected_before='watchlist_listings'))
                screen, packet = read('scroll_%d' % scroll, layout=True)
                if (packet.get('collection_layout') or {}).get('scrollbar') == before:
                    break   # the end of the list
            for point, bounds in card_points(packet):
                if len(tried) >= MAX_CARDS_TRIED:
                    break
                plan['card_point'] = point
                session.perform(dict(kind='click', point=point, viewport=VIEWPORT, expected_before='watchlist_listings'))
                screen, packet = read('card_%d' % len(tried), layout=True)
                selected = same_bounds(selected_bounds(packet), bounds)
                tried.append(dict(scroll=scroll, point=point, countdown_seconds=screen['countdown_seconds'],
                                  button_state=screen['button_state'], selected=selected))
                if not selected:
                    raise ValueError('PURCHASE_TOAST_CARD_NOT_SELECTED')
                if screen['countdown_seconds'] is not None and screen['countdown_seconds'] >= MIN_FOOTER_S:
                    chosen = True
                    break
        if not chosen:
            raise ValueError('PURCHASE_TOAST_NO_LISTING_IN_PUBLIC_NOTICE')
        # A fresh read right before the press.
        screen, packet = read('before_footer_press', layout=True, preview=True)
        refused = press_permitted(screen, qpc_ms(), MIN_FOOTER_S, dialog=False)
        if refused:
            raise ValueError('PURCHASE_TOAST_NO_FOOTER_PRESS:' + refused)
        title = (packet.get('local_title_ocr') or {}).get('words') or [{}]
        result['footer_press'] = dict(countdown_seconds=screen['countdown_seconds'], title=title[0].get('text'))
        plan['footer_armed'] = True
        session.perform(dict(kind='click', point=BUY_POINT, viewport=VIEWPORT, expected_before='watchlist_listings'))
        result['footer_clicks'] = 1
        after_footer = fast_reads('after_footer')
        result['after_footer_toasts'] = sorted({t['text'] for s in after_footer for t in s['toast']})
        if state['dialog_seen']:
            screen, unused = read('before_dialog_press', layout=False, preview=True)
            refused = press_permitted(screen, qpc_ms(), MIN_DIALOG_S, dialog=True)
            result['dialog_press_refused'] = refused
            if refused is None and not budget['dialog_press']:
                budget['dialog_press'] = True  # never a second press
                backend.hover(DIALOG_BUY_POINT)
                backend.click(DIALOG_BUY_POINT)
                result['dialog_clicks'] = 1
                after_dialog = fast_reads('after_dialog')
                result['after_dialog_toasts'] = sorted({t['text'] for s in after_dialog for t in s['toast']})
        result['status'] = 'passed'
    except Exception as error:
        result['error'] = str(error) or type(error).__name__
    finally:
        result['dialog_seen'] = state['dialog_seen']
        if state['dialog_seen']:
            result['close'] = close()
            if result['close'].get('verified') is not True and result['status'] == 'passed':
                result.update(status='blocked', error='PURCHASE_TOAST_CLOSE_UNVERIFIED')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--return-to', choices=sorted(RETURN_POLICIES), default='entry')
    args = p.parse_args()
    root = project_root(__file__)
    output = args.output.resolve()
    if not output.is_relative_to(root / 'artifacts'):
        raise ValueError('PURCHASE_TOAST_OUTPUT_DIRECTORY')
    output.mkdir(parents=True, exist_ok=False)
    from collection_live_session import ForegroundSession
    from run_collection_observed import MemoryReviewBackend
    plan = dict(card_point=None, footer_armed=False, scroll_armed=False,
                cleanup=dict(star_armed=False, refresh_armed=False))

    class ToastSession(ForegroundSession):
        def perform(self, step, remaining=None):
            if not toast_step_permitted(step, plan):
                raise ValueError('PURCHASE_TOAST_STEP_NOT_ALLOWED')
            return super().perform(step, remaining)

    backend = MemoryReviewBackend(root, local_title=True, local_price=True, fast_capture=True, local_hotpath=False,
                                  ready_stream=False, fast_actions=False, parallel_local_price=True,
                                  return_policy=return_policy(args.return_to))
    session = ToastSession(output / 'session.json', backend=backend, timeout_seconds=420, max_steps=400,
                           numeric_price=True, fast_settle=False)
    result = dict(mode='purchase_toast_probe', footer_clicks=0, dialog_clicks=0, reads=[], status='blocked')
    from run_exclusive import exclusive_run
    try:
        with exclusive_run(), session:
            run(session, backend, output, result, plan)
    except Exception as error:
        result['error'] = result.get('error') or str(error) or type(error).__name__
        result['status'] = 'blocked'
    result.update(ide_restored=session.report.get('ide_restored'), session_passed=session.report.get('passed'),
                  plan=plan, toasts_seen=sorted({t['text'] for r in result['reads'] for t in r['toast']}))
    path = output / 'result.json'
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2, default=str) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'reads'}, ensure_ascii=False, default=str))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
