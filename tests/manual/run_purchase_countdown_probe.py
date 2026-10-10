"""对表: read the public-notice countdown of the first 我的关注 listing to the
frame, read-only.

User 2026-10-09: "主要就是点第一个位置的皮肤识别它的公示期剩余时间，必须分秒不差";
"应该是每一秒都在读取时间校准…也有可能会用到系统时间". One lease:

1. a read-only SNTP check of the system clock (never sets it);
2. navigate to 我的关注 (the allowlisted route of run_purchase_probe); the
   first card must be the selected one and its footer must show a countdown;
3. watch the countdown line on every new frame (native, up to 10 s per watch)
   and re-estimate the moment the display reaches zero on every tick;
4. with --follow and at most --follow-limit seconds left: keep watching
   through the unlock, so the unlock itself is timed against that estimate.

No click, key or scroll beyond the navigation. Importing this module is inert.
"""
import argparse
import json
from pathlib import Path
import re

from collection_paths import project_root
from collection_run_config import snapshot_from_files
from purchase_clock import CountdownClock, qpc_ms, standard_time, system_clock_check, window_from_read
from purchase_observation import match_current_watchlist, normalize, read_screen
from run_purchase_probe import allowed_step, navigate, return_policy, sample_step, RETURN_POLICIES

WATCH_MAX_MS = 10000
# The final watch starts this long before the earliest possible zero and ends
# this long after the latest possible unlock; full OCR and the local reads
# between two watches take up to about a second.
FINAL_LEAD_MS = 4500
FINAL_TAIL_MS = 3000
# Watches without a single readable second before giving up.
UNREAD_WATCH_LIMIT = 3
# Same-frame reads of the selected listing: local title, card condition and
# price, precise wear (live 2026-10-09 live04: stable across reads, while the
# broad first detail line was not).
IDENTITY_REGIONS = ('product_title', 'card_fields', 'selected_detail_precise')


# The page of a watch's final frame is checked here, not natively: live
# clock06 lost its last 8 s when one frame's OCR missed the small
# "我的关注 1/150" corner and the unchanged page read as skin_listings. No input
# is sent during a watch, so the page cannot have changed; the listing
# identity is still required. No card layout either: on a skin_listings
# reading the session would treat the frame as a collection page.
WATCH_PAGES = ('watchlist_listings', 'skin_listings')


def watch_step(ms):
    return dict(kind='capture', collection_observation=True, ui_regions=True, purchase_observation=True,
                countdown_watch_ms=int(ms))


def first_card_selected(packet):
    """The selected card is the top-left one (live: bounds [119,304,866,264])."""
    selected = packet.get('collection_selected_card', {})
    bounds = selected.get('bounds')
    if selected.get('selected') is not True or not isinstance(bounds, list) or len(bounds) != 4:
        return False
    cards = [c for c in packet.get('collection_layout', {}).get('cards', []) if isinstance(c.get('bounds'), list)]
    if not cards:
        return False
    top = min(c['bounds'][1] for c in cards)
    first = min((c for c in cards if abs(c['bounds'][1] - top) <= 20), key=lambda c: c['bounds'][0])
    return first['bounds'] == bounds


# The first card slot of the watchlist (live: card bounds [119,304,866,264]).
FIRST_SLOT = (110, 295, 885, 285)


def only_listing_in_first_slot(screen, packet):
    """A watchlist with a single listing has no scrollbar, so the card layout
    is not measured (live 2026-10-09 clock02: E_COLLECTION_SCROLLBAR_TRACK,
    1/150). Then every listing word must lie in the first slot: the only
    listing is the first one, and the detail panel can only show it."""
    if packet.get('collection_layout', {}).get('error') != 'E_COLLECTION_SCROLLBAR_TRACK':
        return False
    x, y, w, h = FIRST_SLOT
    words = [word for line in screen['regions']['listing_text'] for word in line['words']]
    return bool(words) and all(x <= v['x'] and y <= v['y'] and v['x'] + v['width'] <= x + w
                               and v['y'] + v['height'] <= y + h for v in words)


def listing_identity(packet):
    texts = {region.get('kind'): normalize(''.join(w.get('text', '') for w in region.get('words', [])))
             for region in packet.get('collection_observation', {}).get('regions', [])
             if region.get('kind') in IDENTITY_REGIONS}
    # The precise wear by its number only: a neighbouring ")" comes and goes.
    wear = re.search(r'\d+\.\d{4,}', texts.get('selected_detail_precise', ''))
    if wear is None:
        # The fixed wear crop can miss when the detail panel's layout shifts
        # (live preentry01: it read "典藏外观" while the full read of the same
        # frame had "0．827567"): take the wear from that full read.
        detail = [w.get('text', '') for region in packet.get('purchase_observation', {}).get('regions', [])
                  if region.get('kind') == 'selected_detail' for w in region.get('words', [])]
        wear = re.search(r'\d+\.\d{5,}', normalize(''.join(detail)))
    texts['selected_detail_precise'] = wear.group(0) if wear else ''
    return dict(card=packet.get('collection_selected_card', {}).get('bounds'),
                **{kind: texts.get(kind, '') for kind in IDENTITY_REGIONS})


def same_listing(a, b):
    """Same selected card (when both reads measured it) and at least two of
    title / card fields / wear read in both and equal; any field read in both
    must be equal."""
    if a['card'] is not None and b['card'] is not None and a['card'] != b['card']:
        return False
    compared = [kind for kind in IDENTITY_REGIONS if a[kind] and b[kind]]
    return len(compared) >= 2 and all(a[kind] == b[kind] for kind in compared)


def next_watch_ms(window):
    """Watch length from (earliest ms to display zero, latest ms to unlock):
    keep calibrating while far, stop short of the end so that the final
    watch spans zero and the unlock with a margin."""
    if window is None:
        return WATCH_MAX_MS
    earliest, latest = window
    if earliest > WATCH_MAX_MS + FINAL_LEAD_MS + 2000:
        return WATCH_MAX_MS
    if earliest > FINAL_LEAD_MS + 1500:
        return max(500, min(WATCH_MAX_MS, int(earliest - FINAL_LEAD_MS)))
    return max(500, min(WATCH_MAX_MS, int(latest + FINAL_TAIL_MS)))


def countdown_step_permitted(step):
    if step.get('kind') == 'capture' and 'countdown_watch_ms' in step:
        return (type(step['countdown_watch_ms']) is int and 500 <= step['countdown_watch_ms'] <= WATCH_MAX_MS
                and 'expected_page' not in step and step.get('purchase_observation') is True
                and step.get('countdown_area', 'footer') in ('footer', 'dialog')
                and type(step.get('stop_on_button', False)) is bool and type(step.get('stop_on_green', False)) is bool
                and not (step.get('stop_on_green') and step.get('countdown_area') == 'dialog')
                and not any(step.get(k) for k in ('collection_layout', 'expect_collection_added', 'receipt_if_pending',
                                                  'card_id', 'visible_index')))
    return allowed_step(step)


def watch_summary(packet):
    watch = packet.get('purchase_countdown_watch', {})
    return dict(frames_examined=watch.get('frames_examined'), line_reads=watch.get('line_reads'),
                segments=watch.get('segments'), covered_ms=watch.get('covered_ms'), ended_by=watch.get('ended_by'),
                # Ended before its requested time: fewer ticks than planned.
                short=watch.get('ended_by') != 'time',
                max_frame_gap_ms=watch.get('max_frame_gap_ms'),
                events=[dict(kind=e.get('kind'), text=e.get('text'), source_mono_ms=e.get('source_mono_ms'),
                             previous_source_mono_ms=e.get('previous_source_mono_ms'), changed_px=e.get('changed_px'))
                        for e in watch.get('events', [])])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--config', type=Path, default=Path.home() / 'AppData/Local/RelinkStudio/RelinkStudio/config.json')
    p.add_argument('--watches', type=int, choices=range(1, 4), default=1,
                   help='watches when not following (each up to 10 s)')
    p.add_argument('--follow', action='store_true', help='keep watching through the unlock')
    p.add_argument('--follow-limit', type=int, default=180, help='follow only with at most this many seconds left')
    p.add_argument('--no-ntp', action='store_true')
    p.add_argument('--return-to', choices=sorted(RETURN_POLICIES), default='entry')
    args = p.parse_args()
    if not 10 <= args.follow_limit <= 600:
        raise ValueError('PURCHASE_COUNTDOWN_FOLLOW_LIMIT')
    root = project_root(__file__)
    output = args.output.resolve()
    if not output.is_relative_to(root / 'artifacts'):
        raise ValueError('PURCHASE_COUNTDOWN_OUTPUT_DIRECTORY')
    snapshot = snapshot_from_files(args.config, root / 'dist/RelinkStudio/catalog/skins.json')
    output.mkdir(parents=True, exist_ok=False)
    result = dict(mode='purchase_countdown_sync_readonly', purchase_actions=0, status='blocked', system_time_changed=False)
    # Before the lease: network replies must not hold the game foreground.
    result['system_clock'] = None if args.no_ntp else system_clock_check()
    from collection_live_session import ForegroundSession
    from run_collection_observed import MemoryReviewBackend

    class CountdownSession(ForegroundSession):
        def perform(self, step, remaining=None):
            if not countdown_step_permitted(step):
                raise ValueError('PURCHASE_COUNTDOWN_STEP_NOT_ALLOWED')
            return super().perform(step, remaining)

    backend = MemoryReviewBackend(root, local_title=True, local_price=True, fast_capture=True, local_hotpath=False,
                                  ready_stream=False, fast_actions=False, parallel_local_price=True,
                                  return_policy=return_policy(args.return_to))
    session = CountdownSession(output / 'session.json', backend=backend,
                               timeout_seconds=(args.follow_limit + 90) if args.follow else 90,
                               max_steps=80, numeric_price=True, fast_settle=False)
    clock = CountdownClock()
    watches = []
    try:
        with session:
            page = navigate(session)
            if page != 'watchlist_listings':
                raise ValueError('PURCHASE_COUNTDOWN_WATCHLIST_EMPTY')
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
                raise ValueError('PURCHASE_COUNTDOWN_FIRST_CARD_NOT_SELECTED')
            identity = listing_identity(first)
            result['identity'] = identity
            if screen['countdown_seconds'] is None:
                raise ValueError('PURCHASE_COUNTDOWN_NOT_IN_PUBLIC_NOTICE:' + str(screen['button_state']))
            read_frame = screen['source_frame']['source_mono_ms']
            following = args.follow and screen['countdown_seconds'] <= args.follow_limit
            result['following'] = following
            for index in range(200):
                if not following and index >= args.watches:
                    break
                window = clock.zero_window() or window_from_read(screen['countdown_seconds'], read_frame)
                ms = next_watch_ms(window) if following else WATCH_MAX_MS
                session.perform(watch_step(ms))
                packet = session.previous
                after = read_screen(packet)
                if after['page'] not in WATCH_PAGES or after['overlay'] != 'none':
                    raise ValueError('PURCHASE_COUNTDOWN_WATCH_PAGE:' + str(after['page']))
                record = dict(requested_ms=ms, planned_window_ms=list(window), final_page=after['page'],
                              final_button_state=after['button_state'],
                              final_countdown_seconds=after['countdown_seconds'], watch=watch_summary(packet),
                              identity=listing_identity(packet), received_qpc_ms=qpc_ms())
                watches.append(record)
                if not same_listing(identity, record['identity']):
                    raise ValueError('PURCHASE_COUNTDOWN_LISTING_CHANGED')
                record['display_zero'] = clock.add_watch(packet['purchase_countdown_watch'],
                                                         anchor_seconds=after['countdown_seconds'])['display_zero']
                record['anchor_mismatch'] = clock.watches[-1]['anchor_mismatch']
                if clock.unlock()['observed']:
                    break
                if record['display_zero'] is None and len(watches) >= UNREAD_WATCH_LIMIT:
                    raise ValueError('PURCHASE_COUNTDOWN_LINE_UNREAD')
                if following and window[1] < -FINAL_TAIL_MS - 5000:
                    raise ValueError('PURCHASE_COUNTDOWN_UNLOCK_NOT_OBSERVED')
        final = clock.estimate()
        if final['display_zero'] is None and not final['unlock']['observed']:
            raise ValueError('PURCHASE_COUNTDOWN_NO_ESTIMATE:' + str(final.get('reason')))
        result['status'] = 'passed'
    except Exception as error:
        result['error'] = str(error) or type(error).__name__
    result.update(watches=watches, clock=clock.summary(), ide_restored=session.report.get('ide_restored'),
                  session_passed=session.report.get('passed'), config_sha256=snapshot['config_sha256'])
    result['standard_time'] = standard_time(result['clock'].get('display_zero'), (result['system_clock'] or {}).get('offset_ms'))
    if result['status'] == 'passed' and (result['ide_restored'] is not True or result['session_passed'] is False):
        result.update(status='blocked', error=result.get('error') or 'PURCHASE_COUNTDOWN_FOREGROUND_NOT_RETURNED')
    path = output / 'result.json'
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    zero = result['clock'].get('display_zero') or {}
    print(json.dumps(dict(status=result['status'], error=result.get('error'), ticks=result['clock'].get('ticks'),
                          display_zero_local_time=zero.get('estimate_local_time'), uncertainty_ms=zero.get('uncertainty_ms'),
                          consistent=zero.get('consistent'), period_ms=zero.get('period_ms'),
                          unlock=result['clock'].get('unlock'),
                          ntp_offset_ms=(result['system_clock'] or {}).get('offset_ms'),
                          standard_time=(result['standard_time'] or {}).get('estimate_local_time'),
                          standard_second_phase_ms=(result['standard_time'] or {}).get('second_phase_ms'),
                          anchor_mismatch_watches=result['clock'].get('anchor_mismatch_watches'),
                          verified=zero.get('verified')), ensure_ascii=False))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
