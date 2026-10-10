"""Remove expired listings from the head of 我的关注 before a timed purchase.

User 2026-10-09 (screenshot: AS Val 黑银先锋 first, "剩余：2天23小时", no
countdown): "这种属于是超出时间了需要点击刷新按钮把它移除收藏池，然后这个游戏又有一个
机制，刚过期的枪光刷新可能不会马上消失，需要手动点击取消收藏再点刷新"; 21:50: "你直接刷新
啊把时间到了的清了呀" (live toast03: one refresh dropped all 23 expired listings);
2026-10-10: "不要在意收藏夹里面的皮肤是不是什么选中皮肤，他们能进收藏夹本来就是筛选过的…
不要管这么多细节".

What counts is the right panel: whatever listing it shows past its public
notice (no countdown, the remaining-time line above the price button) is
cleared, without checking which card or which skin it is:
1. 刷新;
2. still past its notice: 取消收藏 once if the star is gold, then 刷新 again.
At most MAX_EXPIRED rounds per attempt. Reads are repeated (no input in
between) until they settle.

Pure decisions here; the caller's session sends the steps.
"""

# Live preentry02 watchlist frame and the user's screenshot (2000 px x 1.28):
# the refresh icon at x 1805..1860, y 234..291; the detail star as in collection.
REFRESH_POINT = [1832, 262]
STAR_POINT = [2339, 320]
MAX_EXPIRED = 5
# Reads after an input (or of an unclear head) before deciding.
SETTLE_READS = 3


def star_state(packet):
    """'gold' (favorited), 'white' (not) or 'unknown', from the fixed star ROI
    of the same frame (the collection receipt thresholds)."""
    selected = packet.get('collection_selected_card', {})
    warm, bright = selected.get('favorite_warm_fraction'), selected.get('favorite_bright_fraction')
    if type(warm) not in (int, float) or type(bright) not in (int, float):
        return 'unknown'
    if warm >= .05 and bright <= .02:
        return 'gold'
    if warm == 0 and bright >= .04:
        return 'white'
    return 'unknown'


def expired_first_listing(screen):
    """The selected listing's public notice is over: no countdown at all,
    the remaining-time line and the price button."""
    return (screen['page'] == 'watchlist_listings' and screen['overlay'] == 'none'
            and screen['countdown_seconds'] is None and not screen['countdown_evidence']
            and not screen['countdown_unread'] and screen['button_state'] == 'price_button'
            and not screen['signals'])


def identity_readable(identity):
    """Title and exact wear read: identical cards differ only by their wear."""
    return bool(identity.get('product_title')) and bool(identity.get('selected_detail_precise'))


def head_state(screen, packet, first_card=None, identity_of=None):
    """'empty', 'counting' (the right panel's listing in its public notice),
    'expired' (past it, gold star), 'cleared' (past it, star not gold) or
    'unknown' (no input). `first_card` and `identity_of` are accepted for
    older callers and not used (user 2026-10-10: the panel is what counts)."""
    if screen['page'] == 'empty_watchlist':
        return 'empty'
    if screen['page'] != 'watchlist_listings' or screen['overlay'] != 'none':
        return 'unknown'
    if screen['countdown_seconds'] is not None:
        return 'counting'
    if not expired_first_listing(screen):
        return 'unknown'
    return 'expired' if star_state(packet) == 'gold' else 'cleared'


def cleanup_step_permitted(step, plan):
    """The star and the refresh button, each only when the plan armed it for
    the expired listing just verified, on the watchlist itself."""
    if step.get('kind') != 'click' or step.get('expected_before') != 'watchlist_listings':
        return False
    if step.get('point') == STAR_POINT and plan.get('star_armed'):
        plan['star_armed'] = False
        plan['star_clicks'] = plan.get('star_clicks', 0) + 1
        return True
    if step.get('point') == REFRESH_POINT and plan.get('refresh_armed'):
        plan['refresh_armed'] = False
        plan['refresh_clicks'] = plan.get('refresh_clicks', 0) + 1
        return True
    return False


def settle(read, done, tries=SETTLE_READS):
    """Re-read (no input) until done(screen, packet) or the tries run out."""
    screen, packet = read()
    for attempt in range(tries - 1):
        if done(screen, packet):
            break
        screen, packet = read()
    return screen, packet


def clean_expired_head(session, read, identity_of, same_listing, plan, record, on_event=None, first_card=None,
                       max_expired=MAX_EXPIRED):
    """Clear the expired listings off the head; return (final head state,
    last screen, last packet). `read()` returns (screen, packet) of a fresh
    read; every input goes through session.perform. `record` is updated as
    the steps go, so a stop still shows what was sent. `identity_of` only
    names the listing in the record and banner; `same_listing` and
    `first_card` are no longer used."""
    def state(screen, packet):
        return head_state(screen, packet)

    def gone(s, p):
        return state(s, p) not in ('unknown', 'expired', 'cleared')

    def click(point, armed):
        plan[armed] = True
        try:
            session.perform(dict(kind='click', point=point, viewport=[2560, 1440], expected_before='watchlist_listings'))
        finally:
            plan[armed] = False

    screen, packet = settle(read, lambda s, p: state(s, p) != 'unknown')
    for index in range(max_expired + 1):
        current = state(screen, packet)
        if current not in ('expired', 'cleared'):
            return current, screen, packet
        if index == max_expired:
            raise ValueError('PURCHASE_CLEANUP_TOO_MANY_EXPIRED')
        identity = identity_of(packet) if identity_of else {}
        entry = dict(identity=identity, star_before=star_state(packet), status='seen', refreshes=0)
        record.append(entry)
        if on_event:
            on_event('refresh', identity)
        click(REFRESH_POINT, 'refresh_armed')
        entry.update(status='refresh_sent', refreshes=1)
        screen, packet = settle(read, gone)
        if state(screen, packet) in ('expired', 'cleared'):
            # Just past its notice, a refresh alone may keep it: 取消收藏, then 刷新.
            if star_state(packet) == 'gold':
                if on_event:
                    on_event('unfavorite', identity_of(packet) if identity_of else {})
                click(STAR_POINT, 'star_armed')
                entry['status'] = 'star_sent'
                screen, packet = settle(read, lambda s, p: star_state(p) == 'white' or gone(s, p))
                entry['star_after'] = star_state(packet)
            if state(screen, packet) in ('expired', 'cleared'):
                click(REFRESH_POINT, 'refresh_armed')
                entry.update(status='refresh_sent', refreshes=2)
                screen, packet = settle(read, gone)
        if state(screen, packet) == 'unknown':
            screen, packet = settle(read, lambda s, p: state(s, p) != 'unknown')
        removed = state(screen, packet) not in ('expired', 'cleared', 'unknown')
        if not removed and identity_of and state(screen, packet) != 'unknown':
            # Record only (no input depends on it): another expired listing moved up.
            now = identity_of(packet)
            removed = (identity_readable(identity) and identity_readable(now)
                       and not (same_listing(identity, now) if same_listing else identity == now))
        entry.update(status='removed' if removed else 'still_listed', removed=removed)
        if removed and on_event:
            on_event('removed', identity)
    raise AssertionError('unreachable')
