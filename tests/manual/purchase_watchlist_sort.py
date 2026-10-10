"""Sort 我的关注 by 按稀有度升序 before buying.

User 2026-10-10: "那个搜藏界面需要点下筛选条件的,选择按稀有度升序这样快过期的才会排到前面来".
The sort label sits in the listing controls row (collection_observation
'listing_controls', y 230..300). When it does not read 按稀有度升序:
1. click the sort dropdown once;
2. read until the option 按稀有度升序 shows in the open list below it;
3. click that option's centre once (the open list is an overlay the page
   classifier may not know, so this click goes straight to the backend,
   bounded to the list area);
4. read back: the controls row must now read 按稀有度升序 and the list be closed.
Whether the list is open is read from its options on every read (a list left
open by an interrupted attempt is used or closed, never toggled blindly).
Anything unclear: a list seen open is closed, and the attempt goes on with
the current order (buying works with any order).
"""
import re
import unicodedata

RARITY_ASCENDING = '按稀有度升序'
# The open list (live sort02): seven options in this order.
SORT_OPTIONS = ('默认排序', '按稀有度升序', '按稀有度降序', '按价格升序', '按价格降序', '按成色升序', '按成色降序')
# The sort dropdown on 我的关注 (preview 705..1086 x 146..182 at 1600 px wide).
SORT_POINT = [1433, 262]
# Where its open list may show the options (below the dropdown, same column).
OPTION_AREA = (1100, 285, 700, 700)
SETTLE_READS = 4


def normalize(text):
    return re.sub(r'\s+', '', unicodedata.normalize('NFKC', str(text or '')))


def controls_text(packet):
    """The listing controls row as read (filter, notice, sort labels)."""
    for region in (packet.get('collection_observation') or {}).get('regions', []):
        if region.get('kind') == 'listing_controls' and region.get('ok') is True:
            words = sorted(region.get('words') or [], key=lambda w: w.get('x', 0))
            return normalize(''.join(w.get('text', '') for w in words))
    return None


def sorted_by(packet, label=RARITY_ASCENDING):
    text = controls_text(packet)
    return None if text is None else label in text


def sorted_by_rarity(packet):
    return sorted_by(packet, RARITY_ASCENDING)


def option_point(screen, label=RARITY_ASCENDING):
    """Centre of the open list's option `label`, or None. Only lines inside
    OPTION_AREA count (the dropdown's own label above it does not)."""
    x0, y0, w, h = OPTION_AREA
    hits = []
    for lines in screen['regions'].values():
        for line in lines:
            if normalize(line['text']) != label:
                continue
            words = line['words']
            left = min(word['x'] for word in words)
            top = min(word['y'] for word in words)
            right = max(word['x'] + word['width'] for word in words)
            bottom = max(word['y'] + word['height'] for word in words)
            if x0 <= left and right <= x0 + w and y0 <= top and bottom <= y0 + h:
                hits.append([int(round((left + right) / 2)), int(round((top + bottom) / 2))])
    unique = {tuple(hit) for hit in hits}
    return list(unique.pop()) if len(unique) == 1 else None


def list_open(screen):
    """The sort list shows any of its options below the dropdown. The page,
    overlay and controls row read the same open or closed (live sort02), so
    the options are the only evidence (review wf_da408028-592)."""
    return any(option_point(screen, option) is not None for option in SORT_OPTIONS)


def sort_step_permitted(step, plan):
    """The dropdown click through the session, once per arming, on 我的关注."""
    if (step.get('kind') == 'click' and step.get('expected_before') == 'watchlist_listings'
            and step.get('point') == SORT_POINT and plan.get('sort_armed')):
        plan['sort_armed'] = False
        plan['sort_clicks'] = plan.get('sort_clicks', 0) + 1
        return True
    return False


def ensure_rarity_sort(session, backend, read, plan, record, on_event=None, sleep=None, label=RARITY_ASCENDING,
                       check_stop=None):
    """Make 我的关注 sorted by `label` (按稀有度升序) and leave the list closed.
    Returns 'already', 'sorted', 'not_watchlist' or 'unchanged:<reason>'
    (an unclear screen never raises; the purchase goes on with the current
    order). Each read tells whether the list is open; the dropdown is
    clicked through the session (stop-checked), the option directly after
    `check_stop()`."""
    import time
    sleep = sleep or time.sleep
    check_stop = check_stop or (lambda: None)

    def toggle():
        plan['sort_armed'] = True
        try:
            session.perform(dict(kind='click', point=SORT_POINT, viewport=[2560, 1440], expected_before='watchlist_listings'))
        finally:
            plan['sort_armed'] = False

    def settle(done):
        screen, packet = None, None
        for attempt in range(SETTLE_READS):
            sleep(.15)
            screen, packet = read()
            if done(screen, packet):
                break
        return screen, packet

    def close(screen, packet):
        """Close the list only when it is seen open; then read it gone."""
        if not list_open(screen):
            return screen, packet
        toggle()
        screen, packet = settle(lambda s, p: not list_open(s))
        record['closed'] = not list_open(screen)
        return screen, packet

    screen, packet = read()
    record.update(before=controls_text(packet), page=screen['page'], open_at_start=list_open(screen))
    if screen['page'] != 'watchlist_listings' or screen['overlay'] != 'none':
        return 'not_watchlist'
    if sorted_by(packet, label):
        close(screen, packet)
        return 'already'
    if on_event:
        on_event('sorting')
    if not list_open(screen):
        toggle()
        screen, packet = settle(lambda s, p: option_point(s, label) is not None)
    point = option_point(screen, label)
    record.update(option_point=point, open_page=screen['page'])
    if point is None:
        close(screen, packet)
        return 'unchanged:OPTION_NOT_FOUND'
    check_stop()
    backend.click(point)
    record['option_clicks'] = 1
    record['option_dispatch'] = getattr(backend, 'last_dispatch', None)
    screen, packet = settle(lambda s, p: sorted_by(p, label) and not list_open(s))
    record.update(after=controls_text(packet))
    if list_open(screen):
        screen, packet = close(screen, packet)
    return 'sorted' if sorted_by(packet, label) else 'unchanged:NOT_CONFIRMED'
