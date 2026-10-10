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

from collection_journal import candidate_key, validate_record_identity
from collection_labels import exact_label_target


SCHEMA = 'collection-layout-v1'
MAX_CARDS = 32
CONFIRMED = frozenset(('confirmed', 'confirmed_reconciliation'))
PENDING = frozenset(('prepared', 'dispatched', 'input_uncertain'))
_SIDES = frozenset(('top', 'bottom', 'left', 'right'))
BATCH_SCROLL_DELTAS = frozenset((-600, -480, 480, 600))
SCROLL_DELTAS = BATCH_SCROLL_DELTAS | frozenset((-120, 120))
HORIZONTAL_GEOMETRY_TOLERANCE_PX = 8
# The native scrollbar is the single best-contrast pixel column of a thin
# translucent bar; that column moves between adjacent pixels while the bar
# does not (run04 1877/1878; hotkey 20261009-091329 1878/1879 stopped a row
# because a measured profile demanded x == 1878). Only vertical extents carry
# scroll distance; x merely names the same bar.
SCROLLBAR_CENTERLINE_TOLERANCE_PX = 1
MIN_SMALLER_CARD_COVERAGE_PERCENT = 98


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


def _frame(packet, require_page=True):
    if not isinstance(packet, dict):
        _fail('COLLECTION_FRAME_BINDING')
    page = packet.get('startup_page', {})
    if require_page and (page.get('page') != 'skin_listings' or page.get('overlay') != 'none'):
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


def same_scrollbar_track(a, b):
    """Same track: equal vertical extent and width, centreline jitter only."""
    return a[1:] == b[1:] and abs(a[0] - b[0]) <= SCROLLBAR_CENTERLINE_TOLERANCE_PX


def same_scrollbar_position(a, b):
    """Same bar and an unmoved thumb (both None also counts as the same)."""
    if a is None or b is None:
        return a is None and b is None
    return (same_scrollbar_track(a['track_bounds'], b['track_bounds'])
            and a['thumb_bounds'][0] - a['track_bounds'][0] == b['thumb_bounds'][0] - b['track_bounds'][0]
            and a['thumb_bounds'][1:] == b['thumb_bounds'][1:])


def _stable_lower_band(a, b, fa, fb, tolerance=3):
    """Anchor a top-edge discrepancy to independently measured lower fields."""
    inset_a = a[1] + a[3] - fa[1] - fa[3]
    inset_b = b[1] + b[3] - fb[1] - fb[3]
    return (fa[3] == fb[3] and inset_a == inset_b and 0 <= inset_a <= 3
            and abs(a[1] + a[3] - b[1] - b[3]) <= tolerance
            and abs(fa[1] - fb[1]) <= tolerance
            and abs(fa[1] + fa[3] - fb[1] - fb[3]) <= tolerance)


def _reading_order(cards):
    """Order measured rows left-to-right with bounded edge discrepancies.

    A row must agree pairwise, preventing chained top offsets from merging
    distinct rows. Both rectangles must overlap vertically by at least 95%
    of the taller one, and clipped/complete cards never share a row group.
    This changes enumeration only, never any observed bounds or input lease.
    """
    def same_row(a, b):
        left, right = a['bounds'], b['bounds']
        overlap = max(0, min(left[1] + left[3], right[1] + right[3])
                      - max(left[1], right[1]))
        top_delta = abs(left[1] - right[1])
        top_agrees = top_delta <= 3 or (
            top_delta <= 5 and a['selectable'] and b['selectable']
            and _stable_lower_band(left, right, a['fields_bounds'], b['fields_bounds']))
        return (a['selectable'] == b['selectable']
                and top_agrees
                and overlap * 100 >= max(left[3], right[3]) * 95)

    rows = []
    for card in sorted(cards, key=lambda c: (c['bounds'][1], c['bounds'][0])):
        for row in rows:
            if all(same_row(card, prior) for prior in row):
                row.append(card)
                break
        else:
            rows.append([card])
    rows.sort(key=lambda row: (min(c['bounds'][1] for c in row),
                               min(c['bounds'][0] for c in row)))
    return [card for row in rows for card in sorted(row, key=lambda c: c['bounds'][0])]


def transient_layout_failure(packet):
    """Recognize only a bound native edge miss, not invalid/stale geometry.

    A new frame may be observed at most twice more by the session; this does
    not establish any card geometry or clear a pending selection/scroll.
    """
    try:
        frame = _frame(packet)
        layout = packet.get('collection_layout')
        if (not isinstance(layout, dict) or layout.get('schema') != SCHEMA
                or layout.get('detector') != 'visible_edges' or layout.get('complete') is not False
                or layout.get('same_frame') is not True or layout.get('cards') != []
                or any(layout.get(k) != frame[k] for k in ('frame_id', 'frame_sha256', 'viewport'))
                or layout.get('error') not in ('E_COLLECTION_ROW_EDGES',
                    'E_COLLECTION_CARD_HORIZONTAL_EDGE', 'E_COLLECTION_CARD_VERTICAL_EDGE',
                    'E_COLLECTION_PARTIAL_SPANS_ROWS')):
            return False
        viewport = _rect(layout.get('listing_viewport'))
        if not _contains([0, 0, *frame['viewport']], viewport):
            return False
        if _scrollbar(layout.get('scrollbar'), frame['viewport']) is None:
            return False
    except (ValueError, KeyError, TypeError):
        return False
    return True


def observe_layout(packet, *, require_page=True):
    """Validate same-frame detector output and return an immutable-by-copy lease.

    This accepts no legacy index-to-coordinate fallback. The returned partial
    ids are informational only; ``bind_card`` refuses to produce their points.
    ``require_page=False`` is only for pixel receipt packets, which carry no
    page OCR; such a layout never yields a lease by itself.
    """
    frame = _frame(packet, require_page)
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
        basis = card.get('bounds_basis')
        if basis is not None:
            if basis not in ('observed', 'clipped_search_region'):
                _fail('COLLECTION_LAYOUT_BOUNDS_BASIS')
            if basis == 'clipped_search_region' and (complete or fields is not None
                    or (edges['left'] and edges['right'])
                    or (edges['top'] and edges['bottom'])
                    or 'condition_bounds' in card or 'price_bounds' in card):
                _fail('COLLECTION_LAYOUT_BOUNDS_BASIS')
        body = None
        if complete:
            body = [bounds[0] + 4, bounds[1] + 4, bounds[2] - 8,
                    fields[1] - bounds[1] - 8]
            if body[2] <= 0 or body[3] <= 0:
                _fail('COLLECTION_LAYOUT_BODY')
        item = dict(id=card_id, bounds=bounds, fields_bounds=fields,
                    edges=dict(edges), selected=card['selected'],
                    selectable=complete, body_bounds=body)
        if basis is not None:
            item['bounds_basis'] = basis
        for name in ('condition_bounds', 'price_bounds'):
            if name in card:
                region = _rect(card[name], 'COLLECTION_LAYOUT_FIELD_ROI')
                if fields is None or not _contains(fields, region):
                    _fail('COLLECTION_LAYOUT_FIELD_ROI')
                item[name] = region
        if ('condition_bounds' in item and 'price_bounds' in item
                and _overlaps(item['condition_bounds'], item['price_bounds'])):
            _fail('COLLECTION_LAYOUT_FIELD_ROI')
        normalized.append(item)
    if sum(card['selected'] for card in normalized) > 1:
        _fail('COLLECTION_LAYOUT_SELECTION_AMBIGUOUS')
    normalized = _reading_order(normalized)
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
                # Use the observed body's upper-left interior, below its label.
                # The run03 hover hint followed this point too: this geometry
                # choice alone does not establish reliable game selection.
                # It remains inside THIS fresh card, never a fixed screen slot;
                # the next frame must still prove this very card was selected.
                scope=scope, card=deepcopy(card), point=[x + min(48, width // 8), y + min(40, height // 4)],
                point_policy='observed_body_upper_left_gutter_v1',
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
    """A finite plan for an injected executor and ForegroundSession.

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


def card_geometry_match(before, after, *, tolerance=3, horizontal_tolerance=HORIZONTAL_GEOMETRY_TOLERANCE_PX):
    """Explain a bounded geometry association, or return None on disagreement.

    Run_07 measured a 5px top discrepancy with only 2px lower-field motion.
    Only that top edge may exceed the usual 3px vertical bound: the field band
    must keep its height and its 0--3px bottom inset. Bottom/field edges remain
    bounded to 3px; x remains 8px, and smaller-card coverage remains >=98%.
    This does not identify an item or assert a cause for the edge discrepancy.
    """
    if type(tolerance) is not int or not 0 <= tolerance <= 3:
        return None
    if type(horizontal_tolerance) is not int or not 0 <= horizontal_tolerance <= HORIZONTAL_GEOMETRY_TOLERANCE_PX:
        return None
    if not isinstance(before, dict) or not isinstance(after, dict):
        return None
    try:
        a = _rect(before.get('bounds', before.get('card_bounds')))
        b = _rect(after.get('bounds', after.get('card_bounds')))
        fa = _rect(before.get('fields_bounds'))
        fb = _rect(after.get('fields_bounds'))
    except ValueError:
        return None
    if not _contains(a, fa) or not _contains(b, fb):
        return None
    def edges(rect):
        return rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]
    extended_top = abs(a[1] - b[1]) > tolerance
    top_limit = 5 if tolerance == 3 else tolerance
    if extended_top and not _stable_lower_band(a, b, fa, fb, tolerance):
        return None
    card_limits = (horizontal_tolerance, top_limit, horizontal_tolerance, tolerance)
    field_limits = (horizontal_tolerance, tolerance, horizontal_tolerance, tolerance)
    for left, right, limits, height_limit in (
            (a, b, card_limits, (top_limit + tolerance) if extended_top else 2*tolerance),
            (fa, fb, field_limits, 2*tolerance)):
        if any(abs(x-y) > limit for x, y, limit in zip(edges(left), edges(right), limits)):
            return None
        if abs(left[2]-right[2]) > 2*horizontal_tolerance or abs(left[3]-right[3]) > height_limit:
            return None
    width = max(0, min(a[0]+a[2], b[0]+b[2]) - max(a[0], b[0]))
    height = max(0, min(a[1]+a[3], b[1]+b[3]) - max(a[1], b[1]))
    smaller_area = min(a[2]*a[3], b[2]*b[3])
    if width*height*100 < MIN_SMALLER_CARD_COVERAGE_PERCENT*smaller_area:
        return None
    names = ('left', 'top', 'right', 'bottom')
    return dict(mode='anchored_top_edge' if extended_top else 'bounded_edges',
                card_edge_deltas=dict(zip(names, (y-x for x, y in zip(edges(a), edges(b))))),
                fields_edge_deltas=dict(zip(names, (y-x for x, y in zip(edges(fa), edges(fb))))),
                card_edge_limits=dict(zip(names, card_limits)),
                fields_edge_limits=dict(zip(names, field_limits)),
                fields_bottom_insets=[a[1]+a[3]-fa[1]-fa[3], b[1]+b[3]-fb[1]-fb[3]],
                smaller_card_coverage=width*height/smaller_area,
                discrepancy_cause='unproven', item_identity_proven=False,
                old_card_coordinates_reused=False)


def same_card_geometry(before, after, *, tolerance=3, horizontal_tolerance=8):
    """Boolean geometry association; all item fields still require fresh OCR."""
    return card_geometry_match(before, after, tolerance=tolerance,
                               horizontal_tolerance=horizontal_tolerance) is not None


def layouts_equivalent(before, after):
    """Two observed layouts show the same complete cards in the same places.

    Used after a favorite toggle, which changes only the detail star: every
    complete (selectable) card must map one-to-one with equal edges and
    selection within the usual geometry tolerance; viewports must be
    identical, the scrollbar track keeps its vertical extent and its measured
    one-pixel centreline column within 1 px (run04: 1877/1878), and the thumb
    keeps its column relative to the track and its top edge (+/-1 px). The thumb may shorten when more listings load below
    without moving the view (S11 pipeline run02: 62 -> 47 px at the list top).
    Clipped tail members are never input targets and their measured edges
    jitter between static frames (run01: 980/990 right edge), so they are not
    compared. This proves no list movement, never item identity.
    """
    try:
        complete_before = [card for card in before['cards'] if card['selectable']]
        complete_after = [card for card in after['cards'] if card['selectable']]
        bar_a, bar_b = before['scrollbar'], after['scrollbar']
        if (bar_a is None) != (bar_b is None) or (bar_a is not None and (
                not same_scrollbar_track(bar_a['track_bounds'], bar_b['track_bounds'])
                or bar_a['thumb_bounds'][0] - bar_a['track_bounds'][0] != bar_b['thumb_bounds'][0] - bar_b['track_bounds'][0]
                or bar_a['thumb_bounds'][2] != bar_b['thumb_bounds'][2]
                or abs(bar_a['thumb_bounds'][1] - bar_b['thumb_bounds'][1]) > 1)):
            return False
        if (before['frame_id'] == after['frame_id'] or before['viewport'] != after['viewport']
                or before['listing_viewport'] != after['listing_viewport']
                or len(complete_before) != len(complete_after) or not complete_before):
            return False
        def member(old, new):
            return (old['edges'] == new['edges'] and old['selected'] == new['selected']
                    and old.get('bounds_basis') == new.get('bounds_basis') and same_card_geometry(old, new))
        matches = [[index for index, new in enumerate(complete_after) if member(old, new)]
                   for old in complete_before]
        return (all(len(indices) == 1 for indices in matches)
                and len({indices[0] for indices in matches}) == len(matches))
    except (KeyError, TypeError, IndexError):
        return False


def rebind_selected(packet, rule, previous_lease):
    """Confirm selection in a *new* frame and bind the newly observed geometry.

    The selection capture must show the target rectangle within the measured
    gray/white border tolerance; frame-local ids may change. Item equality is
    NOT asserted here. The existing selected
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
    matches = [(card, card_geometry_match(prior, card)) for card in layout['cards']
               if card['selectable']]
    matches = [(card, evidence) for card, evidence in matches if evidence is not None]
    if (len(selected) != 1 or len(matches) != 1
            or matches[0][0]['id'] != selected[0]['id']):
        _fail('COLLECTION_SELECTION_GEOMETRY_CHANGED')
    geometry_match = dict(matches[0][1], matching_card_count=len(matches),
                          selected_card_id=selected[0]['id'])
    lease = bind_card(packet, rule, selected[0]['id'])
    # Keep evidence separate: a regular lease must remain byte-for-byte
    # comparable by validate_card_lease, without extra confirmation fields.
    return dict(lease=lease, geometry_match=geometry_match,
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
    seen, normalized = set(), []
    for record in records:
        if not isinstance(record, dict) or record.get('status') not in CONFIRMED | PENDING:
            _fail('COLLECTION_JOURNAL_STATUS')
        candidate = record.get('candidate', {})
        _identity(candidate, 'COLLECTION_JOURNAL_IDENTITY')
        key = validate_record_identity(record)
        if key in seen:
            _fail('COLLECTION_JOURNAL_IDENTITY')
        seen.add(key)
        normalized.append(record)
    return normalized


def collection_disposition(candidate, selected_card, records):
    """No-repeat decision after independent selected candidate association.

    This is *not* an input authorization. Unknown identity/color or any pending
    attempt stops. Completed history is statistical evidence, not current star
    state: a newly observed eligible white star may create a separate attempt.
    Gold remains untouched regardless of whether history contains the item.
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
    if prior and gold:
        return dict(disposition='preserve_confirmed', key=prior[0]['key'], allow_toggle=False)
    if gold:
        return dict(disposition='preserve_existing', key=key, allow_toggle=False)
    if not white:
        _fail('COLLECTION_FAVORITE_COLOR_UNPROVEN')
    if prior:
        return dict(disposition='observed_white_requires_new_attempt', key=key,
                    historical_attempt_keys=[record['key'] for record in prior],
                    history_role='statistics_only', current_star='white', allow_toggle=False)
    return dict(disposition='unseen_white_requires_candidate_and_journal', key=key,
                allow_toggle=False)


def validate_scroll_calibration(profile):
    """Freeze an explicit measured batch profile; a single notch is not four.

    The source reference is retained for the coordinator's hash audit. This
    structural validator does not claim to have conducted the referenced live
    experiment, nor that any list item's identity survives scrolling.
    """
    if not isinstance(profile, dict) or profile.get('schema') != 'collection-scroll-calibration-v1':
        _fail('COLLECTION_SCROLL_CALIBRATION')
    if type(profile.get('delta')) is not int or profile['delta'] not in BATCH_SCROLL_DELTAS:
        _fail('COLLECTION_SCROLL_CALIBRATION_DELTA')
    source = profile.get('source')
    if (not isinstance(source, dict)
            or any(not isinstance(source.get(key), str) or not source[key]
                   for key in ('record_path', 'before_frame_id', 'after_frame_id', 'measurement_method'))
            or source['before_frame_id'] == source['after_frame_id']
            or not _sha256(source.get('record_sha256'))):
        _fail('COLLECTION_SCROLL_CALIBRATION_SOURCE')
    viewport = profile.get('viewport')
    if (not isinstance(viewport, list) or len(viewport) != 2
            or any(type(value) is not int or not 1 <= value <= 16384 for value in viewport)):
        _fail('COLLECTION_SCROLL_CALIBRATION_VIEWPORT')
    listing = _rect(profile.get('listing_viewport'), 'COLLECTION_SCROLL_CALIBRATION_VIEWPORT')
    if not _contains([0, 0, *viewport], listing):
        _fail('COLLECTION_SCROLL_CALIBRATION_VIEWPORT')
    def interval(value, maximum):
        return (isinstance(value, list) and len(value) == 2 and all(_number(v) for v in value)
                and 0 < value[0] <= value[1] <= maximum)
    shape = profile.get('shape', {})
    if not isinstance(shape, dict):
        _fail('COLLECTION_SCROLL_CALIBRATION_SHAPE')
    for key in ('card_width_range', 'card_height_range', 'fields_height_range', 'row_pitch_range'):
        if not interval(shape.get(key), max(viewport)):
            _fail('COLLECTION_SCROLL_CALIBRATION_SHAPE')
    columns = shape.get('column_left_ranges')
    if (not isinstance(columns, list) or not 1 <= len(columns) <= 4
            or any(not interval(column, viewport[0]) for column in columns)
            or any(left[1] >= right[0] for left, right in zip(columns, columns[1:]))):
        _fail('COLLECTION_SCROLL_CALIBRATION_SHAPE')
    scrollbar = profile.get('scrollbar', {})
    if not isinstance(scrollbar, dict) or not interval(scrollbar.get('thumb_height_range'), viewport[1]):
        _fail('COLLECTION_SCROLL_CALIBRATION_SCROLLBAR')
    track = _rect(scrollbar.get('track_bounds'), 'COLLECTION_SCROLL_CALIBRATION_SCROLLBAR')
    if not _contains([0, 0, *viewport], track):
        _fail('COLLECTION_SCROLL_CALIBRATION_SCROLLBAR')
    if (not interval(profile.get('content_displacement_pixels'), listing[3])
            or not interval(profile.get('thumb_displacement_pixels'), track[3])
            or not interval(profile.get('content_pixels_per_thumb_pixel'), 16384)):
        _fail('COLLECTION_SCROLL_CALIBRATION_DISTANCE')
    distance = profile['content_displacement_pixels']
    if distance[1] - distance[0] >= shape['row_pitch_range'][0] / 2:
        _fail('COLLECTION_SCROLL_CALIBRATION_AMBIGUOUS_ROWS')
    return deepcopy(profile)


def _profile_layout_shape(layout, profile):
    if layout['viewport'] != profile['viewport'] or layout['listing_viewport'] != profile['listing_viewport']:
        _fail('COLLECTION_SCROLL_CALIBRATION_VIEWPORT')
    bar, expected = layout['scrollbar'], profile['scrollbar']
    if (bar is None or not same_scrollbar_track(bar['track_bounds'], expected['track_bounds'])
            or not expected['thumb_height_range'][0] <= bar['thumb_bounds'][3] <= expected['thumb_height_range'][1]):
        _fail('COLLECTION_SCROLL_CALIBRATION_SCROLLBAR')
    shape = profile['shape']
    columns = [[] for _ in shape['column_left_ranges']]
    full_columns = [[] for _ in columns]
    # Validate the complete-card anchors first. A clipped search-region's
    # missing left side is not an observed x coordinate and must not establish
    # a column by pretending that it is one. No partial card anchors another.
    for card in layout['cards']:
        if not card['selectable']:
            continue
        x, _, width, height = card['bounds']
        matches = [index for index, bounds in enumerate(shape['column_left_ranges']) if bounds[0] <= x <= bounds[1]]
        if (len(matches) != 1 or not shape['card_width_range'][0] <= width <= shape['card_width_range'][1]
                or not shape['card_height_range'][0] <= height <= shape['card_height_range'][1]
                or not shape['fields_height_range'][0] <= card['fields_bounds'][3] <= shape['fields_height_range'][1]):
            _fail('COLLECTION_SCROLL_CALIBRATION_SHAPE')
        full_columns[matches[0]].append(card)
    for card in layout['cards']:
        x, y, width, height = card['bounds']
        if (not card['selectable'] and card.get('bounds_basis') == 'clipped_search_region'
                and not card['edges']['left'] and card['edges']['right']):
            # Use only the real right edge to match independently validated
            # complete cards from this same frame. Reuse the established 8px
            # horizontal and 98% smaller-coverage bounds, applied horizontally
            # because a clipped card cannot satisfy full-height association.
            # Its missing left edge remains missing; its search width remains
            # subject to the explicit measured profile's width interval.
            def anchored_to(full):
                a, _, w, _ = full['bounds']
                overlap = max(0, min(x+width, a+w)-max(x, a))
                return (abs(x+width-a-w) <= HORIZONTAL_GEOMETRY_TOLERANCE_PX
                        and overlap*100 >= MIN_SMALLER_CARD_COVERAGE_PERCENT*min(width, w))
            matches = [index for index, anchors in enumerate(full_columns)
                       if anchors and all(anchored_to(full) for full in anchors)]
        else:
            matches = [index for index, bounds in enumerate(shape['column_left_ranges']) if bounds[0] <= x <= bounds[1]]
        if len(matches) != 1 or not shape['card_width_range'][0] <= width <= shape['card_width_range'][1]:
            _fail('COLLECTION_SCROLL_CALIBRATION_SHAPE')
        if card['selectable']:
            if (not shape['card_height_range'][0] <= height <= shape['card_height_range'][1]
                    or not shape['fields_height_range'][0] <= card['fields_bounds'][3] <= shape['fields_height_range'][1]):
                _fail('COLLECTION_SCROLL_CALIBRATION_SHAPE')
        columns[matches[0]].append(card)
    if any(not cards for cards in columns):
        _fail('COLLECTION_SCROLL_CALIBRATION_SHAPE')
    complete_columns = []
    for cards in columns:
        complete = sorted([card for card in cards if card['selectable']], key=lambda card: card['fields_bounds'][1])
        complete_columns.append(complete)
        for first, second in zip(complete, complete[1:]):
            pitch = second['fields_bounds'][1] - first['fields_bounds'][1]
            if not shape['row_pitch_range'][0] <= pitch <= shape['row_pitch_range'][1]:
                _fail('COLLECTION_SCROLL_CALIBRATION_ROW_PITCH')
    if not complete_columns[0] or any(len(cards) != len(complete_columns[0]) for cards in complete_columns):
        _fail('COLLECTION_SCROLL_CALIBRATION_ROW_SHAPE')
    for row in zip(*complete_columns):
        tops = [card['fields_bounds'][1] for card in row]
        bottoms = [card['fields_bounds'][1] + card['fields_bounds'][3] for card in row]
        if max(tops)-min(tops) > 3 or max(bottoms)-min(bottoms) > 3:
            _fail('COLLECTION_SCROLL_CALIBRATION_ROW_SHAPE')
    return columns


def build_scroll_coverage(packet, rule, processed_cards, *, mode='collection_completed'):
    """Bind this window's processed observations to its current complete cards.

    Geometry only associates observations made while scanning this one window.
    It is never an identity cache across scrolling. A probe is explicitly a
    different mode and cannot be reported as completed collection coverage.
    """
    layout, scope = observe_layout(packet), _scope(packet, rule)
    if mode not in ('collection_completed', 'read_only_probe'):
        _fail('COLLECTION_SCROLL_COVERAGE_MODE')
    allowed = ({'favorite_confirmed', 'favorite_preserved', 'candidate_outside_rule'}
               if mode == 'collection_completed' else {'read_only_observed'})
    complete = [card for card in layout['cards'] if card['selectable']]
    if (not isinstance(processed_cards, list) or not complete
            or len(processed_cards) != len(complete)):
        _fail('COLLECTION_SCROLL_COVERAGE_INCOMPLETE')
    entries, used = [], set()
    for processed in processed_cards:
        if not isinstance(processed, dict) or processed.get('disposition') not in allowed:
            _fail('COLLECTION_SCROLL_COVERAGE_DISPOSITION')
        candidate, original = processed.get('candidate'), processed.get('card')
        _identity(candidate, 'COLLECTION_SCROLL_COVERAGE_CANDIDATE')
        if (candidate.get('product') != scope['product_name'] or candidate.get('row_index') != scope['row_index']
                or type(candidate.get('row_index')) is not int
                or candidate.get('condition') not in (scope['condition_label'],)
                   and scope['condition_label'] != '仅磨损'
                or candidate.get('geometry_mode') != 'observed_dynamic'
                or not isinstance(candidate.get('source_frame_id'), str) or not candidate['source_frame_id']
                or not _sha256(candidate.get('source_frame_sha256'))
                or not isinstance(candidate.get('price'), str) or not re.fullmatch(r'(?:0|[1-9][0-9]*)', candidate['price'])
                or type(candidate.get('eligible')) is not bool or not same_card_geometry(original, candidate)):
            _fail('COLLECTION_SCROLL_COVERAGE_CANDIDATE')
        if (processed['disposition'] == 'candidate_outside_rule' and candidate['eligible']
                or processed['disposition'] in ('favorite_confirmed', 'favorite_preserved') and not candidate['eligible']):
            _fail('COLLECTION_SCROLL_COVERAGE_DISPOSITION')
        # The pre-selection gray outline has already been bound to the
        # independently read selected candidate above. Coverage belongs to
        # that verified candidate, not to the older outline as a second
        # simultaneous reference. run05 observed gray top574 -> selected579
        # -> current580 with an unchanged lower band: both real rebindings
        # pass, but requiring old574 == current580 falsely blocks scrolling.
        # Keep the existing matcher/tolerances, unique current-card mapping,
        # and the old-to-current lower-field anchor (no cumulative list drift).
        def current_match(card):
            if not same_card_geometry(card, candidate):
                return False
            if same_card_geometry(card, original):
                return True
            a, b = _rect(original.get('bounds', original.get('card_bounds'))), _rect(card['bounds'])
            fa, fb = _rect(original.get('fields_bounds')), _rect(card['fields_bounds'])
            return (_stable_lower_band(a, b, fa, fb)
                    and all(abs(x-y) <= HORIZONTAL_GEOMETRY_TOLERANCE_PX
                            for x, y in ((a[0], b[0]), (a[0]+a[2], b[0]+b[2]),
                                         (fa[0], fb[0]), (fa[0]+fa[2], fb[0]+fb[2]))))
        matches = [card for card in complete if current_match(card)]
        if len(matches) != 1 or matches[0]['id'] in used:
            _fail('COLLECTION_SCROLL_COVERAGE_BINDING')
        current = matches[0]
        used.add(current['id'])
        entries.append(dict(card_id=current['id'], bounds=current['bounds'], fields_bounds=current['fields_bounds'],
                            source_frame_id=candidate['source_frame_id'],
                            source_frame_sha256=candidate['source_frame_sha256'], disposition=processed['disposition']))
    if used != {card['id'] for card in complete}:
        _fail('COLLECTION_SCROLL_COVERAGE_INCOMPLETE')
    return dict(schema='collection-scroll-coverage-v1', scope=scope, mode=mode,
                frame_id=layout['frame_id'], frame_sha256=layout['frame_sha256'], layout_id=layout['layout_id'],
                processed_cards=deepcopy(processed_cards), current_cards=entries,
                collection_completion_claimed=mode == 'collection_completed', cross_scroll_identity_reuse=False)


def _batch_scroll_bounds(packet, rule, layout, profile, coverage):
    columns = _profile_layout_shape(layout, profile)
    if not isinstance(coverage, dict) or coverage.get('schema') != 'collection-scroll-coverage-v1':
        _fail('COLLECTION_SCROLL_COVERAGE_REQUIRED')
    rebuilt = build_scroll_coverage(packet, rule, coverage.get('processed_cards'), mode=coverage.get('mode'))
    if rebuilt != coverage:
        _fail('COLLECTION_SCROLL_COVERAGE_EXPIRED')
    down = profile['delta'] < 0
    top, bottom = layout['listing_viewport'][1], layout['listing_viewport'][1] + layout['listing_viewport'][3]
    minimum, maximum, anchors = 0, layout['listing_viewport'][3], []
    margin = 4  # Original three-pixel vertical geometry uncertainty plus one.
    def visible_boundary(card, direction_down):
        edges = card['edges']
        if card['selectable'] or edges['top'] is not direction_down or edges['bottom'] is direction_down:
            return False
        # A clipped search rectangle is not a full card and is never clickable.
        # Its independently detected top/bottom edge is enough to constrain the
        # unscanned boundary. One real side anchors it to the measured column;
        # the other side may remain explicitly unobserved, never synthesized.
        if edges['left'] and edges['right']:
            return True
        return (card.get('bounds_basis') == 'clipped_search_region'
                and (edges['left'] or edges['right']))
    for index, cards in enumerate(columns):
        complete = [card for card in cards if card['selectable']]
        if not complete:
            _fail('COLLECTION_SCROLL_COVERAGE_INCOMPLETE')
        if down:
            last = max(complete, key=lambda card: card['bounds'][1])
            partials = [card for card in cards if visible_boundary(card, True)
                        and card['bounds'][1] > last['bounds'][1]]
            if not partials:
                _fail('COLLECTION_SCROLL_UNSCANNED_BOUNDARY_UNPROVEN')
            boundary = min(partials, key=lambda card: card['bounds'][1])
            edge = boundary['bounds'][1]
            minimum = max(minimum, last['bounds'][1] - top + margin)
            maximum = min(maximum, edge - top - margin)
        else:
            first = min(complete, key=lambda card: card['bounds'][1])
            partials = [card for card in cards if visible_boundary(card, False)
                        and card['bounds'][1] < first['bounds'][1]]
            if not partials:
                _fail('COLLECTION_SCROLL_UNSCANNED_BOUNDARY_UNPROVEN')
            boundary = max(partials, key=lambda card: card['bounds'][1] + card['bounds'][3])
            edge = boundary['bounds'][1] + boundary['bounds'][3]
            minimum = max(minimum, bottom - first['bounds'][1] - first['bounds'][3] + margin)
            maximum = min(maximum, bottom - edge - margin)
        anchors.append(dict(column_index=index, old_card_id=boundary['id'], edge_y=edge,
                            bounds=boundary['bounds'], observed_edge='top' if down else 'bottom'))
    if max(anchor['edge_y'] for anchor in anchors) - min(anchor['edge_y'] for anchor in anchors) > 3:
        _fail('COLLECTION_SCROLL_UNSCANNED_BOUNDARY_UNPROVEN')
    distance = profile['content_displacement_pixels']
    if minimum > maximum or distance[0] < minimum or distance[1] > maximum:
        _fail('COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE')
    return dict(minimum_displacement_pixels=minimum, maximum_displacement_pixels=maximum,
                unscanned_boundary_anchors=anchors, direction='down' if down else 'up',
                vertical_uncertainty_margin_px=margin, item_identity_proven=False)


def select_window_scroll_calibration(packet, rule, coverage, profiles):
    """Choose an independently measured safe displacement for this exact window.

    No distance is scaled from a different thumb size or wheel magnitude. Every
    candidate retains its source measurement and must match the observed shape.
    A previously scanned complete row must leave the selectable viewport while
    the first unscanned clipped row remains observable. The shortest safe
    *measured displacement interval* wins, not the fewest wheel notches.

    Failure carries ``candidate_rejections`` for the coordinator's event log;
    it never returns a one-notch fallback or an unmeasured profile.
    """
    if not isinstance(profiles, (list, tuple)) or not profiles or len(profiles) > 64:
        _fail('COLLECTION_SCROLL_CALIBRATION_BANK')
    frozen_profiles = [validate_scroll_calibration(profile) for profile in profiles]
    layout = observe_layout(packet)
    _scope(packet, rule)
    if not isinstance(coverage, dict) or coverage.get('schema') != 'collection-scroll-coverage-v1':
        _fail('COLLECTION_SCROLL_COVERAGE_REQUIRED')
    rebuilt = build_scroll_coverage(packet, rule, coverage.get('processed_cards'), mode=coverage.get('mode'))
    if rebuilt != coverage:
        _fail('COLLECTION_SCROLL_COVERAGE_EXPIRED')
    # Only an incompatible, otherwise valid calibration can be bypassed. A
    # damaged frame or expired coverage must never be hidden by another entry.
    incompatible = frozenset(('COLLECTION_SCROLL_CALIBRATION_VIEWPORT',
        'COLLECTION_SCROLL_CALIBRATION_SCROLLBAR', 'COLLECTION_SCROLL_CALIBRATION_SHAPE',
        'COLLECTION_SCROLL_CALIBRATION_ROW_PITCH', 'COLLECTION_SCROLL_CALIBRATION_ROW_SHAPE',
        'COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE', 'COLLECTION_SCROLL_UNSCANNED_BOUNDARY_UNPROVEN'))
    candidates, rejected = [], []
    for index, profile in enumerate(frozen_profiles):
        try:
            _batch_scroll_bounds(packet, rule, layout, profile, coverage)
        except ValueError as error:
            if str(error) not in incompatible:
                raise
            rejected.append(dict(profile_index=index, delta=profile['delta'], reason=str(error),
                calibration_sha256=_digest(profile), source=deepcopy(profile['source']),
                content_displacement_pixels=deepcopy(profile['content_displacement_pixels'])))
        else:
            candidates.append(profile)
    if not candidates:
        error = ValueError('COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE')
        error.candidate_rejections = rejected
        raise error
    def order(profile):
        lower, upper = profile['content_displacement_pixels']
        return upper, upper - lower, abs(profile['delta']), _digest(profile)
    return deepcopy(min(candidates, key=order))


def scroll_plan(packet, rule, records, *, delta=-120, calibration=None, coverage=None):
    """Plan one bounded read-only wheel step followed by mandatory observation.

    The legacy single notch carries no full-window coverage claim. A calibrated
    batch additionally requires the caller's actual per-item coverage and a
    measured distance that clears scanned complete cards without crossing the
    first unscanned clipped row. Neither path produces a page-complete flag.
    """
    layout = observe_layout(packet)
    scope = _scope(packet, rule)
    if type(delta) is not int or delta not in SCROLL_DELTAS:
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
    if delta in BATCH_SCROLL_DELTAS:
        profile = validate_scroll_calibration(calibration)
        if delta != profile['delta']:
            _fail('COLLECTION_SCROLL_CALIBRATION_DELTA')
        token['batch_bounds'] = _batch_scroll_bounds(packet, rule, layout, profile, coverage)
        token['calibration'] = profile
        token['calibration_sha256'] = _digest(profile)
        token['coverage'] = deepcopy(coverage)
    elif calibration is not None or coverage is not None:
        _fail('COLLECTION_SCROLL_CALIBRATION_DELTA')
    return dict(schema='collection-geometry-plan-v1', timeout_seconds=15, steps=[
        dict(kind='scroll_visible_list', lease=token, expected_before='skin_listings'),
        dict(kind='capture', expected_page='skin_listings', collection_observation=True,
             collection_layout=True)], page_exhausted=False, purchase_phase_enabled=False)


def validate_scroll_lease(packet, rule, records, lease):
    if not isinstance(lease, dict) or lease.get('schema') != 'collection-scroll-lease-v1':
        _fail('COLLECTION_SCROLL_LEASE')
    current = scroll_plan(packet, rule, records, delta=lease.get('delta'),
                          calibration=lease.get('calibration'), coverage=lease.get('coverage'))['steps'][0]['lease']
    if current != lease:
        _fail('COLLECTION_SCROLL_LEASE_EXPIRED')
    return deepcopy(current)


def rebind_after_scroll(packet, rule, scroll_lease):
    """Prove wheel progress and expose only the new frame's selection targets.

    Changed image hash alone is insufficient. Track/x/width stay fixed; thumb
    height may differ by at most one observed pixel, but BOTH endpoints must
    move at least two pixels in the requested direction. This neither proves
    a quantization cause nor unchanged list membership. Only the new frame's
    complete geometry/field ROIs are returned, never old card coordinates.
    An unchanged bottom thumb is NOT an exhaustion receipt.
    """
    layout = observe_layout(packet)
    scope = _scope(packet, rule)
    if (not isinstance(scroll_lease, dict)
            or scroll_lease.get('schema') != 'collection-scroll-lease-v1'
            or scroll_lease.get('scope') != scope
            or scroll_lease.get('delta') not in SCROLL_DELTAS):
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
    if not same_scrollbar_track(old['track_bounds'], after['track_bounds']):
        _fail('COLLECTION_SCROLL_TRACK_CHANGED')
    old_thumb, new_thumb = old['thumb_bounds'], after['thumb_bounds']
    height_delta = new_thumb[3] - old_thumb[3]
    if (old_thumb[0] - old['track_bounds'][0] != new_thumb[0] - after['track_bounds'][0]
            or old_thumb[2] != new_thumb[2] or abs(height_delta) > 1):
        _fail('COLLECTION_SCROLL_CONTENT_CHANGED')
    shift = new_thumb[1] - old_thumb[1]
    bottom_shift = new_thumb[1] + new_thumb[3] - old_thumb[1] - old_thumb[3]
    if abs(shift) < 2 or abs(bottom_shift) < 2:
        _fail('COLLECTION_SCROLL_PROGRESS_UNPROVEN')
    if ((shift > 0) != (scroll_lease['delta'] < 0)
            or (bottom_shift > 0) != (scroll_lease['delta'] < 0)):
        _fail('COLLECTION_SCROLL_DIRECTION_CONFLICT')
    rebound = dict(layout=layout, scrollbar_shift_pixels=shift,
                scrollbar_bottom_shift_pixels=bottom_shift, height_delta=height_delta,
                height_delta_cause='unproven', content_membership_not_proven=True,
                old_card_coordinates_reused=False,
                selectable_card_ids=[card['id'] for card in layout['cards'] if card['selectable']],
                partial_card_ids=layout['partial_card_ids'], requires_selected_reobservation=True,
                page_exhausted=False, collection_allowed=False, purchase_phase_enabled=False)
    if scroll_lease['delta'] in BATCH_SCROLL_DELTAS:
        profile = validate_scroll_calibration(scroll_lease.get('calibration'))
        if profile['delta'] != scroll_lease['delta'] or _digest(profile) != scroll_lease.get('calibration_sha256'):
            _fail('COLLECTION_SCROLL_CALIBRATION_BINDING')
        columns = _profile_layout_shape(layout, profile)
        thumb_range = profile['thumb_displacement_pixels']
        if any(not thumb_range[0] <= abs(value) <= thumb_range[1] for value in (shift, bottom_shift)):
            _fail('COLLECTION_SCROLL_BATCH_THUMB_DISTANCE')
        bounds = scroll_lease.get('batch_bounds', {})
        anchors = bounds.get('unscanned_boundary_anchors')
        if not isinstance(anchors, list) or len(anchors) != len(columns):
            _fail('COLLECTION_SCROLL_BATCH_BOUNDARY')
        lower, upper = profile['content_displacement_pixels']
        ratio = profile['content_pixels_per_thumb_pixel']
        lower = max(lower, min(abs(shift), abs(bottom_shift)) * ratio[0])
        upper = min(upper, max(abs(shift), abs(bottom_shift)) * ratio[1])
        boundary_matches = []
        for index, (anchor, cards) in enumerate(zip(anchors, columns)):
            if anchor.get('column_index') != index or type(anchor.get('edge_y')) is not int:
                _fail('COLLECTION_SCROLL_BATCH_BOUNDARY')
            down = scroll_lease['delta'] < 0
            matches = []
            for card in cards:
                if not card['selectable']:
                    continue
                edge = card['bounds'][1] if down else card['bounds'][1] + card['bounds'][3]
                displacement = anchor['edge_y'] - edge if down else edge - anchor['edge_y']
                if displacement + 3 >= lower and displacement - 3 <= upper:
                    matches.append((card, displacement))
            if len(matches) != 1:
                _fail('COLLECTION_SCROLL_BATCH_BOUNDARY')
            matched, distance = matches[0]
            lower, upper = max(lower, distance-3), min(upper, distance+3)
            boundary_matches.append(dict(column_index=index, new_card_id=matched['id'],
                                         measured_edge_displacement_pixels=distance))
        if (lower > upper or lower < bounds.get('minimum_displacement_pixels', float('inf'))
                or upper > bounds.get('maximum_displacement_pixels', -1)):
            _fail('COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE')
        rebound['batch_scroll_evidence'] = dict(schema='collection-batch-scroll-readback-v1',
            displacement_interval_pixels=[lower, upper], boundary_matches=boundary_matches,
            calibration_sha256=scroll_lease['calibration_sha256'],
            old_complete_exit_under_calibrated_motion_model=True,
            first_unscanned_boundary_visible_under_calibrated_motion_model=True,
            coverage_mode=scroll_lease['coverage']['mode'],
            item_identity_proven=False, skipped_by_old_identity=False,
            requires_selected_reobservation=True)
    return rebound
