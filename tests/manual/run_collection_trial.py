"""Collection-only live trial: current task snapshot, one foreground session.

Historical startup order is retained. A missing calibration stops explicitly;
unseen listings are never counted as searched, and no purchase action exists.
"""
from __future__ import annotations

import argparse
import copy
import datetime
from decimal import Decimal, InvalidOperation
import hashlib
import json
import os
from pathlib import Path
import time
import uuid

from collection_labels import exact_label_target, normalized
from foreground_batch_core import filter_expectation_matches, filter_matches_under_pointer, filter_readback_unsettled
from collection_season_labels import resolve_season_label

GRADE_KEYS = ('legendary', 'epic', 'rare', 'common')
FILTER_KEYS = ('owned', 'unowned') + GRADE_KEYS
CONFIRM_POINT = [1280, 1008]  # 确认 in the catalogue filter dialog
# Blank band just above 确认, clear of every box, label and page anchor (the
# page is classified from 赛季/品阶/枪种/确认 being read): the pointer rests
# here for re-reads and the final proof, then 确认 is a ~100 px move.
PARK_POINT = [1280, 905]
FILTER_REREADS = 2  # fresh reads after resting the pointer off the boxes


class FilterUnsettled(RuntimeError):
    """The filter dialog could not be read or verified; reopening it may help.
    No 确认 is ever clicked on an unverified state."""
GRADE_LABELS = dict(legendary='传说品阶', epic='史诗品阶', rare='稀有品阶', common='普通品阶')
# The skin's 品阶 from the catalogue (row['game_grade'], the game's own filter
# label) -> filter checkbox. 普通 skins are never collected (user, 2026-10-09).
GRADE_BY_LABEL = {'传说品阶': 'legendary', '史诗品阶': 'epic', '稀有品阶': 'rare'}
from collection_scroll import observe_layout, selection_plan, scroll_plan, same_card_geometry
from collection_paths import project_root

ROOT = project_root(__file__)
VIEWPORT = [2560, 1440]
CONDITION_POINTS = {'S': [2187, 464], 'A': [1861, 539], 'B': [2187, 539], 'C': [1861, 613]}
PROGRESS_SCHEMA = 'collection-rule-progress-v1'
COUNTERS = ('scanned_candidates', 'confirmed_new', 'already_favorited', 'unmatched')
BUDGET_ERRORS = frozenset(('COLLECTION_TRIAL_TIME_BUDGET', 'BATCH_DEADLINE',
    'COLLECTION_RECEIPT_TIME_BUDGET', 'COLLECTION_SESSION_STEP_LIMIT', 'COLLECTION_STEP_BUDGET',
    'COLLECTION_SEGMENT_ROLLOVER'))
STOP_REQUESTED = 'COLLECTION_STOP_REQUESTED'


def validate_scroll_measurement_evidence(profile, evidence):
    """Check the measured record's semantics, not merely its file hash.

    Boundary coordinates come from the observed before/after packets. The
    before candidate may have been read in an intermediate reference frame
    where the original clipped row became fully visible. Those frame IDs are
    retained separately; an edge phase or scrollbar ratio alone is no item
    identity measurement. This validates a record, not its external provenance.
    """
    def require(condition, suffix):
        if not condition:
            raise ValueError('COLLECTION_SCROLL_PROFILE_EVIDENCE_' + suffix)

    def digest(value):
        return (isinstance(value, str) and len(value) == 64
                and all(char in '0123456789abcdef' for char in value))

    def inside(value, interval):
        return interval[0] <= value <= interval[1]

    require(isinstance(evidence, dict), 'SCHEMA')
    require(evidence.get('schema') == 'collection-scroll-calibration-measurement-v1'
            and evidence.get('status') == 'measured', 'SCHEMA')
    require(evidence.get('measurement_method') == 'same_listing_identity_after_independent_batch', 'METHOD')
    require(evidence.get('delta') == profile['delta'], 'DELTA')
    require(evidence.get('profile_parameters') == {key: value for key, value in profile.items() if key != 'source'},
            'PARAMETERS')
    source = profile['source']
    require(source['measurement_method'] == evidence['measurement_method'], 'SOURCE')
    frames = []
    for name in ('before', 'after'):
        frame = evidence.get(name)
        require(isinstance(frame, dict), 'FRAME')
        require(isinstance(frame.get('frame_id'), str) and frame['frame_id']
                and frame['frame_id'] == source[name + '_frame_id']
                and digest(frame.get('frame_sha256')), 'FRAME')
        require(frame.get('viewport') == profile['viewport']
                and frame.get('listing_viewport') == profile['listing_viewport'], 'VIEWPORT')
        bar = frame.get('scrollbar')
        require(isinstance(bar, dict) and bar.get('track_bounds') == profile['scrollbar']['track_bounds'], 'SCROLLBAR')
        thumb = bar.get('thumb_bounds')
        track = bar['track_bounds']
        require(isinstance(thumb, list) and len(thumb) == 4
                and all(type(number) is int for number in thumb), 'SCROLLBAR')
        require(track[0] <= thumb[0] and track[1] <= thumb[1]
                and thumb[2] > 0 and thumb[3] > 0
                and thumb[0] + thumb[2] <= track[0] + track[2]
                and thumb[1] + thumb[3] <= track[1] + track[3]
                and inside(thumb[3], profile['scrollbar']['thumb_height_range']), 'SCROLLBAR')
        frames.append(frame)
    require(frames[0]['frame_id'] != frames[1]['frame_id']
            and frames[0]['frame_sha256'] != frames[1]['frame_sha256'], 'FRAME')
    old, new = (frame['scrollbar']['thumb_bounds'] for frame in frames)
    require(old[0] == new[0] and old[2] == new[2] and abs(old[3] - new[3]) <= 1, 'SCROLLBAR')
    direction = 1 if profile['delta'] < 0 else -1
    shifts = [direction * (new[1] - old[1]),
              direction * (new[1] + new[3] - old[1] - old[3])]
    require(all(inside(shift, profile['thumb_displacement_pixels']) for shift in shifts), 'THUMB_DISTANCE')
    matches = evidence.get('identity_matches')
    column_count = len(profile['shape']['column_left_ranges'])
    require(isinstance(matches, list) and len(matches) == column_count, 'IDENTITY')
    seen = set()
    for match in matches:
        require(isinstance(match, dict) and type(match.get('column_index')) is int, 'IDENTITY')
        column = match['column_index']
        require(0 <= column < column_count and column not in seen, 'IDENTITY')
        seen.add(column)
        first, second = match.get('before_candidate'), match.get('after_candidate')
        require(isinstance(first, dict) and isinstance(second, dict), 'IDENTITY')
        keys = ('product', 'condition', 'price', 'wear')
        require(all(isinstance(first.get(key), str) and first[key] and first[key] == second.get(key)
                    for key in keys), 'IDENTITY')
        try:
            values = [Decimal(first[key]) for key in ('price', 'wear')]
        except InvalidOperation:
            require(False, 'IDENTITY')
        require(all(value.is_finite() and value >= 0 for value in values), 'IDENTITY')
        require(isinstance(match.get('reference_observation_frame_id'), str)
                and match['reference_observation_frame_id']
                and digest(match.get('reference_observation_frame_sha256')), 'REFERENCE_FRAME')
        before_y, after_y = match.get('before_boundary_top_y'), match.get('after_card_top_y')
        require(type(before_y) is int and type(after_y) is int, 'DISTANCE')
        top, height = profile['listing_viewport'][1], profile['listing_viewport'][3]
        require(top <= before_y < top + height and top <= after_y < top + height, 'DISTANCE')
        distance = direction * (before_y - after_y)
        require(inside(distance, profile['content_displacement_pixels']), 'DISTANCE')
        require(all(inside(distance / shift, profile['content_pixels_per_thumb_pixel']) for shift in shifts), 'RATIO')
    return True


def load_scroll_profile(path):
    """Load an explicit measured calibration; never infer it from wheel notches."""
    from collection_scroll import validate_scroll_calibration
    path = Path(path).resolve()
    if not path.is_relative_to(ROOT / 'artifacts') or path.suffix != '.json':
        raise ValueError('COLLECTION_SCROLL_PROFILE_PATH')
    raw = path.read_bytes()
    if not 0 < len(raw) <= 512 * 1024:
        raise ValueError('COLLECTION_SCROLL_PROFILE_SIZE')
    profile = validate_scroll_calibration(json.loads(raw.decode('utf-8-sig')))
    evidence = Path(profile['source']['record_path'])
    evidence = (evidence if evidence.is_absolute() else ROOT / evidence).resolve()
    if not evidence.is_relative_to(ROOT / 'artifacts') or not evidence.is_file():
        raise ValueError('COLLECTION_SCROLL_PROFILE_EVIDENCE_PATH')
    if not 0 < evidence.stat().st_size <= 8 * 1024 * 1024:
        raise ValueError('COLLECTION_SCROLL_PROFILE_EVIDENCE_SIZE')
    evidence_bytes = evidence.read_bytes()
    measured_hash = hashlib.sha256(evidence_bytes).hexdigest()
    if measured_hash != profile['source']['record_sha256']:
        raise ValueError('COLLECTION_SCROLL_PROFILE_EVIDENCE_HASH')
    try:
        evidence_document = json.loads(evidence_bytes.decode('utf-8-sig'))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError('COLLECTION_SCROLL_PROFILE_EVIDENCE_JSON') from error
    validate_scroll_measurement_evidence(profile, evidence_document)
    return profile, dict(path=str(path), sha256=hashlib.sha256(raw).hexdigest(),
                         evidence_path=str(evidence), evidence_sha256=measured_hash,
                         evidence_hash_verified=True, evidence_semantics_verified=True)


def load_scroll_profile_bank(path):
    """Load a hash-bound bank of individually measured profiles.

    Entry paths are project-root-relative artifacts paths, not relative to the
    bank file or the caller's current directory. No missing or invalid entry is
    silently discarded and every profile still passes its own evidence loader.
    """
    path = Path(path).resolve()
    if (not path.is_relative_to(ROOT / 'artifacts') or path.suffix != '.json'
            or not path.is_file()):
        raise ValueError('COLLECTION_SCROLL_BANK_PATH')
    raw = path.read_bytes()
    if not 0 < len(raw) <= 512 * 1024:
        raise ValueError('COLLECTION_SCROLL_BANK_SIZE')
    try:
        document = json.loads(raw.decode('utf-8-sig'))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError('COLLECTION_SCROLL_BANK_JSON') from error
    if (not isinstance(document, dict) or document.get('schema') != 'collection-scroll-calibration-bank-v1'
            or not isinstance(document.get('profiles'), list) or not 1 <= len(document['profiles']) <= 64):
        raise ValueError('COLLECTION_SCROLL_BANK_SCHEMA')
    profiles, sources, seen_paths, seen_hashes = [], [], set(), set()
    for entry in document['profiles']:
        if (not isinstance(entry, dict) or not isinstance(entry.get('path'), str)
                or not entry['path'] or not _hash(entry.get('sha256'))):
            raise ValueError('COLLECTION_SCROLL_BANK_ENTRY')
        relative = Path(entry['path'])
        resolved = (ROOT / relative).resolve()
        if (relative.is_absolute() or not relative.parts or relative.parts[0] != 'artifacts'
                or '..' in relative.parts or not resolved.is_relative_to(ROOT / 'artifacts')
                or resolved.suffix != '.json' or not resolved.is_file()):
            raise ValueError('COLLECTION_SCROLL_BANK_PROFILE_PATH')
        if resolved in seen_paths or entry['sha256'] in seen_hashes:
            raise ValueError('COLLECTION_SCROLL_BANK_DUPLICATE')
        profile, source = load_scroll_profile(resolved)
        # Compare the exact bytes validated by the profile loader, avoiding a
        # second pre-validation read with a separate filesystem race window.
        if source['sha256'] != entry['sha256']:
            raise ValueError('COLLECTION_SCROLL_BANK_PROFILE_HASH')
        seen_paths.add(resolved)
        seen_hashes.add(entry['sha256'])
        profiles.append(profile)
        sources.append(dict(source, declared_sha256=entry['sha256']))
    return profiles, dict(schema='collection-scroll-bank-source-v1', path=str(path),
        sha256=hashlib.sha256(raw).hexdigest(), profile_count=len(profiles), profiles=sources,
        profile_hashes_verified=True, evidence_hashes_verified=True, evidence_semantics_verified=True)


def select_scroll_configuration(*, profile_path=None, bank_path=None, legacy=False):
    """Resolve explicit options before considering the local measured bank."""
    if sum((profile_path is not None, bank_path is not None, bool(legacy))) > 1:
        raise ValueError('COLLECTION_SCROLL_CONFIGURATION_AMBIGUOUS')
    if legacy:
        return None, None, dict(selection='explicit_legacy', legacy_single_notch=True)
    if profile_path is not None:
        profile, source = load_scroll_profile(profile_path)
        return profile, None, dict(source, selection='explicit_profile')
    if bank_path is not None:
        profiles, source = load_scroll_profile_bank(bank_path)
        return None, profiles, dict(source, selection='explicit_bank')
    default_bank = ROOT / 'artifacts' / 'collection_scroll_speed' / 'calibration_bank.json'
    if default_bank.exists():
        profiles, source = load_scroll_profile_bank(default_bank)
        return None, profiles, dict(source, selection='default_bank')
    return None, None, None


def rule_fingerprint(row):
    """Bind the full current rule, including provenance and every numeric literal."""
    return hashlib.sha256(json.dumps(row, ensure_ascii=False, sort_keys=True,
                                    separators=(',', ':'), allow_nan=False).encode('utf-8')).hexdigest()


def _hash(value):
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def progress_binding(snapshot, snapshot_sha256):
    rows = snapshot.get('rows')
    if not isinstance(rows, list) or any(not isinstance(row, dict) or type(row.get('enabled')) is not bool
            or type(row.get('row_index')) is not int or row['row_index'] < 0 for row in rows):
        raise ValueError('COLLECTION_PROGRESS_RULES')
    indices = [row['row_index'] for row in rows]
    if len(set(indices)) != len(indices):
        raise ValueError('COLLECTION_PROGRESS_DUPLICATE_ROW')
    enabled = [row for row in rows if row['enabled']]
    return dict(source_sha256=snapshot['source_sha256'], config_sha256=snapshot['config_sha256'],
        snapshot_sha256=snapshot_sha256, enabled_row_indices=[r['row_index'] for r in enabled],
        rule_fingerprints=[dict(row_index=r['row_index'], task_id=r.get('task_id'),
                               sha256=rule_fingerprint(r)) for r in enabled])


def _boundary_matches(entry, row):
    candidate = entry.get('boundary_candidate', {})
    if not isinstance(candidate, dict):
        return False
    try:
        price = Decimal(candidate.get('price', 'NaN'))
        maximum = Decimal(row['price_max'])
        wear = Decimal(candidate.get('wear', 'NaN'))
    except (InvalidOperation, TypeError, ValueError):
        return False
    return (entry.get('status') == 'original_price_stop_boundary'
        and entry.get('rule_fingerprint') == rule_fingerprint(row)
        and entry.get('row_index') == row['row_index'] and entry.get('task_id') == row.get('task_id')
        and entry.get('unobserved_tail_exhaustive') is False
        and type(entry.get('visible_windows')) is int and entry['visible_windows'] >= 1
        and price.is_finite() and maximum.is_finite() and price > maximum and wear.is_finite() and wear >= 0
        and candidate.get('product') == row['product_name'] and candidate.get('row_index') == row['row_index']
        and candidate.get('condition') == row['condition_label']
        and isinstance(candidate.get('source_frame_id'), str) and bool(candidate['source_frame_id'])
        and _hash(candidate.get('source_frame_sha256')))


def validate_resume(snapshot, snapshot_sha256, previous):
    """Only a same-snapshot, evidence-bearing contiguous enabled prefix can skip work."""
    binding = progress_binding(snapshot, snapshot_sha256)
    if (not isinstance(previous, dict) or previous.get('schema') != PROGRESS_SCHEMA
            or previous.get('phase') != 'collection_only' or previous.get('purchase_phase_started') is not False
            or previous.get('exhaustive_market_scan') is not False):
        raise ValueError('COLLECTION_RESUME_SCHEMA')
    if not _hash(snapshot_sha256) or not _hash(binding['source_sha256']) or not _hash(binding['config_sha256']):
        raise ValueError('COLLECTION_RESUME_HASH_REQUIRED')
    if any(previous.get(key) != value for key, value in binding.items()):
        raise ValueError('COLLECTION_RESUME_SNAPSHOT_OR_RULES_CHANGED')
    completed = previous.get('rows')
    enabled = [row for row in snapshot['rows'] if row['enabled']]
    if not isinstance(completed, list) or len(completed) > len(enabled):
        raise ValueError('COLLECTION_RESUME_COMPLETED_PREFIX')
    for entry, row in zip(completed, enabled):
        if not isinstance(entry, dict) or not _boundary_matches(entry, row):
            raise ValueError('COLLECTION_RESUME_BOUNDARY_EVIDENCE')
    indices = [row['row_index'] for row in enabled[:len(completed)]]
    next_index = enabled[len(completed)]['row_index'] if len(completed) < len(enabled) else None
    if previous.get('completed_row_indices') != indices or previous.get('next_row_index') != next_index:
        raise ValueError('COLLECTION_RESUME_COMPLETED_PREFIX')
    if (type(previous.get('task_file_fully_completed')) is not bool
            or (previous['task_file_fully_completed'] and len(completed) != len(enabled))):
        raise ValueError('COLLECTION_RESUME_FALSE_COMPLETION')
    if type(previous.get('segment_index')) is not int or not 1 <= previous['segment_index'] < 100000:
        raise ValueError('COLLECTION_RESUME_SEGMENT_INDEX')
    totals = previous.get('cycle_totals')
    if not isinstance(totals, dict) or set(totals) != set(COUNTERS):
        raise ValueError('COLLECTION_RESUME_COUNTERS')
    if any(type(totals[key]) is not int or totals[key] < 0 or type(previous.get(key)) is not int
            or not 0 <= previous[key] <= totals[key] for key in COUNTERS):
        raise ValueError('COLLECTION_RESUME_COUNTERS')
    return copy.deepcopy(completed), dict(totals)


def write_progress(path, document):
    """Publish each checkpoint atomically without replacing the prior run's summary."""
    path = Path(path)
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    with temporary.open('x', encoding='utf-8') as output:
        json.dump(document, output, ensure_ascii=False, indent=2, allow_nan=False)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


class CollectionTrial:
    def __init__(self, session, snapshot, snapshot_path, emit=lambda event: None, snapshot_sha256=None,
                 *, resume=None, resume_source=None, checkpoint=None, scroll_profile=None,
                 scroll_profile_source=None, scroll_profile_bank=None, grade_fallbacks=None):
        if not snapshot.get('ready') or snapshot.get('mode') != 'collect_only' or snapshot.get('purchase_phase_enabled') is not False:
            raise ValueError('COLLECTION_SNAPSHOT_MODE')
        if (snapshot.get('schema') != 'collection-run-snapshot-v1' or snapshot.get('source_kind') != 'current_config'
                or snapshot.get('source_sha256') != snapshot.get('config_sha256')):
            raise ValueError('COLLECTION_CURRENT_CONFIG_SNAPSHOT_REQUIRED')
        self.session = session
        self.snapshot = copy.deepcopy(snapshot)
        self.snapshot_path = str(snapshot_path)
        self.snapshot_sha256 = snapshot_sha256
        self.emit = emit
        self.checkpoint = checkpoint
        self.started = time.monotonic()
        self.active_season = None
        self.active_grade = None
        # Shared by every segment of one hotkey run (the runner passes one set).
        self.grade_fallbacks = grade_fallbacks if grade_fallbacks is not None else set()
        self.scroll_profile = None
        self.scroll_profile_bank = None
        self.last_collection_disposition = None
        self.last_attempt_key = None
        # Session-managed receipts (pixel / pipelined) are counted when the
        # session confirms them; window coverage waits for that confirmation.
        self._awaiting_receipts = {}
        self._receipt_events = []
        if hasattr(session, 'receipt_listener'):
            session.receipt_listener = self._receipt_confirmed
        if scroll_profile is not None and scroll_profile_bank is not None:
            raise ValueError('COLLECTION_SCROLL_PROFILE_AMBIGUOUS')
        if scroll_profile is not None:
            from collection_scroll import validate_scroll_calibration
            self.scroll_profile = copy.deepcopy(validate_scroll_calibration(scroll_profile))
            if self.scroll_profile['delta'] >= 0:
                raise ValueError('COLLECTION_SCROLL_PROFILE_DIRECTION')
        if scroll_profile_bank is not None:
            from collection_scroll import validate_scroll_calibration
            if (not isinstance(scroll_profile_bank, (list, tuple))
                    or not 1 <= len(scroll_profile_bank) <= 64):
                raise ValueError('COLLECTION_SCROLL_CALIBRATION_BANK')
            self.scroll_profile_bank = [validate_scroll_calibration(profile) for profile in scroll_profile_bank]
            if any(profile['delta'] >= 0 for profile in self.scroll_profile_bank):
                raise ValueError('COLLECTION_SCROLL_PROFILE_DIRECTION')
        binding = progress_binding(snapshot, snapshot_sha256)
        completed, self.prior_totals = ([], {key: 0 for key in COUNTERS})
        if resume is not None:
            completed, self.prior_totals = validate_resume(snapshot, snapshot_sha256, resume)
        self.summary = dict(schema=PROGRESS_SCHEMA, task_file_fully_completed=False, phase='collection_only', purchase_phase_started=False,
                            source_sha256=snapshot['source_sha256'], rows=[], scanned_candidates=0,
                            snapshot_sha256=snapshot_sha256,
                            enabled_row_indices=[r['row_index'] for r in snapshot['rows'] if r['enabled']],
                            configured_products=list(dict.fromkeys(r['product_name'] for r in snapshot['rows'] if r['enabled'])),
                            disabled_rows_skipped=snapshot.get('disabled_task_count', sum(not r['enabled'] for r in snapshot['rows'])),
                            confirmed_new=0, already_favorited=0, unmatched=0, exhaustive_market_scan=False, error=None)
        self.summary.update(binding, rows=completed, status='ready', segment_index=1 if resume is None else resume['segment_index'] + 1,
            resume_from=copy.deepcopy(resume_source), current_row_index=None, segment_finished=False,
            counter_scope='current_segment', completion_semantics='configured_rule_price_boundary',
            restart_policy='first_unfinished_enabled_rule_fresh_observation_preserve_existing_stars')
        # The task snapshot itself carries grade='any'; the run derives the 品阶.
        self.summary['catalog_filter_policy'] = dict(ownership='owned+unowned',
            grade='task_grade_else_catalogue_grade_else_all_grades',
            catalogue_grades=dict(GRADE_BY_LABEL), not_listed='retry_once_with_all_grades')
        self.summary['listing_scroll_policy'] = dict(
            mode=('calibrated_adaptive_window' if self.scroll_profile_bank is not None else
                  'calibrated_complete_window' if self.scroll_profile is not None else 'legacy_single_notch'),
            calibration_source=copy.deepcopy(scroll_profile_source),
            calibration_sha256=(hashlib.sha256(json.dumps(self.scroll_profile, ensure_ascii=False,
                sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')).hexdigest()
                if self.scroll_profile is not None else None),
            bank_calibration_sha256=([hashlib.sha256(json.dumps(profile, ensure_ascii=False,
                sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')).hexdigest()
                for profile in self.scroll_profile_bank] if self.scroll_profile_bank is not None else None),
            adaptive_selection='shortest_safe_measured_displacement_interval',
            automatic_single_notch_fallback=False,
            infer_batch_distance_from_single_notch=False, cross_window_geometry_identity_skip=False,
            catalog_and_menu_scroll_unchanged=True, list_top_navigation_unchanged=True)
        self.persist()

    def persist(self):
        completed = [entry['row_index'] for entry in self.summary['rows']]
        order = self.summary['enabled_row_indices']
        if completed != order[:len(completed)]:
            raise RuntimeError('COLLECTION_COMPLETED_PREFIX_INCONSISTENT')
        self.summary.update(completed_row_indices=completed,
            next_row_index=order[len(completed)] if len(completed) < len(order) else None,
            cycle_totals={key: self.prior_totals[key] + self.summary[key] for key in COUNTERS},
            pending_collection=copy.deepcopy(getattr(self.session, 'pending', None)),
            pending_geometry=copy.deepcopy(getattr(self.session, 'pending_geometry', None)),
            elapsed_ms=round((time.monotonic() - self.started) * 1000))
        if self.checkpoint is not None:
            self.checkpoint(copy.deepcopy(self.summary))

    def stopped(self, error):
        self.flush_receipt_events()
        self.summary.update(error=str(error), task_file_fully_completed=False,
            status=('segment_budget_exhausted' if str(error) in BUDGET_ERRORS
                    else 'stopped_by_user' if str(error) == STOP_REQUESTED else 'blocked'))
        self.event('trial_stopped', reason=str(error), business_cycle_complete=False)

    def event(self, name, **details):
        event = dict(event=name, elapsed_ms=round((time.monotonic() - self.started) * 1000),
                     timestamp=datetime.datetime.now().astimezone().isoformat(), **details)
        self.emit(event)
        self.persist()

    @property
    def observed(self):
        return self.session.previous

    def page(self):
        return self.observed.get('startup_page', {}).get('page')

    def capture(self, expected=None, **options):
        if expected == 'skin_home':
            options['ui_regions'] = True
        step = dict(kind='capture', collection_observation=True, **options)
        if expected:
            step['expected_page'] = expected
        result = self.session.perform(step)
        # A transition into an empty OR populated watchlist has two valid
        # outcomes, so no single expected-page is sent to the diagnostic.
        # Preserve bounded Unknown re-observation here without replaying input.
        if not expected:
            for _ in range(2):
                if self.page() != 'unknown':
                    break
                self.event('unknown_page_reobserve')
                result = self.session.perform(step)
        return result

    def click(self, point, page=None, **options):
        return self.session.perform(dict(kind='click', point=point, viewport=VIEWPORT,
                                         expected_before=page or self.page(), **options))

    def back(self):
        """The game's Back key (Esc 返回) instead of pointing at the button."""
        return self.session.perform(dict(kind='key', key='escape', expected_before=self.page()))

    def hover(self, point, page=None):
        return self.session.perform(dict(kind='hover', point=point, viewport=VIEWPORT,
                                         expected_before=page or self.page()))

    def click_label(self, region, label):
        return self.session.perform(dict(kind='click_label', expected_before=self.page(),
                                         viewport=VIEWPORT, region=region, label=label))

    def startup(self):
        self.event('startup_identify_current_page')
        self.capture(ui_regions=True)
        page = self.page()
        if page == 'lobby':
            self.click([678, 1402])
            self.capture('mandel', market_anchors=True)
            page = self.page()
        if page == 'mandel':
            self.click([430, 103])
            self.capture('skin_home', ui_regions=True)
            page = self.page()
        if page == 'skin_listings':
            overlay = self.observed.get('startup_page', {}).get('overlay')
            if overlay == 'listing_filter':
                # An interrupted prior segment may leave this known panel open.
                # Close it once via its observed confirm control. Every new rule
                # still reopens/rechecks its condition; this is not rule proof.
                self.click([2340, 1355], expected_overlay='listing_filter')
                self.capture('skin_listings')
                if self.observed.get('startup_page', {}).get('overlay') != 'none':
                    raise RuntimeError('COLLECTION_STARTUP_FILTER_NOT_CLOSED')
                self.event('startup_listing_filter_closed')
            elif overlay != 'none':
                raise RuntimeError('COLLECTION_STARTUP_UNEXPECTED_OVERLAY:' + str(overlay))
            self.event('startup_existing_listing_resume')
            return
        if page == 'catalog_filter':
            self.back()
            self.capture('skin_home', ui_regions=True)
            page = self.page()
        if page == 'skin_home':
            self.click([2310, 274])
            self.capture(ui_regions=True, market_anchors=True)
            page = self.page()
        if page == 'empty_watchlist':
            # This branch only returns to the catalog. It neither clears the
            # watchlist nor authorizes a listing action. Reuse the just-read
            # exact page observation; the normal click guard still checks its
            # age, foreground, viewport and overlay. A second whole-page OCR
            # added no decision and could fail on the same static empty text
            # (recorded S11 ready-stream run03). Do not claim double-confirmed
            # emptiness or carry this observation into candidate matching.
            self.event('watchlist_empty_observed',
                       frame_sha256=self.observed['frames'][-1]['sha256'],
                       confirmation_reads=1, purchase_phase_enabled=False)
        elif page == 'watchlist_listings':
            self.event('existing_favorites_preserved', purchase_phase_enabled=False)
        else:
            raise RuntimeError('COLLECTION_STARTUP_PAGE_NOT_CALIBRATED:' + str(page))
        self.back()
        self.capture('skin_home', ui_regions=True)

    def filter_for(self, row):
        season = resolve_season_label(row)
        grade = self.grade_for(row)
        if self.page() == 'skin_listings':
            self.back()
            self.capture('skin_home', ui_regions=True)
        if self.page() != 'skin_home':
            raise RuntimeError('COLLECTION_HOME_REQUIRED')
        # User 2026-10-09: tick both 已拥有 and 未拥有, and the skin's own
        # 品阶 so the catalogue lists fewer skins. The grade only narrows the
        # list: the product is still found by its OCR'd name and title, and a
        # product missing under its grade is searched again without it.
        wanted = dict(owned='checked', unowned='checked')
        wanted.update((key, 'checked' if key == grade else 'unchecked') for key in GRADE_KEYS)
        target = season.game_label
        for attempt in (1, 2):
            self.click([340, 274])
            self.capture('catalog_filter', filter_state=True)
            try:
                toggles = self._apply_filter(target, wanted)
                break
            except FilterUnsettled as error:
                if attempt == 2:
                    raise RuntimeError(str(error)) from None
                # 兜底 (user 2026-10-09): close the dialog without confirming
                # (Esc 返回) and start it over from a fresh read.
                self.event('catalog_filter_reopened', reason=str(error), season=target)
                self._close_filter()
        self.event('catalog_filter_ready', season=target, ownership='owned+unowned',
                   game_grade=grade or 'any', game_grade_label=GRADE_LABELS.get(grade, '全部品阶'),
                   game_grade_source=self.grade_source(row), toggled=toggles, **season.event_fields())
        self.active_season = normalized(target)
        self.active_grade = grade
        self.click(CONFIRM_POINT)
        self.capture('skin_home', ui_regions=True)

    def _close_filter(self):
        """Esc out of the filter dialog without 确认. One Esc may only close the
        open season dropdown, so the page is read (no expected page: a
        mismatch must not end the lease) and Esc repeated once."""
        for press in range(2):
            self.back()
            self.capture(ui_regions=True)
            if self.page() == 'skin_home':
                return
            if self.page() != 'catalog_filter':
                break
        raise RuntimeError('COLLECTION_FILTER_CLOSE_UNPROVEN:' + str(self.page()))

    def _filter_settled(self, reason):
        """Every box of the current dialog read as checked or unchecked. A box
        under the pointer's glow, or mid-animation, may read unknown: rest the
        pointer on 确认 (off every box) and read again, then give up so the
        caller can reopen the dialog."""
        for read in range(FILTER_REREADS + 1):
            state = self.observed.get('catalog_filter_state', {})
            boxes = state.get('checkboxes', {})
            if state.get('valid_page') is True and all(
                    boxes.get(key, {}).get('state') in ('checked', 'unchecked') for key in FILTER_KEYS):
                return state
            if read == FILTER_REREADS:
                break
            if read == 0:
                self.hover(PARK_POINT)
            self.capture('catalog_filter', filter_state=True)
        unknown = [key for key in FILTER_KEYS if boxes.get(key, {}).get('state') not in ('checked', 'unchecked')]
        raise FilterUnsettled(reason + ':' + ','.join(unknown))

    def _season_settled(self, target):
        """The season label read back as the chosen one (the dropdown may still
        be closing, or OCR may miss once): bounded re-reads, then reopen."""
        for read in range(FILTER_REREADS + 1):
            label = self.observed.get('catalog_filter_state', {}).get('season_label') or ''
            if normalized(label) == normalized(target):
                return
            if read == FILTER_REREADS:
                break
            if read == 0:
                self.hover(PARK_POINT)
            self.capture('catalog_filter', filter_state=True)
        raise FilterUnsettled('COLLECTION_SEASON_READBACK_MISMATCH')

    def _apply_filter(self, target, wanted):
        """Season, then the six boxes; returns the toggled keys once all six are
        proven strictly. Readbacks are judged here, not as failing session
        steps, so an unreadable or unexpected dialog can be reopened."""
        state = self._filter_settled('COLLECTION_FILTER_STATE_INCOMPLETE')
        if normalized(state['season_label']) != normalized(target):
            self.click([1066, 446])
            self.capture('catalog_filter')
            # Its own unread/unfound menu raises FilterUnsettled (one fresh
            # dialog); session errors such as a stop request pass unchanged.
            self.find_season(target)
            self.click_label('season_options', target)
            self.capture('catalog_filter', filter_state=True)
            self._season_settled(target)
            # The chosen season option sits over 已拥有: the pointer rests there.
            state = self._filter_settled('COLLECTION_FILTER_CHECK_UNKNOWN')
        expected = {key: state['checkboxes'][key]['state'] for key in wanted}
        toggles = [key for key in wanted if expected[key] != wanted[key]]
        for key in toggles:
            # The previous readback proved the other boxes, so this box still
            # differs from what is wanted and is clicked exactly once.
            x, y, width, height = self.observed['catalog_filter_state']['checkboxes'][key]['bounds']
            self.click([x + width // 2, y + height // 2])
            expected[key] = wanted[key]
            # Read back where the pointer rests: the clicked box may still glow
            # (unknown), every other box must be unchanged. The pointer goes on
            # to the next box, never back to a neutral spot (user 2026-10-09).
            for read in range(2):
                self.capture('catalog_filter', filter_state=True)
                if filter_matches_under_pointer(self.observed, expected, key):
                    break
                if not filter_readback_unsettled(self.observed, expected):
                    raise FilterUnsettled('COLLECTION_FILTER_READBACK_MISMATCH:' + key)
            else:
                raise FilterUnsettled('COLLECTION_FILTER_READBACK_UNSETTLED:' + key)
        if toggles:
            # On the way to 确认: rest just above it (off every box and anchor),
            # prove all six boxes strictly, then confirm with a short move.
            self.hover(PARK_POINT)
            for read in range(FILTER_REREADS + 1):
                self.capture('catalog_filter', filter_state=True)
                if filter_expectation_matches(self.observed, wanted):
                    break
                if not filter_readback_unsettled(self.observed, wanted):
                    raise FilterUnsettled('COLLECTION_FILTER_READBACK_MISMATCH')
            else:
                raise FilterUnsettled('COLLECTION_FILTER_READBACK_UNSETTLED')
        elif not filter_expectation_matches(self.observed, wanted):
            raise FilterUnsettled('COLLECTION_FILTER_READBACK_MISMATCH')
        return toggles

    def grade_for(self, row):
        """The game 品阶 to tick for this skin, or None (all grades)."""
        if (row.get('product_id') or row['product_name']) in self.grade_fallbacks:
            return None
        if row.get('grade') in GRADE_KEYS:
            return row['grade']
        return GRADE_BY_LABEL.get(row.get('game_grade'))

    def grade_source(self, row):
        if (row.get('product_id') or row['product_name']) in self.grade_fallbacks:
            return 'fallback_all_grades'
        if row.get('grade') in GRADE_KEYS:
            return 'task'
        return 'catalogue_grade' if row.get('game_grade') in GRADE_BY_LABEL else 'none'

    def find_season(self, target):
        # Search current menu, then downward and back upward. Scrolls are tied
        # to the freshly observed season-options ROI, never a guessed match.
        for delta in [0] + [-240] * 6 + [240] * 12:
            if exact_label_target(self.observed, 'season_options', target) is not None:
                return
            if delta:
                regions = self.observed.get('collection_observation', {}).get('regions', [])
                menu = next((r for r in regions if r.get('kind') == 'season_options'), {})
                if not menu.get('ok') or not menu.get('words'):
                    raise FilterUnsettled('COLLECTION_SEASON_MENU_UNOBSERVED')
                self.session.perform(dict(kind='scroll', expected_before='catalog_filter',
                    point=[925, 735], delta=delta, viewport=VIEWPORT))
                self.capture('catalog_filter')
        if exact_label_target(self.observed, 'season_options', target) is None:
            raise FilterUnsettled('COLLECTION_SEASON_NOT_FOUND:' + target)

    def open_product(self, row):
        season = resolve_season_label(row)
        target_season = normalized(season.game_label)
        if season.corrected:
            self.event('catalog_season_label_resolved', task_id=row.get('task_id'),
                       product_id=row.get('product_id'), product=row['product_name'],
                       **season.event_fields())
        filtered = self.active_season == target_season and self.active_grade == self.grade_for(row)
        if (self.page() == 'skin_listings' and filtered
                and exact_label_target(self.observed, 'product_title', row['product_name']) is not None):
            self.event('product_already_open', task_id=row.get('task_id'), product=row['product_name'])
            return
        if self.page() == 'skin_listings' and filtered:
            self.back()
            self.capture('skin_home', ui_regions=True)
        elif self.page() != 'skin_home' or not filtered:
            self.filter_for(row)
        try:
            self.find_product(row['product_name'])
        except RuntimeError as error:
            if (not str(error).startswith('COLLECTION_PRODUCT_NOT_FOUND_IN_OBSERVED_CATALOG')
                    or self.active_grade is None):
                raise
            # The grade came from the catalogue colour; the game lists this
            # skin under another grade. Search once more under all grades.
            self.grade_fallbacks.add(row.get('product_id') or row['product_name'])
            self.event('catalog_grade_filter_fallback', product=row['product_name'],
                       game_grade=self.active_grade, catalogue_grade=row.get('game_grade'))
            self.filter_for(row)
            self.find_product(row['product_name'])
        self.click_label('catalog_names', row['product_name'])
        self.capture('skin_home', ui_regions=True)
        if exact_label_target(self.observed, 'product_title', row['product_name']) is None:
            raise RuntimeError('COLLECTION_CATALOG_TITLE_NOT_CONFIRMED:' + row['product_name'])
        self.click([2240, 1180], require_label={'region': 'product_title', 'label': row['product_name']})
        self.capture('skin_listings')
        if exact_label_target(self.observed, 'product_title', row['product_name']) is None:
            raise RuntimeError('COLLECTION_LISTING_TITLE_NOT_CONFIRMED')
        self.event('product_title_verified', product=row['product_name'])

    def find_product(self, target):
        # The current client exposes the catalogue as a scrollable left list.
        # Keep the requested product unfinished while inspecting further views;
        # never infer a click point from its index in our local skin database.
        for delta in (-360, 360):
            seen = set()
            for _ in range(12):
                if exact_label_target(self.observed, 'catalog_names', target) is not None:
                    return
                region = next((r for r in self.observed.get('collection_observation', {}).get('regions', [])
                               if r.get('kind') == 'catalog_names'), {})
                if (self.active_grade is not None and region.get('words') == [] and not region.get('truncated')
                        and (region.get('ok') or region.get('error') == 'LOCAL_CATALOG_EMPTY')):
                    # Nothing is listed under the ticked grade: not in it.
                    raise RuntimeError('COLLECTION_PRODUCT_NOT_FOUND_IN_OBSERVED_CATALOG:' + target)
                if not region.get('ok') or region.get('truncated') or not region.get('words'):
                    raise RuntimeError('COLLECTION_CATALOG_NAMES_UNOBSERVED')
                signature = tuple((normalized(w['text']), round(w['y'] / 4)) for w in region['words'])
                if signature in seen:
                    break
                seen.add(signature)
                self.session.perform(dict(kind='scroll', expected_before='skin_home',
                    point=[350, 1100], delta=delta, viewport=VIEWPORT))
                self.capture('skin_home')
                self.event('catalog_search_view', target=target, scroll_delta=delta)
        if exact_label_target(self.observed, 'catalog_names', target) is None:
            raise RuntimeError('COLLECTION_PRODUCT_NOT_FOUND_IN_OBSERVED_CATALOG:' + target)

    def condition(self, row):
        self.session.reset_segment()
        condition = row['condition_label']
        if condition not in ('成色S', '成色A', '成色B', '成色C'):
            raise RuntimeError('COLLECTION_CONDITION_PLAN_NOT_CALIBRATED:' + condition)
        for label in ('不限公示期', '默认排序'):
            if exact_label_target(self.observed, 'listing_controls', label) is None:
                raise RuntimeError('COLLECTION_LISTING_CONTROL_NOT_CONFIRMED:' + label)
        if exact_label_target(self.observed, 'product_title', row['product_name']) is None:
            raise RuntimeError('COLLECTION_LISTING_TITLE_NOT_CONFIRMED')
        self.click([300, 263], require_label={'region': 'product_title', 'label': row['product_name']})
        self.capture()
        if self.observed.get('startup_page', {}).get('overlay') != 'listing_filter':
            raise RuntimeError('COLLECTION_CONDITION_MENU_REQUIRED')
        states = self.observed.get('collection_condition_filter', {})
        desired = condition[-1]
        wanted = {key: 'checked' if key == desired else 'unchecked' for key in ('all', 'S', 'A', 'B', 'C')}
        if any(states.get(key, {}).get('state') not in ('checked', 'unchecked') for key in wanted):
            raise RuntimeError('COLLECTION_CONDITION_STATE_UNKNOWN')
        if any(states[key]['state'] != value for key, value in wanted.items()):
            self.click(CONDITION_POINTS[desired], expected_overlay='listing_filter')
            self.capture(expected_condition_filter=wanted)
        self.click([2340, 1355], expected_overlay='listing_filter')
        self.capture('skin_listings', collection_layout=True)
        if exact_label_target(self.observed, 'product_title', row['product_name']) is None:
            raise RuntimeError('COLLECTION_LISTING_TITLE_NOT_CONFIRMED')
        for label in ('不限公示期', '默认排序'):
            if exact_label_target(self.observed, 'listing_controls', label) is None:
                raise RuntimeError('COLLECTION_LISTING_CONTROL_NOT_CONFIRMED:' + label)

    def geometry(self):
        return observe_layout(self.observed)

    def _receipt_confirmed(self, settled):
        # Called inside the session, possibly in the next selection's dispatch
        # guard: update counters now, emit/persist the event after that input.
        candidate = settled['candidate']
        self.summary['confirmed_new'] += 1
        entry = self._awaiting_receipts.pop(settled['key'], None)
        if entry is not None:
            entry['disposition'] = 'favorite_confirmed'
        self._receipt_events.append(dict(row=candidate['row_index'], card_bounds=candidate.get('card_bounds'),
            candidate=dict(candidate, receipt_status='confirmed'), receipt_frame_sha256=settled.get('frame_sha256'),
            receipt_mode=settled['mode'], receipt_layout_unchanged=settled['layout_unchanged']))

    def flush_receipt_events(self):
        while self._receipt_events:
            self.event('favorite_confirmed', **self._receipt_events.pop(0))

    def settle_receipts(self, row):
        """Confirm any in-flight receipt; reobserve when the list is unproven."""
        settle = getattr(self.session, 'settle_receipts', None)
        if settle is None:
            return
        outcome = settle()
        self.flush_receipt_events()
        if outcome is not None and (not self.observed
                or self.observed.get('collection_geometry_scope') == 'receipt_only'):
            self.event('receipt_confirmed_full_layout_reobserve', row=row['row_index'])
            self.capture('skin_listings', collection_layout=True)

    def scroll_list(self, row, delta=-120, *, calibration=None, coverage=None):
        # Same identity/status projection the session uses for its lease
        # check; full historical OCR evidence is not needed to plan a wheel.
        journal = self.session.journal
        if callable(getattr(journal, 'decision_records', None)):
            records = journal.decision_records()
        else:
            records = [json.loads(path.read_text(encoding='utf-8')) for path in journal.directory.glob('*.json')]
        options = dict(delta=delta)
        if calibration is not None or coverage is not None:
            options.update(calibration=calibration, coverage=coverage)
        plan = scroll_plan(self.observed, row, records, **options)
        for step in plan['steps']:
            self.session.perform(step)
        rebound = self.observed.get('collection_geometry_rebind', {})
        if rebound.get('passed') is not True or rebound.get('kind') != 'scroll':
            raise RuntimeError('COLLECTION_SCROLL_REBIND_REQUIRED')
        self.event('listing_scroll_observed', row=row['row_index'], delta=delta,
                   shift=rebound['evidence']['scrollbar_shift_pixels'],
                   partial_cards=self.geometry()['partial_card_ids'],
                   calibrated_window=calibration is not None)

    def scroll_window(self, row, processed_cards):
        """Advance after complete current-window coverage, then reobserve.

        The measured profile controls the allowed wheel distance and the
        geometry layer proves both coverage and no overshoot. No old card is
        carried into the next window as an already-processed item identity.
        """
        if self.scroll_profile is None and self.scroll_profile_bank is None:
            self.event('listing_scroll_calibration_not_selected', row=row['row_index'],
                       mode='legacy_single_notch', inferred_batch_distance=False)
            self.scroll_list(row)
            return
        from collection_scroll import build_scroll_coverage, select_window_scroll_calibration
        self.settle_receipts(row)
        coverage = build_scroll_coverage(self.observed, row, processed_cards)
        profile = self.scroll_profile
        if self.scroll_profile_bank is not None:
            try:
                profile = select_window_scroll_calibration(self.observed, row, coverage, self.scroll_profile_bank)
            except ValueError as error:
                self.event('listing_scroll_profile_selection_failed', row=row['row_index'], error=str(error),
                           candidate_rejections=copy.deepcopy(getattr(error, 'candidate_rejections', [])),
                           automatic_single_notch_fallback=False)
                raise
        delta = profile['delta']
        profile_digest = hashlib.sha256(json.dumps(profile, ensure_ascii=False, sort_keys=True,
                                                  separators=(',', ':'), allow_nan=False).encode('utf-8')).hexdigest()
        self.event('listing_window_coverage_proven', row=row['row_index'],
                   delta=delta, coverage=copy.deepcopy(coverage),
                   calibration_sha256=profile_digest,
                   measured_displacement_pixels=copy.deepcopy(profile.get('content_displacement_pixels')))
        self.scroll_list(row, delta=delta, calibration=profile, coverage=coverage)
        self.event('listing_scroll_window_complete', row=row['row_index'], delta=delta,
                   next_frame_id=self.observed.get('collection_observation', {}).get('frame_id'),
                   prior_positions_skipped_as_identity=False)

    def ensure_list_top(self, row):
        # A condition change may retain the previous scroll offset. A price
        # boundary is valid only after starting at an observed list beginning.
        while True:
            if self.session.remaining_seconds() < 12:
                raise RuntimeError('COLLECTION_TRIAL_TIME_BUDGET')
            bar = self.geometry()['scrollbar']
            if bar is None:
                raise RuntimeError('COLLECTION_LIST_START_UNPROVEN')
            if bar['thumb_bounds'][1] - bar['track_bounds'][1] <= 2:
                self.event('listing_top_observed', row=row['row_index'], scrollbar=bar)
                return
            self.scroll_list(row, delta=120)

    def select_card(self, row, card):
        if card['selected']:
            return
        for step in selection_plan(self.observed, row, card['id'])['steps']:
            self.session.perform(step)
            self.flush_receipt_events()

    def collect_card(self, row, card):
        self.select_card(row, card)
        step = dict(kind='collect_selected', expected_before='skin_listings',
            snapshot=self.snapshot_path, row_index=row['row_index'], viewport=VIEWPORT,
            skip_ineligible=True, skip_favorited=True, end_segment_on_price_above=True)
        if self.snapshot_sha256 is not None:
            step['snapshot_sha256'] = self.snapshot_sha256
        action = self.session.perform(step)
        candidate = action.get('candidate') or action.get('collection_attempt')
        if candidate is None:
            raise RuntimeError('COLLECTION_CANDIDATE_MISSING')
        self.summary['scanned_candidates'] += 1
        reason = action.get('reason')
        details = dict(row=row['row_index'], card_bounds=card['bounds'])
        self.last_attempt_key = action.get('collection_attempt_key')
        if action.get('collection_attempt') and action.get('receipt_mode') is not None:
            # The session owns this receipt; the listener counts it.
            if action['receipt_mode'] == 'pipelined_pixel':
                self.last_collection_disposition = 'favorite_pending'
                self.event('favorite_dispatched', **details, candidate=candidate)
            else:
                self.last_collection_disposition = 'favorite_confirmed'
                if not self.observed or self.observed.get('collection_geometry_scope') == 'receipt_only':
                    self.event('receipt_confirmed_full_layout_reobserve', row=row['row_index'])
                    self.capture('skin_listings', collection_layout=True)
        elif action.get('collection_attempt'):
            self.capture('skin_listings', collection_layout=True,
                         expect_collection_added=True, receipt_if_pending=True)
            self.summary['confirmed_new'] += 1
            self.last_collection_disposition = 'favorite_confirmed'
            self.event('favorite_confirmed', **details,
                candidate=dict(candidate, receipt_status='confirmed'),
                receipt_frame_sha256=self.observed.get('collection_observation', {}).get('frame_sha256'))
            if self.observed.get('collection_geometry_scope') == 'receipt_only':
                # The old card's local proof confirms this dispatched action,
                # not the surrounding list. Before any further input, acquire
                # an independently complete current layout; never promote the
                # receipt proof into a list or reuse its old card coordinates.
                self.event('receipt_confirmed_full_layout_reobserve', row=row['row_index'])
                self.capture('skin_listings', collection_layout=True)
        elif reason == 'already_favorited':
            self.summary['already_favorited'] += 1
            self.last_collection_disposition = 'favorite_preserved'
            self.event('favorite_preserved', **details, candidate=candidate)
        else:
            self.summary['unmatched'] += 1
            self.last_collection_disposition = 'candidate_outside_rule'
            self.event('candidate_outside_rule', **details, candidate=candidate)
        return candidate

    def scan_row(self, row):
        if row.get('limit_raw') != 0:
            raise RuntimeError('COLLECTION_NONZERO_LIMIT_REQUIRES_COUNTER')
        self.summary['current_row_index'] = row['row_index']
        self.event('rule_begin', task_id=row.get('task_id'), row=row['row_index'], product=row['product_name'],
                   condition=row['condition_label'], minimum=row['price_min'], maximum=row['price_max'], wear_max=row['max_wear'])
        self.open_product(row)
        self.condition(row)
        self.ensure_list_top(row)
        visited_cards = []
        processed_cards = []
        visible_windows = 1
        window_viewport = None
        while True:
            if self.session.remaining_seconds() < 12:
                raise RuntimeError('COLLECTION_TRIAL_TIME_BUDGET')
            self._require_step_headroom()
            layout = self.geometry()
            if window_viewport is None:
                window_viewport = layout['listing_viewport']
            cards = self._unvisited(layout, visited_cards)
            if not cards:
                layout, cards = self._reobserve_window_end(row, layout, visited_cards, window_viewport)
            if not cards:
                # Bottom-of-thumb alone does not prove that another page or a
                # clipped tail is absent. Such a page needs explicit evidence,
                # never an invented successful task-complete receipt.
                bar = layout['scrollbar']
                if bar is None:
                    raise RuntimeError('COLLECTION_LIST_END_UNPROVEN')
                track, thumb = bar['track_bounds'], bar['thumb_bounds']
                if track[1] + track[3] - thumb[1] - thumb[3] <= 2:
                    raise RuntimeError('COLLECTION_BOTTOM_REQUIRES_PAGE_END_EVIDENCE')
                self.scroll_window(row, processed_cards)
                visible_windows += 1
                visited_cards.clear()
                processed_cards.clear()
                window_viewport = None
                continue
            card = cards[0]
            candidate = self.collect_card(row, card)
            processed_cards.append(dict(card=copy.deepcopy(card), candidate=copy.deepcopy(candidate),
                                        disposition=self.last_collection_disposition))
            if self.last_collection_disposition == 'favorite_pending':
                self._awaiting_receipts[self.last_attempt_key] = processed_cards[-1]
            visited_cards.append(card)
            if candidate.get('card_bounds'):
                visited_cards.append(candidate)
            if self.session.segment_finished:
                if Decimal(candidate['price']) <= Decimal(row['price_max']):
                    raise RuntimeError('COLLECTION_PRICE_BOUNDARY_INCONSISTENT')
                completion = dict(row_index=row['row_index'], task_id=row.get('task_id'),
                    status='original_price_stop_boundary', visible_windows=visible_windows,
                    unobserved_tail_exhaustive=False, rule_fingerprint=rule_fingerprint(row),
                    boundary_candidate=copy.deepcopy(candidate), completion_segment_index=self.summary['segment_index'],
                    session_record=self.summary.get('session_record'))
                if not _boundary_matches(completion, row):
                    raise RuntimeError('COLLECTION_PRICE_BOUNDARY_EVIDENCE_INCOMPLETE')
                self.summary['rows'].append(completion)
                self.summary['current_row_index'] = None
                self.event('rule_price_boundary', row=row['row_index'], candidate=candidate)
                self.session.reset_segment()
                self.flush_receipt_events()
                return

    @staticmethod
    def _unvisited(layout, visited_cards):
        return [c for c in layout['cards'] if c['selectable']
                and not any(same_card_geometry(old, c) for old in visited_cards)]

    def _reobserve_window_end(self, row, layout, visited_cards, window_viewport):
        """Decide "this window is done" on a current complete layout (read-only).

        A pipelined receipt restores the last card's selection frame as the
        basis. Hotkey 20261009-234656 #374 was such a frame read short
        (listing viewport 548 px instead of 903, track 544 instead of 899,
        the second row clipped): the window looked done with two of its four
        cards unscanned, and coverage (correctly) refused the scroll. Settle
        the in-flight receipt first (it re-reads the list when it differs);
        if the viewport still disagrees with this window's first frame, read
        once more. No input is sent: unscanned cards are scanned, never
        skipped, and processed cards keep their geometry association.
        """
        decided = layout['frame_id']
        self.settle_receipts(row)
        current = self.geometry()
        if current['listing_viewport'] != window_viewport:
            self.event('listing_window_end_reobserve', row=row['row_index'], frame_id=current['frame_id'],
                       listing_viewport=current['listing_viewport'], window_viewport=window_viewport)
            self.capture('skin_listings', collection_layout=True)
            current = self.geometry()
        if current['frame_id'] == decided:
            return current, []
        cards = self._unvisited(current, visited_cards)
        if cards:
            self.event('listing_window_unscanned_after_reobserve', row=row['row_index'],
                       decided_frame_id=decided, frame_id=current['frame_id'],
                       card_ids=[c['id'] for c in cards])
        return current, cards

    def _require_step_headroom(self, margin=40):
        """End a lease at a card boundary rather than letting the session's
        hard step limit fire between a selection and its star (review
        2026-10-09): nothing is left pending, so the next lease can resume."""
        limit = getattr(self.session, 'max_steps', None)
        steps = getattr(self.session, 'steps', None)
        if type(limit) is int and isinstance(steps, list) and len(steps) >= limit - margin:
            raise RuntimeError('COLLECTION_STEP_BUDGET')

    def run(self, row_gate=None):
        """row_gate(row) -> False ends this segment cleanly before that row."""
        rows = [row for row in self.snapshot['rows'] if row['enabled']]
        if not rows:
            raise RuntimeError('COLLECTION_NO_ENABLED_TASKS')
        if getattr(self.session, 'pending', None) is not None or getattr(self.session, 'pending_geometry', None) is not None:
            raise RuntimeError('COLLECTION_RESUME_PENDING_RECONCILIATION')
        for path in self.session.journal.directory.glob('*.json'):
            document = json.loads(path.read_text(encoding='utf-8'))
            if document.get('status') not in ('confirmed', 'confirmed_reconciliation'):
                raise RuntimeError('COLLECTION_RESUME_PENDING_RECONCILIATION')
        remaining_rows = rows[len(self.summary['rows']):]
        self.summary['status'] = 'running'
        self.event('collection_cycle_segment_begin', remaining_row_indices=[row['row_index'] for row in remaining_rows])
        if remaining_rows:
            if self.session.remaining_seconds() < 12:
                raise RuntimeError('COLLECTION_TRIAL_TIME_BUDGET')
            self.startup()
        for row in remaining_rows:
            self.summary['current_row_index'] = row['row_index']
            self.persist()
            if self.session.remaining_seconds() < 12:
                raise RuntimeError('COLLECTION_TRIAL_TIME_BUDGET')
            if row_gate is not None and not row_gate(row):
                raise RuntimeError('COLLECTION_SEGMENT_ROLLOVER')
            self.scan_row(row)
        if [entry['row_index'] for entry in self.summary['rows']] != self.summary['enabled_row_indices']:
            raise RuntimeError('COLLECTION_CYCLE_BOUNDARIES_INCOMPLETE')
        self.summary['task_file_fully_completed'] = True
        self.summary.update(status='completed', current_row_index=None)
        self.event('collection_rule_cycle_complete', observed_tail_exhaustive=False)
        return self.summary


def build_argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=240)
    parser.add_argument('--numeric-price', action='store_true',
                        help='Opt in to same-frame numeric ink preprocessing; live validation is separate from offline tests.')
    parser.add_argument('--resume', type=Path, help='Prior summary.json; exact same snapshot bytes and rules are required.')
    scroll_options = parser.add_mutually_exclusive_group()
    scroll_options.add_argument('--scroll-profile', type=Path,
        help='Use one measured listing-scroll calibration JSON, overriding the default bank.')
    scroll_options.add_argument('--scroll-bank', type=Path,
        help='Use a hash-bound measured calibration bank; the project default bank is selected when present.')
    scroll_options.add_argument('--legacy-scroll', action='store_true',
        help='Explicitly use the original single-notch listing scroll instead of any measured bank.')
    return parser


def main():
    args = build_argument_parser().parse_args()
    snapshot_path = args.snapshot.resolve()
    output = args.output.resolve()
    if not snapshot_path.is_relative_to(ROOT / 'artifacts') or not output.is_relative_to(ROOT / 'artifacts'):
        raise ValueError('COLLECTION_TRIAL_PATH')
    if not 30 <= args.seconds <= 900:
        raise ValueError('COLLECTION_TRIAL_DURATION')
    raw_snapshot = snapshot_path.read_bytes()
    snapshot = json.loads(raw_snapshot.decode('utf-8-sig'))
    snapshot_hash = hashlib.sha256(raw_snapshot).hexdigest()
    scroll_profile, scroll_profile_bank, scroll_profile_source = select_scroll_configuration(
        profile_path=args.scroll_profile, bank_path=args.scroll_bank, legacy=args.legacy_scroll)
    resume = resume_source = None
    if args.resume is not None:
        resume_path = args.resume.resolve()
        if not resume_path.is_relative_to(ROOT / 'artifacts') or resume_path.suffix != '.json':
            raise ValueError('COLLECTION_RESUME_PATH')
        raw_resume = resume_path.read_bytes()
        if not 0 < len(raw_resume) <= 8 * 1024 * 1024:
            raise ValueError('COLLECTION_RESUME_SIZE')
        resume = json.loads(raw_resume.decode('utf-8-sig'))
        validate_resume(snapshot, snapshot_hash, resume)
        resume_source = dict(path=str(resume_path), sha256=hashlib.sha256(raw_resume).hexdigest())
    output.mkdir(exist_ok=False)
    from collection_live_session import ForegroundSession
    session = ForegroundSession(output / 'session.json', timeout_seconds=args.seconds, max_steps=1000,
                                numeric_price=args.numeric_price)
    with (output / 'events.jsonl').open('x', encoding='utf-8') as log:
        def emit(event):
            log.write(json.dumps(event, ensure_ascii=False) + '\n')
            log.flush()
        trial = CollectionTrial(session, snapshot, snapshot_path, emit, snapshot_sha256=snapshot_hash,
                                resume=resume, resume_source=resume_source,
                                scroll_profile=scroll_profile, scroll_profile_source=scroll_profile_source,
                                scroll_profile_bank=scroll_profile_bank,
                                checkpoint=lambda value: write_progress(output / 'summary.json', value))
        trial.summary.update(session_record=str(output / 'session.json'), event_log=str(output / 'events.jsonl'))
        trial.persist()
        try:
            with session:
                trial.run()
        except Exception as error:
            trial.stopped(error)
        trial.summary.update(elapsed_ms=round((time.monotonic() - trial.started) * 1000),
                             ide_restored=bool(session.report.get('ide_restored')), image_file_writes=0,
                             session_record=str(output / 'session.json'), event_log=str(output / 'events.jsonl'),
                             segment_finished=True)
        trial.persist()
        # Preview remains in memory; if needed, the caller may inspect the last
        # session preview separately. No game image file is persisted.
        print(json.dumps(trial.summary, ensure_ascii=False))
    if trial.summary['task_file_fully_completed'] and trial.summary['ide_restored']:
        return 0
    if (trial.summary['status'] == 'segment_budget_exhausted' and trial.summary['ide_restored']
            and trial.summary['pending_collection'] is None and trial.summary['pending_geometry'] is None):
        return 2
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
