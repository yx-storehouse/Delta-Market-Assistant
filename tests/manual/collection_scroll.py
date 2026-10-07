"""Read-only collection geometry contracts. This module performs no OS input.

The old six-card packet has no dynamic-layout evidence and is intentionally not
upgraded here. A capture producer must emit ``collection_layout`` from the same
in-memory frame as ``collection_observation``. No coordinates, missing edges,
scrollbar positions or item identities are inferred from the previous frame.

Schema ``collection-layout-v1``::

    {same_frame: true, frame_id: str, frame_sha256: str,
     schema: "collection-layout-v1", detector: "visible_edges", complete: true,
     viewport: [width, height], listing_viewport: [x, y, width, height],
     cards: [{id: str, bounds: [x, y, width, height],
              edges: {top: bool, bottom: bool, left: bool, right: bool},
              fields_bounds: [x, y, width, height] | null, selected: bool}],
     scrollbar: {track_bounds: [x, y, width, height],
                 thumb_bounds: [x, y, width, height]} | null}

``bounds`` is an observed visible rectangle, not an extrapolation beyond the
viewport. ``complete`` means the detector enumerated this *visible viewport*;
it does not mean the entire page, task or listing tail has been scanned. A
clipped card may have a visible rectangle but must have an unobserved edge set
to false. A card is selectable only with all four observed edges and a fully
visible fields strip. Card ids are frame-local; leases expire on every capture.

Scrollbar motion proves a bounded scroll changed position. Missing or unmoved
scrollbars do not establish progress or end-of-list. Selection/scroll plans
always end with a fresh observation and never include a favorite/purchase.
"""

from copy import deepcopy
from decimal import Decimal
import hashlib
import json
from math import isfinite
import re

from collection_journal import candidate_key
from collection_labels import exact_label_target


SCHEMA = 'collection-layout-v1'
MAX_CARDS = 32
CONFIRMED = frozenset(('confirmed', 'confirmed_reconciliation'))
PENDING = frozenset(('prepared', 'dispatched', 'input_uncertain'))
_SIDES = frozenset(('top', 'bottom', 'left', 'right'))


def _fail(code):
    raise ValueError(code)


def _number(value):
    return type(value) in (int, float) and isfinite(value)


def _rect(value, code='COLLECTION_LAYOUT_RECT'):
    if not isinstance(value, (list, tuple)) or len(value) != 4:
        _fail(code)
    if any(type(v) is not int for v in value):
        _fail(code)
    x, y, width, height = value
    if x < 0 or y < 0 or width <= 0 or height <= 0:
        _fail(code)
    return list(value)


def _contains(outer, inner):
    x, y, w, h = outer
    a, b, c, d = inner
    return x <= a and y <= b and a + c <= x + w and b + d <= y + h


def _overlaps(a, b):
    return (max(a[0], b[0]) < min(a[0] + a[2], b[0] + b[2])
            and max(a[1], b[1]) < min(a[1] + a[3], b[1] + b[3]))


def _sha256(value):
    return (isinstance(value, str) and len(value) == 64
            and all(ch in '0123456789abcdef' for ch in value))


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                    ensure_ascii=False).encode('utf-8')).hexdigest()


def _frame(packet):
    if not isinstance(packet, dict):
        _fail('COLLECTION_FRAME_BINDING')
    page = packet.get('startup_page', {})
    if page.get('page') != 'skin_listings' or page.get('overlay') != 'none':
        _fail('COLLECTION_PAGE')
    projection = packet.get('collection_observation', {})
    frames = packet.get('frames')
    if not isinstance(frames, list) or not frames or not isinstance(frames[-1], dict):
        _fail('COLLECTION_FRAME_BINDING')
    frame = frames[-1]
    digest, frame_id = projection.get('frame_sha256'), projection.get('frame_id')
    if (projection.get('same_frame') is not True or not _sha256(digest)
            or frame.get('sha256') != digest or not isinstance(frame_id, str) or not frame_id):
        _fail('COLLECTION_FRAME_BINDING')
    dimensions = [frame.get('width'), frame.get('height')]
    if any(type(v) is not int or not 1 <= v <= 16384 for v in dimensions):
        _fail('COLLECTION_LAYOUT_VIEWPORT')
    return dict(frame_id=frame_id, frame_sha256=digest, viewport=dimensions)


def _scrollbar(value, viewport):
    if value is None:
        return None
    if not isinstance(value, dict):
        _fail('COLLECTION_SCROLLBAR_GEOMETRY')
    track = _rect(value.get('track_bounds'), 'COLLECTION_SCROLLBAR_GEOMETRY')
    thumb = _rect(value.get('thumb_bounds'), 'COLLECTION_SCROLLBAR_GEOMETRY')
    if (not _contains([0, 0, *viewport], track) or not _contains(track, thumb)
            or track[3] <= track[2] or thumb[2] > track[2] or thumb[3] >= track[3]):
        _fail('COLLECTION_SCROLLBAR_GEOMETRY')
    return dict(track_bounds=track, thumb_bounds=thumb)


def observe_layout(packet):
    """Validate same-frame detector output and return an immutable-by-copy lease.

    This accepts no legacy index-to-coordinate fallback. The returned partial
    ids are informational only; ``bind_card`` refuses to produce their points.
    """
    frame = _frame(packet)
    layout = packet.get('collection_layout')
    if not isinstance(layout, dict):
        _fail('COLLECTION_LAYOUT_MISSING')
    if (layout.get('schema') != SCHEMA or layout.get('detector') != 'visible_edges'
            or layout.get('complete') is not True):
        _fail('COLLECTION_LAYOUT_UNPROVEN')
    if (layout.get('same_frame') is not True
            or any(layout.get(k) != frame[k] for k in ('frame_id', 'frame_sha256'))):
        _fail('COLLECTION_LAYOUT_FRAME_BINDING')
    if layout.get('viewport') != frame['viewport']:
        _fail('COLLECTION_LAYOUT_VIEWPORT')
    viewport = _rect(layout.get('listing_viewport'))
    if not _contains([0, 0, *frame['viewport']], viewport):
        _fail('COLLECTION_LAYOUT_VIEWPORT')
    cards = layout.get('cards')
    if not isinstance(cards, list) or len(cards) > MAX_CARDS:
        _fail('COLLECTION_LAYOUT_CARD_COUNT')
    normalized, ids = [], set()
    for card in cards:
        if not isinstance(card, dict):
            _fail('COLLECTION_LAYOUT_CARD')
        card_id = card.get('id')
        if not isinstance(card_id, str) or not card_id or len(card_id) > 128 or card_id in ids:
            _fail('COLLECTION_LAYOUT_CARD_ID')
        ids.add(card_id)
        bounds = _rect(card.get('bounds'))
        if not _contains(viewport, bounds):
            _fail('COLLECTION_LAYOUT_CLIPPING')
        if any(_overlaps(bounds, prior['bounds']) for prior in normalized):
            _fail('COLLECTION_LAYOUT_OVERLAP')
        edges = card.get('edges')
        if (not isinstance(edges, dict) or set(edges) != _SIDES
                or any(type(v) is not bool for v in edges.values())):
            _fail('COLLECTION_LAYOUT_EDGES')
        fields = card.get('fields_bounds')
        if fields is not None:
            fields = _rect(fields)
            if not _contains(bounds, fields):
                _fail('COLLECTION_LAYOUT_FIELDS')
        if type(card.get('selected')) is not bool:
            _fail('COLLECTION_LAYOUT_SELECTION')
        # Require an observed card body above the fields, with room for a
        # center point that is inset from borders and outside the price strip.
        complete = all(edges.values()) and fields is not None
        body = None
        if complete:
            body = [bounds[0] + 4, bounds[1] + 4, bounds[2] - 8,
                    fields[1] - bounds[1] - 8]
            if body[2] <= 0 or body[3] <= 0:
                _fail('COLLECTION_LAYOUT_BODY')
        normalized.append(dict(id=card_id, bounds=bounds, fields_bounds=fields,
                               edges=dict(edges), selected=card['selected'],
                               selectable=complete, body_bounds=body))
    if sum(card['selected'] for card in normalized) > 1:
        _fail('COLLECTION_LAYOUT_SELECTION_AMBIGUOUS')
    normalized.sort(key=lambda card: (card['bounds'][1], card['bounds'][0]))
    result = dict(schema=SCHEMA, **frame, listing_viewport=viewport, cards=normalized,
                  scrollbar=_scrollbar(layout.get('scrollbar'), frame['viewport']),
                  partial_card_ids=[c['id'] for c in normalized if not c['selectable']],
                  page_exhausted=False, purchase_phase_enabled=False)
    result['layout_id'] = _digest(result)
    return result


def _scope(packet, rule):
    if (not isinstance(rule, dict) or rule.get('enabled') is not True
            or rule.get('dictionary_resolved') is not True
            or type(rule.get('row_index')) is not int or rule['row_index'] < 0
            or not isinstance(rule.get('product_name'), str) or not rule['product_name']
            or rule.get('condition_label') not in ('成色S', '成色A', '成色B', '成色C', '仅磨损')):
        _fail('COLLECTION_RULE_NOT_RESOLVED')
    if exact_label_target(packet, 'product_title', rule['product_name']) is None:
        _fail('COLLECTION_PRODUCT_MISMATCH')
    return {k: rule[k] for k in ('row_index', 'product_name', 'condition_label')}


def bind_card(packet, rule, card_id):
    """Bind a read-only card selection to observed geometry, never to an index."""
    layout = observe_layout(packet)
    scope = _scope(packet, rule)
    cards = [card for card in layout['cards'] if card['id'] == card_id]
    if len(cards) != 1:
        _fail('COLLECTION_LAYOUT_CARD_MISSING')
    card = cards[0]
    if not card['selectable']:
        _fail('COLLECTION_PARTIAL_CARD_REQUIRES_SCROLL')
    x, y, width, height = card['body_bounds']
    return dict(schema='collection-card-lease-v1', layout_id=layout['layout_id'],
                frame_id=layout['frame_id'], frame_sha256=layout['frame_sha256'],
                scope=scope, card=deepcopy(card), point=[x + width // 2, y + height // 2],
                viewport=layout['viewport'], purchase_phase_enabled=False)


def validate_card_lease(packet, rule, lease):
    """An old frame's card point must not survive any selection/scroll/capture."""
    if not isinstance(lease, dict) or lease.get('schema') != 'collection-card-lease-v1':
        _fail('COLLECTION_CARD_LEASE')
    current = bind_card(packet, rule, lease.get('card', {}).get('id'))
    if current != lease:
        _fail('COLLECTION_CARD_LEASE_EXPIRED')
    return deepcopy(current)


def selection_plan(packet, rule, card_id):
    """A finite plan for an injected executor; not accepted by the old runner.

    ``select_visible_card`` requires ``validate_card_lease`` immediately before
    input. The capture producer must then enumerate again, and the executor
    calls ``rebind_selected`` before parsing any candidate/price/wear. There is
    deliberately no favorite toggle in this plan.
    """
    lease = bind_card(packet, rule, card_id)
    return dict(schema='collection-geometry-plan-v1', timeout_seconds=15, steps=[
        dict(kind='select_visible_card', lease=lease, expected_before='skin_listings'),
        dict(kind='capture', expected_page='skin_listings', collection_observation=True,
             collection_layout=True, selected_geometry_required=True)],
        purchase_phase_enabled=False)


def rebind_selected(packet, rule, previous_lease):
    """Confirm selection in a *new* frame and bind the newly observed geometry.

    The selection capture must show exactly the target rectangle; frame-local
    ids may change. Item equality is NOT asserted here. The existing selected
    title/condition/price/wear matcher and durable journal remain mandatory.
    """
    layout = observe_layout(packet)
    scope = _scope(packet, rule)
    if (not isinstance(previous_lease, dict)
            or previous_lease.get('schema') != 'collection-card-lease-v1'
            or previous_lease.get('scope') != scope):
        _fail('COLLECTION_CARD_LEASE')
    if (layout['frame_id'] == previous_lease.get('frame_id')
            or layout['frame_sha256'] == previous_lease.get('frame_sha256')):
        _fail('COLLECTION_REOBSERVATION_REQUIRED')
    selected = [card for card in layout['cards'] if card['selected']]
    prior = previous_lease.get('card', {})
    if (len(selected) != 1 or selected[0]['bounds'] != prior.get('bounds')
            or selected[0]['fields_bounds'] != prior.get('fields_bounds')):
        _fail('COLLECTION_SELECTION_GEOMETRY_CHANGED')
    lease = bind_card(packet, rule, selected[0]['id'])
    # Keep evidence separate: a regular lease must remain byte-for-byte
    # comparable by validate_card_lease, without extra confirmation fields.
    return dict(lease=lease,
                selected_observed=True, previous_frame_id=previous_lease['frame_id'],
                item_identity_proven=False, collection_allowed=False)


def _identity(candidate, error):
    if (not isinstance(candidate, dict)
            or not isinstance(candidate.get('product'), str) or not candidate['product']
            or candidate.get('condition') not in ('成色S', '成色A', '成色B', '成色C')
            or not isinstance(candidate.get('wear'), str)
            or not re.fullmatch(r'[0-9]+\.[0-9]{1,9}', candidate['wear'])):
        _fail(error)
    # Same observed wear with a trailing zero is not a new listing. Keep the
    # original persistent journal key intact but compare decimal identities.
    return candidate['product'], candidate['condition'], Decimal(candidate['wear'])


def _journal_records(records):
    if not isinstance(records, (list, tuple)):
        _fail('COLLECTION_JOURNAL_RECORDS')
    seen, identities, normalized = set(), set(), []
    for record in records:
        if not isinstance(record, dict) or record.get('status') not in CONFIRMED | PENDING:
            _fail('COLLECTION_JOURNAL_STATUS')
        candidate = record.get('candidate', {})
        identity = _identity(candidate, 'COLLECTION_JOURNAL_IDENTITY')
        key = candidate_key(candidate)
        if record.get('key') != key or key in seen or identity in identities:
            _fail('COLLECTION_JOURNAL_IDENTITY')
        seen.add(key)
        identities.add(identity)
        normalized.append(record)
    return normalized


def collection_disposition(candidate, selected_card, records):
    """No-repeat decision after independent selected candidate association.

    This is *not* an input authorization. Unknown identity/color or any pending
    attempt stops. Same product/condition/wear colliding with a prior confirmed
    key is preserved even if position or price changed. A confirmed key shown
    white needs reconciliation, not another toggle.
    """
    records = _journal_records(records)
    if any(record['status'] in PENDING for record in records):
        _fail('COLLECTION_PENDING_RECONCILIATION')
    identity = _identity(candidate, 'COLLECTION_CANDIDATE_IDENTITY')
    if not isinstance(selected_card, dict) or selected_card.get('selected') is not True:
        _fail('COLLECTION_SELECTED_CARD_UNPROVEN')
    warm = selected_card.get('favorite_warm_fraction')
    bright = selected_card.get('favorite_bright_fraction')
    if any(not _number(value) or not 0 <= value <= 1 for value in (warm, bright)):
        _fail('COLLECTION_FAVORITE_COLOR_UNPROVEN')
    gold = warm >= .05 and bright <= .02
    white = warm == 0 and bright >= .04
    key = candidate_key(candidate)
    prior = [record for record in records
             if _identity(record['candidate'], 'COLLECTION_JOURNAL_IDENTITY') == identity]
    if prior:
        if not gold:
            _fail('COLLECTION_CONFIRMED_STATE_CONFLICT')
        return dict(disposition='preserve_confirmed', key=prior[0]['key'], allow_toggle=False)
    if gold:
        return dict(disposition='preserve_existing', key=key, allow_toggle=False)
    if not white:
        _fail('COLLECTION_FAVORITE_COLOR_UNPROVEN')
    return dict(disposition='unseen_white_requires_candidate_and_journal', key=key,
                allow_toggle=False)


def scroll_plan(packet, rule, records, *, delta=-120):
    """Plan one bounded read-only wheel step followed by mandatory observation.

    This does not decide that visible cards were fully scanned. The caller must
    persist its actual per-item coverage separately. No page-complete flag is
    produced, even when a scrollbar currently appears at its bottom.
    """
    layout = observe_layout(packet)
    scope = _scope(packet, rule)
    if type(delta) is not int or delta not in (-120, 120):
        _fail('COLLECTION_SCROLL_BOUND')
    records = _journal_records(records)
    if any(record['status'] in PENDING for record in records):
        _fail('COLLECTION_PENDING_RECONCILIATION')
    if layout['scrollbar'] is None:
        _fail('COLLECTION_SCROLL_PROGRESS_UNPROVEN')
    viewport = layout['listing_viewport']
    point = [viewport[0] + viewport[2] // 2, viewport[1] + viewport[3] // 2]
    token = dict(schema='collection-scroll-lease-v1', scope=scope,
                 layout_id=layout['layout_id'], frame_id=layout['frame_id'],
                 frame_sha256=layout['frame_sha256'], viewport=layout['viewport'],
                 listing_viewport=layout['listing_viewport'],
                 scrollbar=layout['scrollbar'], delta=delta, point=point)
    return dict(schema='collection-geometry-plan-v1', timeout_seconds=15, steps=[
        dict(kind='scroll_visible_list', lease=token, expected_before='skin_listings'),
        dict(kind='capture', expected_page='skin_listings', collection_observation=True,
             collection_layout=True)], page_exhausted=False, purchase_phase_enabled=False)


def validate_scroll_lease(packet, rule, records, lease):
    if not isinstance(lease, dict) or lease.get('schema') != 'collection-scroll-lease-v1':
        _fail('COLLECTION_SCROLL_LEASE')
    current = scroll_plan(packet, rule, records, delta=lease.get('delta'))['steps'][0]['lease']
    if current != lease:
        _fail('COLLECTION_SCROLL_LEASE_EXPIRED')
    return deepcopy(current)


def rebind_after_scroll(packet, rule, scroll_lease):
    """Prove wheel progress and expose only the new frame's selection targets.

    Changed image hash alone is insufficient. Track geometry must stay fixed,
    thumb length must stay stable, and its observed movement must agree with
    wheel direction. An unchanged bottom thumb is NOT an exhaustion receipt.
    """
    layout = observe_layout(packet)
    scope = _scope(packet, rule)
    if (not isinstance(scroll_lease, dict)
            or scroll_lease.get('schema') != 'collection-scroll-lease-v1'
            or scroll_lease.get('scope') != scope
            or scroll_lease.get('delta') not in (-120, 120)):
        _fail('COLLECTION_SCROLL_LEASE')
    if (layout['frame_id'] == scroll_lease.get('frame_id')
            or layout['frame_sha256'] == scroll_lease.get('frame_sha256')):
        _fail('COLLECTION_REOBSERVATION_REQUIRED')
    if (layout['viewport'] != scroll_lease.get('viewport')
            or layout['listing_viewport'] != scroll_lease.get('listing_viewport')):
        _fail('COLLECTION_SCROLL_VIEWPORT_CHANGED')
    before, after = scroll_lease.get('scrollbar'), layout['scrollbar']
    if not isinstance(before, dict) or after is None:
        _fail('COLLECTION_SCROLL_PROGRESS_UNPROVEN')
    old = _scrollbar(before, layout['viewport'])
    if old['track_bounds'] != after['track_bounds']:
        _fail('COLLECTION_SCROLL_TRACK_CHANGED')
    old_thumb, new_thumb = old['thumb_bounds'], after['thumb_bounds']
    if old_thumb[0] != new_thumb[0] or old_thumb[2:] != new_thumb[2:]:
        _fail('COLLECTION_SCROLL_CONTENT_CHANGED')
    shift = new_thumb[1] - old_thumb[1]
    if abs(shift) < 2:
        _fail('COLLECTION_SCROLL_PROGRESS_UNPROVEN')
    if (shift > 0) != (scroll_lease['delta'] < 0):
        _fail('COLLECTION_SCROLL_DIRECTION_CONFLICT')
    return dict(layout=layout, scrollbar_shift_pixels=shift,
                selectable_card_ids=[card['id'] for card in layout['cards'] if card['selectable']],
                partial_card_ids=layout['partial_card_ids'], requires_selected_reobservation=True,
                page_exhausted=False, collection_allowed=False, purchase_phase_enabled=False)
