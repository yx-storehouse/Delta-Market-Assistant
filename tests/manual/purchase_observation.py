"""Same-frame purchase evidence reader; no input, timers, network or disk I/O."""
import math
import re
import unicodedata
from copy import deepcopy


def normalize(text):
    return re.sub(r'\s+','',unicodedata.normalize('NFKC',text))


# The detail panel's buy button, a fixed control (live 2026-10-09
# live05_preview, 2560x1440: x 1978..2371, y 1206..1280). Its red price text is
# often unreadable for Windows OCR ("230" read as "02彐B", the coin icon as
# "0"), so the button is located by the layout, not by its text.
BUY_BUTTON_RECT = (1978, 1206, 393, 74)


# The countdown / remaining-time line above the button is [clock icon][gap]
# [text], centred on the button. OCR sometimes reads the icon as a character:
# live 2026-10-09 clock03/04 read "6" + "26分0秒" as "626分0秒" (626 minutes),
# both in the full frame and in the line read; clock05 merged it into the
# first word ("618" for 18). Measured on every live read: with the icon read
# the words centre at x 2176.5 and the text starts 24.6 px after the icon's
# left edge; without it the text alone centres at 2189-2190.
LINE_GROUP_CENTRE_X = 2176.5
LINE_TEXT_CENTRE_X = 2189.5
LINE_ICON_ADVANCE = 24.6
# The 外观购买 dialog repeats the line centred on its own button (x 1428..1824):
# dialog03 read the icon as "0" at x 1527 with the text from 1558 to 1724
# (group centre 1625.5, text centre 1641). Not yet measured in the countdown
# state, so its text centre is taken loosely; minutes below 60 still guard.
LINE_LAYOUTS = dict(footer=(LINE_GROUP_CENTRE_X, LINE_TEXT_CENTRE_X, LINE_ICON_ADVANCE),
                    dialog=(1625.5, 1640.0, 28.0))
DIALOG_LINE_BAND = (842, 882)
CORE_REGIONS = frozenset(('watch_header','listing_text','selected_detail','purchase_footer','dialog_text'))
# The short result toast near the top (native region 'toast', 2026-10-09):
# "订单尚未开放购买" (pressed before the unlock), "抢购队列已满，无法购买",
# "您已加入抢购池…". Older packets have no toast region.
REGION_KINDS = CORE_REGIONS | {'toast'}


def _centre(words):
    return (min(w['x'] for w in words) + max(w['x'] + w['width'] for w in words)) / 2


def strip_line_icon(words, layout='footer'):
    """(words without a read clock icon, the removed icon reading or None).

    Only a line centred as icon+text has its first character removed, as a
    word of its own or as the first character of the first word."""
    group_x, text_x, advance = LINE_LAYOUTS[layout]
    ordered = sorted(words, key=lambda w: w['x'])
    if len(ordered) < 2 or abs(_centre(ordered) - group_x) > 5:
        return ordered, None
    first, rest = ordered[0], ordered[1:]
    if len(first['text']) == 1:
        kept = rest
    elif first['width'] > advance + 4:
        kept = [dict(first, text=first['text'][1:], x=first['x'] + advance,
                     width=first['width'] - advance)] + rest
    else:
        return ordered, None
    if abs(_centre(kept) - text_x) > 5:
        return ordered, None
    return kept, dict(text=first['text'][0], x=first['x'], y=first['y'], merged=len(first['text']) > 1)


def countdown_seconds(text):
    """Literal screen grammar; never fix O/0 or accept a partial OCR suffix."""
    if not isinstance(text,str) or len(text)>80:
        raise ValueError('PURCHASE_COUNTDOWN_TEXT')
    value=normalize(text)
    # Minutes below 60 always: hours have their own field, and a clock icon
    # read as a leading digit ("626分0秒", live clock03) then fails as unread
    # instead of becoming ten hours.
    m=re.fullmatch(r'(?:(\d{1,2})小时)?(?:(\d{1,2})分)?(\d{1,2})秒后解锁购买',value)
    if not m:raise ValueError('PURCHASE_COUNTDOWN_FORMAT')
    h,mn,s=(int(x or 0) for x in m.groups())
    if s>=60 or mn>=60 or h*3600+mn*60+s>86400:
        raise ValueError('PURCHASE_COUNTDOWN_RANGE')
    return h*3600+mn*60+s


def text_lines(words,bounds):
    if not isinstance(words,list) or len(words)>2000:raise ValueError('PURCHASE_WORDS')
    if (not isinstance(bounds,list) or len(bounds)!=4
        or any(type(v) is not int for v in bounds) or min(bounds[:2])<0 or min(bounds[2:])<=0):
        raise ValueError('PURCHASE_REGION_BOUNDS')
    x,y,w,h=bounds;groups=[]
    for word in sorted(words,key=lambda a:(a.get('y',-1),a.get('x',-1)) if isinstance(a,dict) else (-1,-1)):
        if not isinstance(word,dict) or not isinstance(word.get('text'),str) or not word['text'] or len(word['text'])>1024:
            raise ValueError('PURCHASE_WORD')
        if any(type(word.get(k)) not in (int,float) or not math.isfinite(word[k]) for k in ('x','y','width','height')):
            raise ValueError('PURCHASE_WORD_BOUNDS')
        a,b,c,d=(word[k] for k in ('x','y','width','height'))
        if c<=0 or d<=0 or not (x<=a and y<=b and a+c<=x+w and b+d<=y+h):raise ValueError('PURCHASE_WORD_BOUNDS')
        score=word.get('score',1.)
        if type(score) not in (int,float) or not math.isfinite(score) or not 0<=score<=1:raise ValueError('PURCHASE_WORD_SCORE')
        if score<.97:continue
        center=b+d/2
        # Compare against the first token, not a drifting adjacent-line chain.
        matches=[g for g in groups if abs(g['center']-center)<=min(g['height'],d)*.42]
        # A word close to two lines joins the nearer one (live buy03_01: a
        # 2 px high dash in a card title made the whole read fail). Lines are
        # text evidence only; a wrong join can only leave a line unread.
        matches.sort(key=lambda g:abs(g['center']-center))
        if matches:matches[0]['words'].append(word)
        else:groups.append(dict(center=center,height=d,words=[word]))
    out=[]
    for group in groups:
        members=sorted(group['words'],key=lambda a:a['x']);runs=[]
        for word in members:
            if runs and word['x']-(runs[-1][-1]['x']+runs[-1][-1]['width'])<=max(word['height'],runs[-1][-1]['height'])*1.3:
                runs[-1].append(word)
            else:runs.append([word])
        out.extend(dict(text=''.join(w['text'] for w in run),words=deepcopy(run)) for run in runs)
    return out


def read_screen(packet):
    p=packet.get('purchase_observation',{})
    frames=packet.get('frames',[])
    if (packet.get('capture_passed') is not True or packet.get('ocr_passed') is not True
        or not frames or p.get('schema')!='purchase-observation-v1' or p.get('valid') is not True
        or p.get('same_frame') is not True or p.get('actions_enabled') is not False
        or p.get('purchase_authorized') is not False or not isinstance(p.get('frame_id'),str) or not p['frame_id']
        or not re.fullmatch(r'[0-9a-f]{64}',str(p.get('frame_sha256','')))
        or p['frame_sha256']!=frames[-1].get('sha256')
        or p.get('viewport')!=[frames[-1].get('width'),frames[-1].get('height')]):
        raise ValueError('PURCHASE_FRAME_BINDING')
    page=packet.get('startup_page',{})
    if p.get('page')!=page.get('page') or p.get('overlay')!=page.get('overlay'):raise ValueError('PURCHASE_PAGE_BINDING')
    if p.get('geometry_role')!='text_observation_only_not_click_targets':raise ValueError('PURCHASE_OBSERVATION_SCOPE')
    regions={}
    for region in p.get('regions',[]):
        kind=region.get('kind')
        if kind in regions or kind not in REGION_KINDS:
            raise ValueError('PURCHASE_REGIONS')
        if (region.get('ok') is not True or region.get('truncated') is not False or region.get('same_frame') is not True
            or any(region.get(k)!=p[k] for k in ('frame_id','frame_sha256'))):raise ValueError('PURCHASE_REGION_FRAME')
        x,y,w,h=region.get('bounds',[0,0,0,0])
        if x<0 or y<0 or x+w>p['viewport'][0] or y+h>p['viewport'][1]:raise ValueError('PURCHASE_REGION_BOUNDS')
        regions[kind]=text_lines(region.get('words'),region.get('bounds'))
    if not CORE_REGIONS<=set(regions):raise ValueError('PURCHASE_REGIONS')
    regions.setdefault('toast',[])
    bx,by,bw,bh=BUY_BUTTON_RECT
    footer=[];icons=[]
    for line in regions['purchase_footer']:
        words=line['words']
        if all(w['y']+w['height']<=by for w in words):
            words,icon=strip_line_icon(words)
            if icon is not None:icons.append(dict(text=icon['text'],x=icon['x'],y=icon['y']))
        footer.append(normalize(''.join(w['text'] for w in words)))
    countdowns=[];unread=[]
    for text in footer:
        if '后解锁购买' not in text:continue
        try:countdowns.append(dict(text=text,seconds=countdown_seconds(text)))
        except ValueError:unread.append(text)
    remaining=countdowns[0]['seconds'] if len(countdowns)==1 and not unread else None
    # After the public notice the footer shows the listing's remaining time
    # ("剩余：2天23小时") above a button that carries only the price ("230"),
    # no 购买 text (live 2026-10-09 purchase_readonly/live05_preview).
    def in_button(line):
        return all(bx<=w['x'] and by<=w['y'] and w['x']+w['width']<=bx+bw and w['y']+w['height']<=by+bh
                   for w in line['words'])
    labels=[line for line in regions['purchase_footer'] if in_button(line)]
    remaining_shown=any(text.startswith('剩余') for text in footer)
    button=('publicity' if '公示中' in footer else 'buy_visible' if '购买' in footer else 'unknown')
    # During the public notice the button itself reads 公示中: not a conflict.
    if '公示中' in footer and '购买' in footer:button='conflict'
    elif button=='unknown' and remaining_shown and labels and not countdowns and not unread:
        button='price_button'
    elif button=='unknown' and remaining is not None and labels and not remaining_shown:
        # User screenshot 2026-10-09: in the last seconds the countdown still
        # runs while the button is already green with the price; the dialog
        # can then be opened ahead of the unlock.
        button='price_ready'
    price_button=None
    if button=='price_button':
        text=''.join(normalize(line['text']) for line in labels)
        # Text evidence only: the caller decides whether and where to click.
        price_button=dict(text=text,price=text if re.fullmatch(r'[1-9][0-9]{0,8}',text) else None,
                          bounds=list(BUY_BUTTON_RECT),located_by='fixed_layout_with_text_inside')
    dialog_line=None
    lo,hi=DIALOG_LINE_BAND
    for line in regions['dialog_text']:
        if not all(lo<=w['y'] and w['y']+w['height']<=hi for w in line['words']):continue
        words,_=strip_line_icon(line['words'],'dialog')
        text=normalize(''.join(w['text'] for w in words))
        if '后解锁购买' in text or '剩余' in text:
            dialog_line=text
    dialog_countdown=None
    if dialog_line and '后解锁购买' in dialog_line:
        try:dialog_countdown=countdown_seconds(dialog_line)
        except ValueError:dialog_countdown=None
    signals=[]
    names={'获得枪械外观':'success_text','该商品已被购买或已下架':'sold_or_removed',
        '队列已满':'queue_full','仍在公示期':'still_publicity','余额不足':'insufficient_balance',
        '当前订单还在公示期内':'still_publicity','确认购买':'confirm_dialog',
        '外观购买':'purchase_dialog','购买结果公示':'result_publicity',
        # Live 2026-10-09 (purchase_readonly/dialog02): with 7 三角币 for a 230
        # listing the price button opens "您当前的三角币不足，是否前往充值？
        # （缺少223三角币，推荐充值¥30档位）" with 取消 and 充值 (real-money top-up).
        '三角币不足':'insufficient_balance','是否前往充值':'recharge_prompt',
        # BBZPS log result texts (docs/PURCHASE_READONLY.md): a pooled draw,
        # its loss, crowding and a not-yet-open order.
        '加入抢购池':'lottery_pool','运气不佳':'lottery_lost','抢购请求玩家过多':'too_many_buyers',
        # User 2026-10-09 (screenshot at 0分1秒): the toast after pressing the
        # dialog's green button before the unlock ("点快了").
        '订单尚未开放购买':'not_open_yet'}
    shortfall=None
    seen=set();toast=[]
    for region in ('dialog_text','toast'):
        for line in regions[region]:
            text=normalize(line['text'])
            kinds=[kind for phrase,kind in names.items() if phrase in text]
            if region=='toast' and text:toast.append(dict(text=text,kinds=kinds))
            for kind in kinds:
                if (kind,text) not in seen:
                    seen.add((kind,text));signals.append(dict(kind=kind,text=text,region=region))
            m=re.search(r'缺少([1-9][0-9]{0,8})三角币',text)
            if m:shortfall=int(m.group(1))
    return dict(schema='purchase-screen-readonly-v1',frame_id=p['frame_id'],frame_sha256=p['frame_sha256'],
        page=p['page'],overlay=p['overlay'],countdown_seconds=remaining,countdown_evidence=countdowns,
        countdown_unread=unread,countdown_icon_words_removed=icons,button_state=button,
        dialog_line=dialog_line,dialog_countdown_seconds=dialog_countdown,
        dialog_unlocked=bool(dialog_line and dialog_line.startswith('剩余')),price_button=price_button,signals=signals,regions=regions,
        toast=toast,
        balance_shortfall=shortfall,
        countdown_ambiguous=len(countdowns)>1,
        screen_result_is_order_receipt=False,actions_enabled=False,purchase_authorized=False,
        source_frame=deepcopy(frames[-1]))


def match_current_watchlist(packet,snapshot):
    """Original matcher and current configured thresholds; no imported history."""
    from collection_candidate import match_watchlist_selected
    from collection_labels import exact_label_target
    if snapshot.get('schema')!='collection-run-snapshot-v1' or snapshot.get('ready') is not True:
        raise ValueError('PURCHASE_RULE_SNAPSHOT')
    screen=read_screen(packet)
    if screen['page']=='empty_watchlist':
        return dict(screen=screen,decision='Empty',candidates=[],reasons=[],actions_enabled=False,config_sha256=snapshot['config_sha256'])
    if screen['page']!='watchlist_listings' or screen['overlay']!='none':
        return dict(screen=screen,decision='NeedsReview',candidates=[],reasons=['PURCHASE_WATCHLIST_PAGE'],actions_enabled=False,config_sha256=snapshot['config_sha256'])
    matches=[];reasons=[]
    for rule in snapshot['rows']:
        if not rule.get('enabled') or exact_label_target(packet,'product_title',rule['product_name']) is None:continue
        try:
            candidate=match_watchlist_selected(packet,rule)
            if candidate['eligible']:matches.append(candidate)
            else:reasons.append('RULE_NOT_MATCHED:'+str(rule['row_index']))
        except ValueError as error:reasons.append(str(error)+':'+str(rule['row_index']))
    decision='Match' if len(matches)==1 else 'NeedsReview' if len(matches)>1 or any(not r.startswith('RULE_NOT_MATCHED') for r in reasons) else 'NoMatch'
    if len(matches)>1:reasons.append('PURCHASE_OVERLAPPING_RULES')
    if not matches and not reasons:reasons.append('PURCHASE_NO_CONFIGURED_PRODUCT')
    return dict(screen=screen,decision=decision,candidates=matches,reasons=reasons,
        config_sha256=snapshot['config_sha256'],
        actions_enabled=False,purchase_authorized=False,quantity_policy_applied=False)
