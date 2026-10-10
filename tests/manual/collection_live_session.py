"""Continuous collection focus lease. Importing this module never touches Win32.

The caller owns business navigation; this class retains the calibrated legacy
input/receipt contract. One session keeps window binding, observation and
pending journal across finite steps. Images stay in memory only.
"""
import base64
import copy
import datetime
import hashlib
import io
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid

from collection_candidate import (match_selected_first_card, receipt_reference, validate_receipt_geometry,
                                  validate_pixel_receipt, pixel_receipt_record)
from collection_journal import CollectionJournal, receipt_matches
from collection_labels import exact_label_target
from collection_scroll import (observe_layout, validate_card_lease, validate_scroll_lease,
                               rebind_selected, rebind_after_scroll, collection_disposition,
                               same_card_geometry, transient_layout_failure, layouts_equivalent,
                               same_scrollbar_position)
from cursor_motion import plan_motion, execute_motion, MotionInterrupted
from foreground_batch_core import (filter_expectation_matches, filter_matches_under_pointer,
                                   filter_readback_unsettled, stabilize_capture)
from collection_paths import project_root

ROOT = project_root(__file__)
# Native capture errors that a fresh read may clear (no input is repeated).
TRANSIENT_CAPTURE_ERRORS = frozenset(('E_DXGI_ACQUIRE',))
# Native capture refusals that mean the game is no longer the front window.
WINDOW_LOST_ERRORS = frozenset(('E_WINDOW_NOT_FOREGROUND', 'E_BATCH_FOREGROUND_NOT_OWNED'))
KINDS = frozenset(('capture', 'click', 'hover', 'scroll', 'click_label', 'collect_selected',
                   'select_visible_card', 'scroll_visible_list', 'key'))
# Keyboard navigation: only the game's own back key ("Esc 返回" on the listing,
# watchlist and catalogue-filter pages). It replaces moving the pointer to the
# on-screen Back button; the page readback after it is unchanged.
KEY_CODES = {'escape': 0x1B}


def _require(condition, error):
    if not condition:
        raise RuntimeError(error)


def _finite(value):
    return type(value) in (int, float) and math.isfinite(value)


GAME_EXECUTABLE = 'deltaforceclient-win64-shipping.exe'


def find_game_windows(executable=GAME_EXECUTABLE):
    """{pid: main window} like .NET MainWindowHandle: first visible unowned
    top-level window in z-order of each process with that image name."""
    import ctypes
    from ctypes import wintypes
    user, kernel = ctypes.windll.user32, ctypes.windll.kernel32
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                  ctypes.POINTER(wintypes.DWORD)]
    user.GetWindow.restype = wintypes.HWND
    user.GetWindow.argtypes = [wintypes.HWND, ctypes.c_uint]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    names, found = {}, {}

    def image(pid):
        if pid not in names:
            names[pid] = ''
            process = kernel.OpenProcess(0x1000, False, pid)
            if process:
                try:
                    size = wintypes.DWORD(1024)
                    buffer = ctypes.create_unicode_buffer(size.value)
                    if kernel.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(size)):
                        names[pid] = buffer.value.rsplit('\\', 1)[-1].lower()
                finally:
                    kernel.CloseHandle(process)
        return names[pid]

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, unused):
        if user.IsWindowVisible(hwnd) and not user.GetWindow(hwnd, 4):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value and pid.value not in found and image(pid.value) == executable:
                found[pid.value] = int(hwnd)
        return True

    user.EnumWindows(visit, 0)
    return found


def process_integrity(pid=None):
    """Mandatory integrity RID of a process (0x2000 medium, 0x3000 high), or None.

    UIPI silently drops SendInput aimed at a higher-integrity window, so a
    hotkey run checks this before any input. Unknown is never a refusal.
    """
    import ctypes
    from ctypes import wintypes
    kernel, advapi = ctypes.windll.kernel32, ctypes.windll.advapi32
    kernel.OpenProcess.restype = kernel.GetCurrentProcess.restype = wintypes.HANDLE
    advapi.GetSidSubAuthorityCount.restype = advapi.GetSidSubAuthority.restype = ctypes.c_void_p
    advapi.GetSidSubAuthority.argtypes = [ctypes.c_void_p, wintypes.DWORD]
    advapi.GetSidSubAuthorityCount.argtypes = [ctypes.c_void_p]
    advapi.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    advapi.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                           ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    process = kernel.GetCurrentProcess() if pid is None else kernel.OpenProcess(0x1000, False, int(pid))
    if not process:
        return None
    token = wintypes.HANDLE()
    try:
        if not advapi.OpenProcessToken(process, 0x0008, ctypes.byref(token)):
            return None
        size = wintypes.DWORD()
        advapi.GetTokenInformation(token, 25, None, 0, ctypes.byref(size))
        if not size.value:
            return None
        buffer = ctypes.create_string_buffer(size.value)
        if not advapi.GetTokenInformation(token, 25, buffer, size, ctypes.byref(size)):
            return None
        sid = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_void_p))[0]
        count = ctypes.cast(advapi.GetSidSubAuthorityCount(sid), ctypes.POINTER(ctypes.c_ubyte))[0]
        return int(ctypes.cast(advapi.GetSidSubAuthority(sid, count - 1), ctypes.POINTER(wintypes.DWORD))[0])
    except (OSError, ValueError, ctypes.ArgumentError):
        return None
    finally:
        if token:
            kernel.CloseHandle(token)
        if pid is not None:
            kernel.CloseHandle(process)


def _write_bytes(path, payload):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    with temporary.open('xb') as output:
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


class AsyncJsonWriter:
    """Background fsync of session evidence; the newest bytes per path win.

    Callers serialize on their own thread, so later in-memory mutation cannot
    race a write. The collection journal (input reservation and receipts)
    stays synchronous; only step/summary evidence leaves the input path.
    flush()/close() raise a recorded write error instead of hiding it.
    """
    def __init__(self):
        self._condition = threading.Condition()
        self._pending = {}
        self._writing = False
        self._error = None
        self._closed = False
        self.writes = self.coalesced = 0
        self._thread = threading.Thread(target=self._run, name='collection-report-writer', daemon=True)
        self._thread.start()

    def submit(self, path, payload):
        with self._condition:
            _require(self._error is None, 'COLLECTION_REPORT_WRITE:' + str(self._error))
            _require(not self._closed, 'COLLECTION_REPORT_WRITER_CLOSED')
            if path in self._pending:
                self.coalesced += 1
                del self._pending[path]
            self._pending[path] = payload
            self._condition.notify_all()

    def _run(self):
        while True:
            with self._condition:
                while not self._pending and not self._closed:
                    self._condition.wait()
                if not self._pending:
                    return
                path = next(iter(self._pending))
                payload = self._pending.pop(path)
                self._writing = True
            try:
                _write_bytes(path, payload)
            except BaseException as error:
                with self._condition:
                    self._error = type(error).__name__ + ':' + str(error)
                    self._writing = False
                    self._condition.notify_all()
                return
            with self._condition:
                self._writing = False
                self.writes += 1
                self._condition.notify_all()

    def flush(self):
        with self._condition:
            while (self._pending or self._writing) and self._error is None and self._thread.is_alive():
                self._condition.wait(.5)
            _require(self._error is None, 'COLLECTION_REPORT_WRITE:' + str(self._error))
            _require(not self._pending and not self._writing, 'COLLECTION_REPORT_WRITER_STOPPED')

    def close(self):
        try:
            self.flush()
        finally:
            with self._condition:
                self._closed = True
                self._condition.notify_all()
            self._thread.join(5)

    def metadata(self):
        with self._condition:
            return dict(mode='async_latest_per_path', writes=self.writes, coalesced=self.coalesced,
                        pending=len(self._pending), error=self._error)


class PixelReceiptJob:
    """One in-flight pixel receipt request for an already dispatched favorite."""
    def __init__(self, capture, command, timeout, *, pending_key, basis, started):
        self.command, self.timeout = command, timeout
        self.pending_key, self.basis, self.started = pending_key, basis, started
        self.process = self.error = None
        self.returned_monotonic = None
        self.done = threading.Event()
        self.thread = threading.Thread(target=self._run, args=(capture,),
                                       name='collection-pixel-receipt', daemon=True)
        self.thread.start()

    def _run(self, capture):
        try:
            self.process = capture(self.command, self.timeout)
        except BaseException as error:
            self.error = error
        finally:
            self.returned_monotonic = time.monotonic()
            self.done.set()


def conservative_observation_age(result, roundtrip_ms):
    """Coordinator receive-age upper bound includes worker encoding/IPC tail.

    Request start precedes acquisition. Adding source age at capture end and
    source uncertainty to the *entire* request roundtrip overestimates, rather
    than omits, its tail. max() preserves the reported bound; 5 s is unchanged.
    """
    frames = result.get('frames', [])
    if not frames:
        return None
    frame = frames[-1]
    values = [roundtrip_ms, frame.get('source_age_ms'), frame.get('source_uncertainty_ms'),
              result.get('ocr', {}).get('frame_age_at_result_ms')]
    if any(not _finite(value) or value < 0 for value in values):
        return None
    return max(values[3], values[0] + values[1] + values[2])


def unchanged_layout_waiting(result, pending):
    """An independently captured, wholly unchanged layout may still be settling.

    This authorizes another read only, never input or reuse of old geometry.
    A moved card, changed selected state or scrollbar motion is not stasis.
    Successful selection/scroll still goes through the original rebind code.
    """
    try:
        before = pending['wait_layout']
        after = observe_layout(result)
        if (after['frame_id'] == before['frame_id']
                or any(after[key] != before[key] for key in ('viewport', 'listing_viewport'))
                or not same_scrollbar_position(before['scrollbar'], after['scrollbar'])):
            return False
        def shapes(layout):
            return [{key: value for key, value in card.items() if key != 'id'}
                    for card in layout['cards']]
        return shapes(before) == shapes(after)
    except (KeyError, TypeError, ValueError):
        return False


def _stationary_selection_readiness_waiting(result, pending):
    """Explain a read-only retry while no selected card is yet proven.

    Every observed member must uniquely match the pre-input layout. Complete
    cards keep the existing geometry tolerance; clipped members permit only
    one-pixel edge changes, or the explicit unresolved right viewport edge of
    a bottom-clipped tail. The latter is not a geometry match. This proves
    neither item identity nor selection, and never creates a lease.
    """
    try:
        if pending.get('kind') != 'selection':
            return None
        before, lease, rule = pending['wait_layout'], pending['lease'], pending['rule']
        after = observe_layout(result)
        scope = {key: rule[key] for key in ('row_index', 'product_name', 'condition_label')}
        if (lease.get('schema') != 'collection-card-lease-v1' or lease.get('scope') != scope
                or any(lease.get(key) != before[key] for key in ('frame_id', 'frame_sha256', 'viewport'))
                or after['frame_id'] == before['frame_id']
                or exact_label_target(result, 'product_title', rule['product_name']) is None
                or any(after[key] != before[key] for key in ('viewport', 'listing_viewport'))
                or not same_scrollbar_position(before['scrollbar'], after['scrollbar'])
                or any(card['selected'] for card in after['cards'])
                or result.get('collection_selected_card', {}).get('selected') is not False
                or len(before['cards']) != len(after['cards']) or not before['cards']):
            return None
        selected_packet = result['collection_selected_card']
        if (selected_packet.get('same_frame') is not True
                or any(selected_packet.get(key) != after[key] for key in ('frame_id', 'frame_sha256'))):
            return None
        original_targets = [card for card in before['cards'] if card['id'] == lease['card']['id']]
        if len(original_targets) != 1 or original_targets[0] != lease['card']:
            return None
        targets = [card for card in after['cards']
                   if card['selectable'] and same_card_geometry(lease['card'], card)]
        if len(targets) != 1:
            return None

        unsettled_tail_edges = []
        def member_matches(old, new):
            # Only an explicitly unresolved, bottom-clipped tail may gain a
            # clipped right side here; it remains non-selectable and is not a
            # geometry proof. All other masks/bases retain exact equality.
            viewport = after['listing_viewport']
            a, b = old['bounds'], new['bounds']
            if (not old['selectable'] and not new['selectable'] and not old['selected']
                    and old['fields_bounds'] is None and new['fields_bounds'] is None
                    and old.get('bounds_basis') == 'observed' and new.get('bounds_basis') == 'clipped_search_region'
                    and old['edges'] == dict(top=True, bottom=False, left=True, right=True)
                    and new['edges'] == dict(top=True, bottom=False, left=True, right=False)
                    and b[0]+b[2] == viewport[0]+viewport[2] and a[0]+a[2] < b[0]+b[2]
                    and a[1]+a[3] == b[1]+b[3] == viewport[1]+viewport[3]
                    and abs(a[0]-b[0]) <= 1 and abs(a[1]-b[1]) <= 1):
                unsettled_tail_edges.append(dict(before_card_id=old['id'], after_card_id=new['id'],
                    edge='right', measured_at_viewport_boundary=True, geometry_accepted=False))
                return True
            if (old['selectable'] != new['selectable'] or old['edges'] != new['edges']
                    or old.get('bounds_basis') != new.get('bounds_basis')):
                return False
            if old['selectable']:
                if not same_card_geometry(old, new):
                    return False
                # Field sub-ROIs must retain their measured dimensions and
                # anchors inside the lower band; no inferred replacement ROI.
                for key in ('condition_bounds', 'price_bounds'):
                    if (key in old) != (key in new):
                        return False
                    if key in old:
                        a, b = old[key], new[key]
                        fa, fb = old['fields_bounds'], new['fields_bounds']
                        anchor_a = a[0]-fa[0] if key == 'condition_bounds' else fa[0]+fa[2]-a[0]-a[2]
                        anchor_b = b[0]-fb[0] if key == 'condition_bounds' else fb[0]+fb[2]-b[0]-b[2]
                        if (a[2:] != b[2:] or a[1]-fa[1] != b[1]-fb[1] or anchor_a != anchor_b):
                            return False
                return True
            if old['fields_bounds'] is not None or new['fields_bounds'] is not None:
                return False
            def edges(card):
                x, y, width, height = card['bounds']
                return x, y, x+width, y+height
            return all(abs(a-b) <= 1 for a, b in zip(edges(old), edges(new)))

        matches = [[index for index, new in enumerate(after['cards']) if member_matches(old, new)]
                   for old in before['cards']]
        if any(len(indices) != 1 for indices in matches) or len({indices[0] for indices in matches}) != len(matches):
            return None
        return dict(scope='readiness_retry_only', reason=('clipped_tail_boundary_no_selected_card'
            if unsettled_tail_edges else 'complete_layout_no_selected_card'),
            previous_frame_id=before['frame_id'], frame_id=after['frame_id'],
            target_matching_card_id=targets[0]['id'], target_matching_card_count=1,
            layout_member_count=len(matches), unique_member_geometry_matches=not unsettled_tail_edges,
            unique_full_card_geometry_matches=True, unique_member_correspondence_for_retry=True,
            full_card_count=sum(card['selectable'] for card in after['cards']),
            partial_edge_tolerance_px=1, scrollbar_unchanged=True,
            unresolved_clipped_tail_edges=unsettled_tail_edges,
            original_selection_lease_unchanged=True, requires_original_selected_rebind=True,
            selected_observed=False, item_identity_proven=False,
            collection_allowed=False, old_card_coordinates_reused=False)
    except (KeyError, TypeError, ValueError):
        return None


def _local_price_confidence_observation(result):
    """Validate a failed price observation, never accept its price text."""
    try:
        price = result.get('local_price_ocr', {})
        title = result.get('local_title_ocr', {})
        if (result.get('capture_passed') is not False
                or result.get('local_title_error') != 'LOCAL_PRICE_CONFIDENCE'
                or price.get('error') != 'LOCAL_PRICE_CONFIDENCE' or price.get('ok') is not False
                or title.get('ok') is not True or price.get('expected_value_supplied') is not False
                or result.get('ocr_passed') is not True or result.get('page_match_passed') is not True
                or result.get('focus_activation_requests') != 0 or result.get('focus_restore_requests') != 0
                or result.get('collection_requested_card_error')):
            return None
        layout = observe_layout(result)
        for metadata in (price, title):
            if (metadata.get('source_frame_id') != layout['frame_id']
                    or metadata.get('source_frame_sha256') != layout['frame_sha256']):
                return None
        texts, scores = price.get('raw_texts'), price.get('scores')
        if (not isinstance(texts, list) or len(texts) != 1 or not isinstance(texts[0], str)
                or not texts[0] or any(ch not in '0123456789' for ch in texts[0])
                or not isinstance(scores, list) or len(scores) != 1
                or not _finite(scores[0]) or not 0 <= scores[0] < .99):
            return None
        return layout
    except (KeyError, TypeError, ValueError):
        return None


def _local_condition_unread_observation(result):
    """Validate a failed frame whose selected condition label read as no words.

    Hotkey run 2026-10-08 22:45: Windows OCR returned zero words for the
    selected card's "成色S" once in ~85 selections; price, title, page and
    layout of the same frame were proven. Nothing from this frame is
    accepted. Extra or misplaced condition words are a real mismatch, not this.
    """
    try:
        price = result.get('local_price_ocr', {})
        title = result.get('local_title_ocr', {})
        native = price.get('native_observation') or {}
        if (result.get('capture_passed') is not False
                or result.get('local_title_error') != 'LOCAL_PRICE_UNEXPECTED_NATIVE_WORDS'
                or price.get('error') != 'LOCAL_PRICE_UNEXPECTED_NATIVE_WORDS' or price.get('ok') is not False
                or title.get('ok') is not True or price.get('expected_value_supplied') is not False
                or result.get('ocr_passed') is not True or result.get('page_match_passed') is not True
                or result.get('focus_activation_requests') != 0 or result.get('focus_restore_requests') != 0
                or result.get('collection_requested_card_error') or native.get('words') != []):
            return None
        attempts = [a for a in native.get('field_attempts', []) if a.get('field') == 'condition_bounds']
        if (len(attempts) != 1 or attempts[0].get('ok') is not True or attempts[0].get('word_count') != 0
                or attempts[0].get('same_frame') is not True):
            return None
        layout = observe_layout(result)
        for metadata in (price, title):
            if (metadata.get('source_frame_id') != layout['frame_id']
                    or metadata.get('source_frame_sha256') != layout['frame_sha256']):
                return None
        if attempts[0].get('frame_id') != layout['frame_id']:
            return None
        return layout
    except (KeyError, TypeError, ValueError):
        return None


def _local_price_clipped_observation(result):
    """Validate a failed frame whose selected price crop read as clipped ink.

    F2 cycle 2026-10-09 21:2x (hotkey_runs/20261009-212123 step 312): right
    after a selection the bottom card's price crop came back
    E_NUMERIC_CLIPPED_INK once, so the local price provider never ran
    (LOCAL_PRICE_PROVIDER_NOT_SELECTED); the read right after passed. Title,
    page, condition and layout of the same frame were proven. Nothing from
    this frame is accepted.
    """
    try:
        price = result.get('local_price_ocr', {})
        title = result.get('local_title_ocr', {})
        native = price.get('native_observation') or {}
        if (result.get('capture_passed') is not False
                or result.get('local_title_error') != 'LOCAL_PRICE_PROVIDER_NOT_SELECTED'
                or price.get('error') != 'LOCAL_PRICE_PROVIDER_NOT_SELECTED' or price.get('ok') is not False
                or title.get('ok') is not True or price.get('expected_value_supplied') is not False
                or result.get('ocr_passed') is not True or result.get('page_match_passed') is not True
                or result.get('focus_activation_requests') != 0 or result.get('focus_restore_requests') != 0
                or result.get('collection_requested_card_error') or native.get('error') != 'E_NUMERIC_CLIPPED_INK'):
            return None
        attempts = [a for a in native.get('field_attempts', []) if a.get('field') == 'price_bounds']
        conditions = [a for a in native.get('field_attempts', []) if a.get('field') == 'condition_bounds']
        if (len(attempts) != 1 or attempts[0].get('error') != 'E_NUMERIC_CLIPPED_INK'
                or attempts[0].get('same_frame') is not True
                or len(conditions) != 1 or conditions[0].get('ok') is not True):
            return None
        layout = observe_layout(result)
        for metadata in (price, title):
            if (metadata.get('source_frame_id') != layout['frame_id']
                    or metadata.get('source_frame_sha256') != layout['frame_sha256']):
                return None
        if attempts[0].get('frame_id') != layout['frame_id'] or conditions[0].get('frame_id') != layout['frame_id']:
            return None
        return layout
    except (KeyError, TypeError, ValueError):
        return None


def _local_field_unread_observation(result):
    """(layout, reason) when only a selected-card field was unreadable, else (None, None)."""
    layout = _local_price_confidence_observation(result)
    if layout is not None:
        return layout, 'local_price_confidence_pending'
    layout = _local_condition_unread_observation(result)
    if layout is not None:
        return layout, 'local_condition_unread_pending'
    layout = _local_price_clipped_observation(result)
    if layout is not None:
        return layout, 'local_price_clipped_pending'
    return None, None


def _selected_card_same_frame(result):
    """The selected-card proof belongs to the very frame of the layout."""
    try:
        selected, layout = result['collection_selected_card'], result['collection_layout']
        return (selected.get('selected') is True and selected.get('same_frame') is True
                and selected.get('frame_id') == layout.get('frame_id')
                and selected.get('frame_sha256') == layout.get('frame_sha256'))
    except (KeyError, TypeError, AttributeError):
        return False


def local_field_unread_listing_waiting(result):
    """A plain listing read (rule start after the filter, refresh after a
    receipt) whose selected card's price or condition alone was unreadable:
    read a fresh frame. Review 2026-10-09: these reads feed collect_selected
    directly, so the same one-off empty read must not stop the run. Nothing
    from this frame is used; the next frame must pass every original gate.
    """
    try:
        layout, reason = _local_field_unread_observation(result)
        if layout is None or not _selected_card_same_frame(result) or result.get('collection_geometry_error'):
            return None
        return dict(scope='readiness_retry_only', reason=reason.replace('_pending', '_listing_pending'),
                    frame_id=layout['frame_id'], source_frame_sha256=layout['frame_sha256'],
                    collection_allowed=False, price_accepted=False, minimum_confidence_unchanged=.99,
                    requires_original_full_capture=True, click_retry_allowed=False)
    except (KeyError, TypeError, ValueError):
        return None


def local_price_readiness_waiting(result, pending):
    """A failed fixed-price confidence gate permits fresh reads, never a price.

    This applies only after a selection, before any star reservation. The
    current selected geometry must already satisfy the original association,
    and title/model metadata must agree with the very same frame. A later
    frame still has to pass the unchanged 0.99 provider gate and normal rebind.
    An empty condition-label read is handled the same way: fresh frames only,
    the condition must still be read and matched in a later frame.
    """
    try:
        if not pending or pending.get('kind') != 'selection':
            return None
        layout, reason = _local_field_unread_observation(result)
        if layout is None:
            return None
        rebound = rebind_selected(result, pending['rule'], pending['lease'])
        return dict(scope='readiness_retry_only', reason=reason,
                    frame_id=layout['frame_id'], source_frame_sha256=layout['frame_sha256'],
                    selected_card_id=rebound['lease']['card']['id'], collection_allowed=False,
                    price_accepted=False, minimum_confidence_unchanged=.99,
                    original_lease_retained=True, requires_original_selected_rebind=True)
    except (KeyError, TypeError, ValueError):
        return None


def local_price_refresh_waiting(result, origin):
    """Retry only the full-layout refresh after an already confirmed receipt.

    run04's first refresh read 300 at .88659; a later independent frame read
    300 at .99998. Preserve the .99 threshold and reacquire, not reuse the old
    price/geometry. No new action, history override, or receipt is authorized.
    """
    try:
        if (not isinstance(origin,dict) or origin.get('capture_passed') is not True
                or origin.get('collection_geometry_scope')!='receipt_only'
                or origin.get('collection_receipt_passed') is not True
                or origin.get('collection_receipt_geometry_passed') is not True):
            return None
        layout=_local_price_confidence_observation(result)
        if layout is None:
            return None
        previous_projection=origin['collection_observation']
        if layout['frame_id']==previous_projection['frame_id']:
            return None
        previous_title=origin['local_title_ocr']
        if (previous_title.get('ok') is not True
                or previous_title.get('source_frame_id')!=previous_projection['frame_id']
                or previous_title.get('source_frame_sha256')!=previous_projection['frame_sha256']):
            return None
        old_words=previous_title.get('words',[])
        new_words=result['local_title_ocr'].get('words',[])
        if (len(old_words)!=1 or len(new_words)!=1 or not old_words[0].get('text')
                or old_words[0]['text']!=new_words[0].get('text')):
            return None
        old,new=origin['collection_selected_card'],result['collection_selected_card']
        for selected,projection in ((old,previous_projection),(new,layout)):
            if (selected.get('selected') is not True or selected.get('same_frame') is not True
                    or any(selected.get(k)!=projection[k] for k in ('frame_id','frame_sha256'))
                    or not _finite(selected.get('favorite_warm_fraction'))
                    or not _finite(selected.get('favorite_bright_fraction'))
                    or not .05<=selected['favorite_warm_fraction']<=1
                    or not 0<=selected['favorite_bright_fraction']<=.02):
                return None
        if not same_card_geometry(old,new):
            return None
        return dict(scope='readiness_retry_only',reason='local_price_confidence_after_confirmed_receipt',
            frame_id=layout['frame_id'],source_frame_sha256=layout['frame_sha256'],
            selected_card_id=new['card_id'],collection_allowed=False,price_accepted=False,
            minimum_confidence_unchanged=.99,requires_original_full_capture=True,
            old_receipt_kept_confirmed=True,click_retry_allowed=False)
    except (KeyError, TypeError, ValueError):
        return None


def _coherent_shift_selection_readiness_waiting(result, pending):
    """Explain a read after measured stasis/dehighlight or <=2px listing shift.

    This is not a geometry rebind. The input target lease stays unchanged and
    the original rebind_selected must still succeed on a subsequent new frame.
    Three non-target full cards, in both columns and two rows, must independently
    show the exact same small vertical shift. Every clipped member is checked
    separately, including its fixed clipping boundary and visibility mask.
    """
    try:
        if pending.get('kind') != 'selection':
            return None
        before, lease, rule = pending['wait_layout'], pending['lease'], pending['rule']
        after = observe_layout(result)
        scope = {key: rule[key] for key in ('row_index', 'product_name', 'condition_label')}
        selected_packet = result.get('collection_selected_card', {})
        if (lease.get('schema') != 'collection-card-lease-v1' or lease.get('scope') != scope
                or any(lease.get(key) != before[key] for key in ('frame_id', 'frame_sha256', 'viewport'))
                or after['frame_id'] == before['frame_id']
                or exact_label_target(result, 'product_title', rule['product_name']) is None
                or any(after[key] != before[key] for key in ('viewport', 'listing_viewport'))
                or not same_scrollbar_position(before['scrollbar'], after['scrollbar'])
                or any(card['selected'] for card in after['cards'])
                or selected_packet.get('selected') is not False or selected_packet.get('same_frame') is not True
                or any(selected_packet.get(key) != after[key] for key in ('frame_id', 'frame_sha256'))
                or len(before['cards']) != len(after['cards']) or not before['cards']):
            return None
        original_targets = [card for card in before['cards'] if card['id'] == lease['card']['id']]
        if len(original_targets) != 1 or original_targets[0] != lease['card']:
            return None

        def same_subroi_anchors(old, new):
            for key in ('condition_bounds', 'price_bounds'):
                if (key in old) != (key in new):
                    return False
                if key in old:
                    a, b = old[key], new[key]
                    fa, fb = old['fields_bounds'], new['fields_bounds']
                    anchor_a = a[0]-fa[0] if key == 'condition_bounds' else fa[0]+fa[2]-a[0]-a[2]
                    anchor_b = b[0]-fb[0] if key == 'condition_bounds' else fb[0]+fb[2]-b[0]-b[2]
                    if a[2:] != b[2:] or a[1]-fa[1] != b[1]-fb[1] or anchor_a != anchor_b:
                        return False
            return True

        def member_matches(old, new, shift):
            if (old['selectable'] != new['selectable'] or old['edges'] != new['edges']
                    or old.get('bounds_basis') != new.get('bounds_basis')):
                return False
            if old['selectable']:
                # Translation is an analytical comparison only. Neither packet
                # is altered or returned as replacement actionable geometry.
                translated = copy.deepcopy(old)
                translated['bounds'][1] += shift
                translated['fields_bounds'][1] += shift
                return same_card_geometry(translated, new) and same_subroi_anchors(old, new)
            if old['fields_bounds'] is not None or new['fields_bounds'] is not None:
                return False
            a, b = old['bounds'], new['bounds']
            old_edges, new_edges = (a[0], a[1], a[0]+a[2], a[1]+a[3]), (b[0], b[1], b[0]+b[2], b[1]+b[3])
            residual_limit = 2 if old['selected'] else 1
            for index, side in enumerate(('left', 'top', 'right', 'bottom')):
                if not old['edges'][side]:
                    if old_edges[index] != new_edges[index]:
                        return False  # Actual clipping/search boundary stays fixed.
                elif side in ('top', 'bottom'):
                    if abs(new_edges[index] - old_edges[index] - shift) > residual_limit:
                        return False
                elif abs(new_edges[index] - old_edges[index]) > (2 if shift or old['selected'] else 1):
                    return False
            return True

        explanations = []
        for shift in (-2, -1, 0, 1, 2):
            # run04: the previous selection is now a clipped card. Removing
            # its white outline changes its measured edges by two pixels even
            # though all three independent full-card peers remain stationary.
            # Permit one more observation, not a selected-card proof or click.
            if shift == 0 and not any(card['selected'] and not card['selectable']
                                      for card in before['cards']):
                continue
            matches = [[index for index, new in enumerate(after['cards']) if member_matches(old, new, shift)]
                       for old in before['cards']]
            if any(len(indices) != 1 for indices in matches) or len({indices[0] for indices in matches}) != len(matches):
                continue
            paired = [(old, after['cards'][indices[0]]) for old, indices in zip(before['cards'], matches)]
            peers = [(old, new) for old, new in paired if old['selectable'] and old['id'] != lease['card']['id']]
            if (len(peers) < 3
                    or any(old['selected'] or new['bounds'][1]-old['bounds'][1] != shift
                           or new['bounds'][3] != old['bounds'][3]
                           or new['fields_bounds'][1]-old['fields_bounds'][1] != shift
                           or new['fields_bounds'][3] != old['fields_bounds'][3] for old, new in peers)):
                continue
            if (max(old['bounds'][0] for old, new in peers)-min(old['bounds'][0] for old, new in peers)
                    <= min(old['bounds'][2] for old, new in peers)/2
                    or max(old['bounds'][1] for old, new in peers)-min(old['bounds'][1] for old, new in peers)
                    <= min(old['bounds'][3] for old, new in peers)/2):
                continue
            target = next(new for old, new in paired if old['id'] == lease['card']['id'])
            explanations.append(dict(scope='readiness_retry_only', reason=(
                'dehighlighted_partial_no_selected_card' if shift == 0 else
                'coherent_small_layout_shift_no_selected_card'),
                previous_frame_id=before['frame_id'], frame_id=after['frame_id'],
                target_matching_card_id=target['id'], target_matching_card_count=1,
                layout_member_count=len(matches), full_card_count=sum(card['selectable'] for card in after['cards']),
                unique_member_geometry_matches=True, measured_common_vertical_shift_px=shift,
                independent_non_target_peer_count=len(peers), peer_rows_and_columns_independent=True,
                peer_observations=[dict(before_card_id=old['id'], after_card_id=new['id'],
                    top_shift_px=new['bounds'][1]-old['bounds'][1],
                    fields_shift_px=new['fields_bounds'][1]-old['fields_bounds'][1]) for old, new in peers],
                partial_horizontal_edge_tolerance_px=2, partial_vertical_shift_residual_px=1,
                previously_selected_partial_residual_px=2, clipping_edges_unchanged=True,
                scrollbar_unchanged=True, movement_cause='unproven', selected_observed=False,
                item_identity_proven=False, collection_allowed=False, old_card_coordinates_reused=False,
                original_selection_lease_unchanged=True, requires_original_selected_rebind=True))
        return explanations[0] if len(explanations) == 1 else None
    except (KeyError, TypeError, ValueError):
        return None


def selection_readiness_waiting(result, pending):
    """Return bounded read-only readiness evidence; never accept selection."""
    return (_stationary_selection_readiness_waiting(result, pending)
            or _coherent_shift_selection_readiness_waiting(result, pending))


def same_item_white_waiting(before, after, packet):
    """A fresh same-item white star is pending, not a failed/repeatable input."""
    selected = packet.get('collection_selected_card', {})
    return (all(before.get(key) == after.get(key)
                for key in ('product', 'condition', 'price', 'wear', 'row_index'))
            and before.get('source_frame_id') != after.get('source_frame_id')
            and same_card_geometry(before, after)
            and selected.get('favorite_warm_fraction') == 0
            and _finite(selected.get('favorite_bright_fraction'))
            and selected['favorite_bright_fraction'] >= .04)


def selection_preflight_waiting(result, pending):
    """A native negative-only observation can request a new read, never input.

    This does not classify the screen, associate an item, or promote layout
    geometry. The pending input is retained; a subsequent frame must complete
    the original full OCR, page classifier and selected-card rebind.
    """
    try:
        gate=result['collection_selection_preflight']
        projection=result['collection_observation']
        frame=result['frames'][-1]
        layout=result['collection_layout']
        before=pending['wait_layout']
        digest=projection['frame_sha256']
        frame_id=projection['frame_id']
        return bool(pending['kind']=='selection'
            and gate.get('schema')=='collection-selection-preflight-v1'
            and gate.get('scope')=='negative_readiness_gate_only'
            and gate.get('ready_for_full_ocr') is False and gate.get('ocr_deferred') is True
            and all(gate.get(k) is False for k in ('actions_enabled','page_classified','identity_proven'))
            and gate.get('requires_original_full_validation') is True
            and result.get('capture_passed') is True and result.get('ocr_passed') is False
            and result.get('recognition_performed') is False and result.get('page_match_passed') is False
            and not result.get('error') and not result.get('page_error') and not result.get('local_title_error')
            and result.get('startup_page',{}).get('page')=='unobserved'
            and result.get('startup_page',{}).get('overlay')=='unobserved'
            and result.get('ocr',{}).get('provider')=='not_invoked'
            and result.get('ocr_session',{}).get('request_count')==0
            and isinstance(frame_id,str) and frame_id and frame_id!=before['frame_id']
            and isinstance(digest,str) and len(digest)==64 and all(c in '0123456789abcdef' for c in digest)
            and digest==frame['sha256'] and digest!=before['frame_sha256']
            and all(part.get('same_frame') is True and part.get('frame_id')==frame_id
                and part.get('frame_sha256')==digest for part in (gate,projection,layout))
            and projection.get('regions')==[]
            and layout.get('schema')=='collection-layout-v1'
            and gate.get('layout_complete')==layout.get('complete')
            and type(gate.get('selected_count')) is int
            and gate['selected_count']==sum(c.get('selected') is True for c in layout.get('cards',[])))
    except (KeyError,TypeError,IndexError):
        return False


class WindowsBackend:
    """Instantiated explicitly for live work; tests inject a pure backend."""
    # 'ide': agent-run batches return to the single Mirasim IDE window.
    # 'entry_foreground': a hotkey run returns to whatever root window was in
    # front when it started; when that was the game itself, it stays there.
    RETURN_POLICIES = ('ide', 'entry_foreground')

    def __init__(self, root, *, persistent_capture=False, return_policy='ide'):
        import ctypes as c
        from ctypes import wintypes as w
        import threading
        from navigate_lobby_to_warehouse import u, activate, Input
        _require(return_policy in self.RETURN_POLICIES, 'COLLECTION_RETURN_POLICY')
        self.return_policy = return_policy
        self.stay_in_target = False
        self.c, self.w, self.threading = c, w, threading
        self.u, self.activate, self.Input = u, activate, Input
        self.root = Path(root)
        self.exe = self.root / 'dist/RelinkStudio/RelinkStudio.exe'
        self.identities = {}
        self.transitions = []
        self.last_motion = None
        self.last_dispatch = None
        self.motion_history = []
        self.cursor_restore_error = None
        _require(type(persistent_capture) is bool, 'COLLECTION_CAPTURE_WORKER_OPTION')
        self.capture_worker = None
        self.capture_worker_close_error = None
        if persistent_capture:
            from capture_worker_client import CaptureWorkerClient
            self.capture_worker = CaptureWorkerClient(self.exe)
        self.dpi = self.pointer = self.watcher = None
        self.halt = threading.Event()
        self._metadata = dict(binary_sha256=hashlib.sha256(self.exe.read_bytes()).hexdigest(),
            ocr_helper_sha256=hashlib.sha256((self.exe.parent / 'vision/windows_ocr_worker.ps1').read_bytes()).hexdigest())

    def _sample(self):
        window = self.u.GetForegroundWindow()
        if not self.transitions or self.transitions[-1]['hwnd'] != window:
            self.transitions.append(dict(hwnd=window, monotonic_ms=round(time.monotonic() * 1000)))

    def _bind_entry_foreground(self, entry_foreground):
        """Hotkey policy: the game identity alone, return to the entry root."""
        games = find_game_windows()
        _require(len(games) == 1, 'COLLECTION_WINDOW_IDENTITY')
        (pid, hwnd), = games.items()
        self.identities = dict(target_hwnd=int(hwnd), target_pid=int(pid))
        game, runner = process_integrity(self.identities['target_pid']), process_integrity()
        self._metadata['integrity_levels'] = dict(game=game, runner=runner)
        _require(game is None or runner is None or game <= runner, 'COLLECTION_GAME_ELEVATED')
        root = self.u.GetAncestor(entry_foreground, 3) if entry_foreground else 0
        root_pid = self.w.DWORD()
        rect = self.w.RECT()
        if root:
            self.u.GetWindowThreadProcessId(root, self.c.byref(root_pid))
        if (not root or root == self.identities['target_hwnd'] or not root_pid.value
                or not self.u.GetClientRect(root, self.c.byref(rect))
                or rect.right <= rect.left or rect.bottom <= rect.top):
            # Started from inside the game (or from no usable window): the
            # lease ends where it began, with no activation and no cursor move.
            self.identities.update(return_hwnd=self.identities['target_hwnd'],
                                   return_pid=self.identities['target_pid'])
            self.stay_in_target = True
        else:
            # A cycle started inside the game whose game was switched away meanwhile
            # (review wf_3215e495-f2d): no activation, the user has taken over.
            _require(not getattr(self, 'require_target_in_front', False), 'BATCH_FOREGROUND_LOST')
            self.identities.update(return_hwnd=int(root), return_pid=int(root_pid.value))
        self._metadata.update(return_policy=self.return_policy, stay_in_target=self.stay_in_target,
                              return_window_binding='entry_foreground_root_owner',
                              return_window_identity_verified_before_activation=True)

    def enter(self):
        entry_foreground = self.u.GetForegroundWindow()
        if getattr(self, 'return_policy', 'ide') == 'entry_foreground':
            self._bind_entry_foreground(entry_foreground)
        else:
            self._bind_ide(entry_foreground)
        self.dpi = self.u.SetThreadDpiAwarenessContext(self.c.c_void_p(-4))
        _require(bool(self.dpi), 'COLLECTION_DPI_CONTEXT')
        self.pointer = self.w.POINT()
        _require(self.u.GetCursorPos(self.c.byref(self.pointer)), 'COLLECTION_CURSOR')
        def monitor():
            while not self.halt.is_set():
                self._sample()
                self.halt.wait(.01)
        self.watcher = self.threading.Thread(target=monitor, daemon=True)
        self.watcher.start()
        self.activate(self.identities['target_hwnd'], self.identities['target_pid'])
        # Activation can leave fullscreen/cursor ownership in transition.
        # ForegroundSession owns the settle interval and only then requests
        # the guarded neutral motion. Never move here or retry interference.

    def _bind_ide(self, entry_foreground):
        # .NET MainWindowHandle can briefly name an IDE popup that disappears
        # between enumeration and binding. Prefer the actual foreground IDE's
        # root owner, with its PID independently verified after enumeration.
        query = r"""$ErrorActionPreference='Stop';
        $g=@(Get-Process DeltaForceClient-Win64-Shipping|Where-Object {$_.MainWindowHandle -ne 0});
        $i=@(Get-Process Mirasim|Where-Object {$_.MainWindowHandle -ne 0});
        if($g.Count -ne 1 -or $i.Count -ne 1){throw 'Window identity ambiguous'};
        @{target_hwnd=[string]$g[0].MainWindowHandle;target_pid=[string]$g[0].Id;
          return_hwnd=[string]$i[0].MainWindowHandle;return_pid=[string]$i[0].Id}|ConvertTo-Json -Compress
        """
        powershell = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
        found = subprocess.run([str(powershell), '-NoProfile', '-NonInteractive', '-Command', query],
            capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
        _require(found.returncode == 0, 'COLLECTION_WINDOW_IDENTITY')
        self.identities = {key: int(value) for key, value in json.loads(found.stdout).items()}
        foreground_root = self.u.GetAncestor(entry_foreground, 3)
        root_pid = self.w.DWORD()
        self.u.GetWindowThreadProcessId(foreground_root, self.c.byref(root_pid))
        if foreground_root and root_pid.value == self.identities['return_pid']:
            rect = self.w.RECT()
            if self.u.GetClientRect(foreground_root, self.c.byref(rect)) and rect.right>rect.left and rect.bottom>rect.top:
                self.identities['return_hwnd'] = int(foreground_root)
                self._metadata['return_window_binding'] = 'entry_foreground_root_owner_same_pid'
        # Validate the final choice even when the foreground was not the IDE
        # and the enumerated MainWindowHandle remains the fallback. A vanished
        # popup must fail before taking foreground ownership from the user.
        return_pid = self.w.DWORD()
        return_rect = self.w.RECT()
        return_hwnd = self.identities['return_hwnd']
        self.u.GetWindowThreadProcessId(return_hwnd, self.c.byref(return_pid))
        _require(return_hwnd and return_pid.value == self.identities['return_pid']
                 and self.u.GetClientRect(return_hwnd, self.c.byref(return_rect))
                 and return_rect.right > return_rect.left and return_rect.bottom > return_rect.top,
                 'COLLECTION_RETURN_WINDOW_IDENTITY')
        self._metadata['return_window_identity_verified_before_activation'] = True

    def prepare(self):
        """Before any foreground lease: start the capture service so its first
        request does not pay process start and OCR warm-up. No capture, no input."""
        if self.capture_worker is not None:
            self.capture_worker.start()
        return self

    def healthy(self):
        """A prepared backend whose capture service is still usable."""
        return self.capture_worker is None or self.capture_worker.alive()

    def discard(self):
        """Release a prepared backend that never entered a lease."""
        if self.capture_worker is not None:
            self.capture_worker.close()

    def foreground(self):
        return self.u.GetForegroundWindow() == self.identities.get('target_hwnd')

    def viewport(self):
        rect = self.w.RECT()
        _require(self.u.GetClientRect(self.identities['target_hwnd'], self.c.byref(rect)), 'COLLECTION_CLIENT_RECT')
        return rect.right, rect.bottom

    def _cursor(self):
        point = self.w.POINT()
        _require(self.u.GetCursorPos(self.c.byref(point)), 'COLLECTION_CURSOR')
        return point.x, point.y

    def _idle_input(self):
        return not any(self.u.GetAsyncKeyState(key) & 0x8000 for key in [1, 2, 16, 17, 18])

    def _move_screen(self, point, foreground_hwnd, check, purpose, *, avoid_bounds=None):
        start = self._cursor()
        left, top = self.u.GetSystemMetrics(76), self.u.GetSystemMetrics(77)
        width, height = self.u.GetSystemMetrics(78), self.u.GetSystemMetrics(79)
        bounds = (left, top, left + width - 1, top + height - 1) if width > 0 and height > 0 else None
        profile=(getattr(self,'collection_motion_profile','collection_fast')
                 if purpose in ('collection_click','collection_scroll') and getattr(self,'fast_collection_motion',False)
                 else 'standard')
        route_reason='not_requested'
        if avoid_bounds is not None:
            from collection_motion_route import inside
            # Existing navigation may leave the pointer in the panel. Do not
            # invent an already-clear route or retry a click: the original
            # checked movement and mandatory fresh readback remain available.
            route_reason='current_start_and_target_outside_panel'
            if inside(start,avoid_bounds) or inside(point,avoid_bounds):
                avoid_bounds=None;route_reason='endpoint_inside_panel_original_checked_motion'
        plan = plan_motion(start, tuple(point), bounds=bounds,profile=profile,avoid_bounds=avoid_bounds)
        try:
            self.last_motion = execute_motion(plan, get_pos=self._cursor,
                set_pos=lambda x, y: bool(self.u.SetCursorPos(x, y)),
                foreground=lambda: self.u.GetForegroundWindow() == foreground_hwnd,
                check=check)
        except MotionInterrupted as error:
            self.last_motion = dict(error.metadata, purpose=purpose)
            self.motion_history.append(self.last_motion)
            raise
        self.last_motion['purpose'] = purpose
        self.last_motion['hover_route_decision'] = route_reason
        self.motion_history.append(self.last_motion)
        return self.last_motion

    def _position(self, point, purpose='input'):
        _require(self.foreground(), 'BATCH_FOREGROUND_LOST')
        origin = self.w.POINT()
        _require(self.u.ClientToScreen(self.identities['target_hwnd'], self.c.byref(origin)), 'COLLECTION_CLIENT_ORIGIN')
        screen = self.w.POINT(origin.x + point[0], origin.y + point[1])
        width, height = self.viewport()
        _require(self.u.GetAncestor(self.u.WindowFromPoint(screen), 2) == self.identities['target_hwnd'], 'COLLECTION_TARGET_OCCLUDED')
        _require(self._idle_input(), 'COLLECTION_USER_INPUT_ACTIVE')
        def check():
            current = self.w.POINT()
            return (self._idle_input() and bool(self.u.ClientToScreen(self.identities['target_hwnd'], self.c.byref(current)))
                and (current.x, current.y) == (origin.x, origin.y) and self.viewport() == (width, height)
                and self.u.GetAncestor(self.u.WindowFromPoint(screen), 2) == self.identities['target_hwnd'])
        avoid_bounds = None
        if (purpose in ('collection_click','collection_scroll') and (width,height)==(2560,1440)
                and getattr(self,'fast_collection_motion',False)
                and getattr(self,'collection_motion_profile','collection_fast')=='collection_continuous'):
            # run07 contains real condition-help tooltip text and a 57/97px
            # occluded scrollbar after the old curve crossed the detail pane.
            # Route pointer-only movement outside that panel; never alter the
            # real target, detection thresholds, input checks or receipt gate.
            avoid_bounds=(origin.x+1900,origin.y+350,origin.x+2449,origin.y+1295)
        self._move_screen((screen.x, screen.y), self.identities['target_hwnd'], check, purpose,
                          avoid_bounds=avoid_bounds)
        _require(check() and self.foreground(), 'COLLECTION_TARGET_CHANGED_DURING_MOTION')
        _require(self._cursor() == (screen.x, screen.y), 'COLLECTION_CURSOR_ENDPOINT')
        self._planned_target = ((screen.x, screen.y), check)

    def _require_on_planned_target(self):
        """After a dispatch guard (receipt settle, layout recheck: up to ~1 s of
        captures), the pointer must still rest exactly on the planned target
        and the window/occlusion checks must still hold. A nudged mouse must
        stop the input, never send it wherever the pointer now is."""
        planned = getattr(self, '_planned_target', None)
        if planned is None:
            return
        point, check = planned
        _require(self._cursor() == point, 'COLLECTION_CURSOR_MOVED_BEFORE_INPUT')
        _require(check() and self.foreground(), 'COLLECTION_TARGET_CHANGED_BEFORE_INPUT')

    def neutral_pointer(self):
        width, height = self.viewport()
        self._position([width // 2, height // 3], purpose='neutral_pointer')

    def capture(self, command, timeout):
        if self.capture_worker is not None:
            return self.capture_worker.capture(command, timeout)
        return subprocess.run(command, capture_output=True, timeout=timeout,
                              creationflags=subprocess.CREATE_NO_WINDOW)

    def click(self, point, before_dispatch=None):
        return self._click(point,before_dispatch,False)

    def fast_collection_click(self,point,before_dispatch=None):
        return self._click(point,before_dispatch,True)

    def fast_selection_click(self,point,before_dispatch=None):
        # The run01/02/03 new-frame reads observed an unchanged selection after
        # a successful two-event API call at different in-card locations.
        # Give this ONE selection press a distinct down/up interval. This is
        # not a click retry or a claim about the game's input implementation.
        # Favorite toggles and ordinary navigation retain their original path.
        return self._click(point,before_dispatch,True,selection_hold=True)

    def _click(self,point,before_dispatch,fast,selection_hold=False):
        self.last_dispatch = None
        self._planned_target = None
        self._position(point, purpose='collection_click' if fast else 'click')
        _require(self.foreground(), 'BATCH_FOREGROUND_LOST')
        if before_dispatch is not None:
            before_dispatch()
            self._require_on_planned_target()
        _require(self.foreground() and self._idle_input(), 'COLLECTION_INPUT_GUARD_CHANGED')
        pair = (self.Input * 2)()
        pair[0].data.mi.dwFlags = 2
        pair[1].data.mi.dwFlags = 4
        if selection_hold:
            return self._selection_press(pair)
        dispatch_start = time.monotonic() * 1000
        # QPC (as the capture clock's source_mono_ms) around the API call:
        # time.monotonic steps 15.6 ms on this machine.
        dispatch_qpc = time.perf_counter() * 1000
        sent = self.u.SendInput(2, pair, self.c.sizeof(self.Input))
        returned_qpc = time.perf_counter() * 1000
        self.last_dispatch = dict(api='SendInput', kind='click',
            started_mono_ms=dispatch_start, returned_mono_ms=time.monotonic()*1000,
            started_qpc_ms=dispatch_qpc, returned_qpc_ms=returned_qpc,
            expected_events=2, returned_events=sent, clock='python_monotonic',
            semantics='API_call_boundary_not_hardware_delivery_timestamp')
        if sent == 1:
            release = self.Input()
            release.data.mi.dwFlags = 4
            self.u.SendInput(1, self.c.byref(release), self.c.sizeof(self.Input))
        return sent

    def _selection_press(self,pair):
        """One bounded press, always released; never repeats a mouse-down."""
        planned=getattr(self,'_planned_target',None)
        endpoint=planned[0] if planned is not None else self._cursor()
        _require(self._cursor()==endpoint,'COLLECTION_CURSOR_MOVED_BEFORE_INPUT')
        started=time.monotonic()*1000
        down=up=cleanup=0
        held_ms=0
        error=None
        try:
            down=self.u.SendInput(1,self.c.byref(pair[0]),self.c.sizeof(self.Input))
            if down==1:
                hold_started=time.monotonic()*1000
                time.sleep(.024)
                held_ms=time.monotonic()*1000-hold_started
                _require(self.foreground(),'BATCH_FOREGROUND_LOST')
                _require(self._cursor()==endpoint,'COLLECTION_CURSOR_ENDPOINT')
        except BaseException as caught:
            error=caught
        finally:
            # A lost foreground, interrupted wait or failed check must not
            # leave our own button held. Release is cleanup, not another click.
            if down==1:
                try:
                    up=self.u.SendInput(1,self.c.byref(pair[1]),self.c.sizeof(self.Input))
                except BaseException as caught:
                    error=error or caught
                if up!=1:
                    # Preserve the original incomplete dispatch count even if
                    # this release-only cleanup succeeds; the action must stop.
                    try:
                        cleanup=self.u.SendInput(1,self.c.byref(pair[1]),self.c.sizeof(self.Input))
                    except BaseException as caught:
                        error=error or caught
            self.last_dispatch=dict(api='SendInput',kind='click',
                started_mono_ms=started,returned_mono_ms=time.monotonic()*1000,
                expected_events=2,returned_events=down+up,clock='python_monotonic',
                semantics='API_call_boundary_not_hardware_delivery_timestamp',
                delivery_policy='single_selection_press_separate_release_v1',
                press_events=down,release_events=up,release_cleanup_events=cleanup,requested_hold_ms=24,
                observed_hold_ms=held_ms,mouse_down_calls=1,
                error=str(error) if error is not None else None)
        if error is not None:
            raise error
        return down+up

    def scroll(self, point, delta, before_dispatch=None):
        return self._scroll(point, delta, before_dispatch, 'scroll')

    def fast_collection_scroll(self, point, delta, before_dispatch=None):
        # Listing wheel only: the collection pointer profile and detail-panel
        # detour; the wheel delta, lease and stability readback are unchanged.
        return self._scroll(point, delta, before_dispatch, 'collection_scroll')

    def _scroll(self, point, delta, before_dispatch, purpose):
        self._planned_target = None
        self._position(point, purpose=purpose)
        wheel = self.Input()
        wheel.data.mi.dwFlags = 0x800
        wheel.data.mi.mouseData = delta & 0xffffffff
        _require(self.foreground(), 'BATCH_FOREGROUND_LOST')
        if before_dispatch is not None:
            before_dispatch()
            self._require_on_planned_target()
        _require(self.foreground() and self._idle_input(), 'COLLECTION_INPUT_GUARD_CHANGED')
        return self.u.SendInput(1, self.c.byref(wheel), self.c.sizeof(self.Input))

    def hover(self, point):
        self._position(point, purpose='hover')

    def _keyboard(self):
        """INPUT_KEYBOARD records sized like the mouse INPUT, and SendInput."""
        if getattr(self, '_keyboard_api', None) is None:
            c, w = self.c, self.w
            class KeyInput(c.Structure):
                _fields_ = [('wVk', w.WORD), ('wScan', w.WORD), ('dwFlags', w.DWORD), ('time', w.DWORD),
                            ('dwExtraInfo', c.c_size_t)]
            class Data(c.Union):
                _fields_ = [('ki', KeyInput), ('mi', type(self.Input().data.mi))]
            class KeyboardInput(c.Structure):
                _fields_ = [('type', w.DWORD), ('data', Data)]
            user32 = c.WinDLL('user32', use_last_error=True)
            send = user32.SendInput
            send.argtypes = [w.UINT, c.POINTER(KeyboardInput), c.c_int]
            send.restype = w.UINT
            user32.MapVirtualKeyW.argtypes = [w.UINT, w.UINT]
            user32.MapVirtualKeyW.restype = w.UINT
            self._keyboard_api = (KeyboardInput, send, user32.MapVirtualKeyW)
        return self._keyboard_api

    def key(self, virtual_key, before_dispatch=None):
        """One key press (down, up) to the foreground game; the pointer stays.

        Same guards as a click without the pointer: the game must be in front
        and no mouse button or Shift/Ctrl/Alt may be held (a held modifier
        would turn Esc into another shortcut)."""
        _require(virtual_key in KEY_CODES.values(), 'COLLECTION_KEY')
        c = self.c
        self.last_dispatch = None
        self._planned_target = None
        _require(self.foreground(), 'BATCH_FOREGROUND_LOST')
        if before_dispatch is not None:
            before_dispatch()
        # A pointer in motion means the user is taking over; the click path
        # stops the same way. Look twice, 20 ms apart, before any key event.
        resting = self._cursor()
        time.sleep(.02)
        _require(self._cursor() == resting, 'CURSOR_INTERFERENCE')
        _require(self.foreground() and self._idle_input(), 'COLLECTION_INPUT_GUARD_CHANGED')
        KeyboardInput, send, scan_code = self._keyboard()
        scan = scan_code(virtual_key, 0)
        down, up = KeyboardInput(), KeyboardInput()
        for item, flags in ((down, 0), (up, 2)):  # KEYEVENTF_KEYUP = 2
            item.type = 1  # INPUT_KEYBOARD
            item.data.ki.wVk, item.data.ki.wScan, item.data.ki.dwFlags = virtual_key, scan, flags
        started = time.monotonic() * 1000
        pressed = released = cleanup = 0
        held_ms = 0
        error = None
        try:
            pressed = send(1, c.byref(down), c.sizeof(KeyboardInput))
            if pressed == 1:
                # Bounded hold, like the selection press: a game that samples
                # key state once per frame can miss a zero-length press.
                hold_started = time.monotonic() * 1000
                time.sleep(.03)
                held_ms = time.monotonic() * 1000 - hold_started
                _require(self.foreground(), 'BATCH_FOREGROUND_LOST')
        except BaseException as caught:
            error = caught
        finally:
            if pressed == 1:
                # Release is cleanup, never a second press.
                try:
                    released = send(1, c.byref(up), c.sizeof(KeyboardInput))
                except BaseException as caught:
                    error = error or caught
                if released != 1:
                    try:
                        cleanup = send(1, c.byref(up), c.sizeof(KeyboardInput))
                    except BaseException as caught:
                        error = error or caught
            self.last_dispatch = dict(api='SendInput', kind='key', virtual_key=virtual_key, scan_code=scan,
                started_mono_ms=started, returned_mono_ms=time.monotonic() * 1000, expected_events=2,
                returned_events=pressed + released, press_events=pressed, release_events=released,
                release_cleanup_events=cleanup, requested_hold_ms=30, observed_hold_ms=held_ms,
                clock='python_monotonic', semantics='API_call_boundary_not_hardware_delivery_timestamp',
                error=str(error) if error is not None else None,
                last_error=c.get_last_error() if pressed + released != 2 else 0)
        if error is not None:
            raise error
        return pressed + released

    def leave(self):
        restored = False
        try:
            if self.identities and getattr(self, 'stay_in_target', False):
                self._sample()
                restored = self.u.GetForegroundWindow() == self.identities['target_hwnd']
            elif self.identities:
                self.activate(self.identities['return_hwnd'], self.identities['return_pid'])
                if self.pointer is not None:
                    try:
                        self._move_screen((self.pointer.x, self.pointer.y), self.identities['return_hwnd'],
                                          self._idle_input, 'restore_pointer')
                    except MotionInterrupted as error:
                        self.cursor_restore_error = str(error)
                self._sample()
                restored = self.u.GetForegroundWindow() == self.identities['return_hwnd']
        finally:
            if self.capture_worker is not None:
                try:
                    self.capture_worker.close()
                except Exception as error:
                    self.capture_worker_close_error = str(error) or type(error).__name__
            self.halt.set()
            if self.watcher is not None:
                self.watcher.join(timeout=1)
            if self.dpi:
                self.u.SetThreadDpiAwarenessContext(self.dpi)
        return restored

    def metadata(self):
        return dict(self._metadata, identities=dict(self.identities),
                    foreground_transitions=list(self.transitions), foreground_sampling_interval_ms=10,
                    cursor_motions=list(self.motion_history), cursor_restore_error=self.cursor_restore_error,
                    capture_transport=(self.capture_worker.metadata() if self.capture_worker is not None
                                       else dict(mode='one_shot')),
                    capture_worker_close_error=self.capture_worker_close_error,
                    cursor_motion_policy='deterministic_cubic_bezier_ease_in_out_120_to_280ms')


class ForegroundSession:
    """One foreground lifetime; perform raises after durably recording failure.

    With usage restores once, including entry/step exceptions. finish is
    idempotent. reset_segment requires no pending receipt. Reports omit images.
    """
    def __init__(self, record, *, root=ROOT, timeout_seconds=900, max_steps=1000,
                 journal_directory=None, plan=None, backend=None, now=time.monotonic,
                 wait=time.sleep, entry_settle_seconds=.9, input_settle_seconds=.6, numeric_price=False,
                 persistent_capture=False, fast_settle=False, async_reports=False, pixel_receipts=False,
                 pipeline_receipts=False, stop_requested=None, step_listener=None):
        _require(_finite(timeout_seconds) and 1 <= timeout_seconds <= 3600, 'COLLECTION_SESSION_TIMEOUT')
        _require(stop_requested is None or callable(stop_requested), 'COLLECTION_STOP_HOOK')
        # Checked before every step, never inside one: a requested stop sends
        # no further input; an in-flight receipt is still settled by finish.
        self.stop_requested = stop_requested
        # Development log: called with (index, step record) after every step;
        # its own failures are swallowed and never change the session.
        _require(step_listener is None or callable(step_listener), 'COLLECTION_STEP_LISTENER')
        self.step_listener = step_listener
        _require(type(max_steps) is int and 1 <= max_steps <= 10000, 'COLLECTION_SESSION_STEP_LIMIT')
        _require(all(_finite(v) and 0 <= v <= 1 for v in (entry_settle_seconds, input_settle_seconds)), 'COLLECTION_SESSION_SETTLE')
        _require(type(numeric_price) is bool, 'COLLECTION_NUMERIC_PRICE_OPTION')
        _require(type(persistent_capture) is bool, 'COLLECTION_CAPTURE_WORKER_OPTION')
        _require(type(fast_settle) is bool, 'COLLECTION_FAST_SETTLE_OPTION')
        _require(all(type(v) is bool for v in (async_reports, pixel_receipts, pipeline_receipts)),
                 'COLLECTION_SESSION_OPTION')
        _require(not pipeline_receipts or pixel_receipts, 'COLLECTION_PIPELINE_REQUIRES_PIXEL_RECEIPTS')
        _require(not pixel_receipts or fast_settle, 'COLLECTION_PIXEL_RECEIPT_REQUIRES_FAST_SETTLE')
        self.root = Path(root).resolve()
        self.report_path = (self.root / record).resolve()
        _require(self.report_path.is_relative_to(self.root / 'artifacts') and self.report_path.suffix == '.json', 'COLLECTION_REPORT_PATH')
        self.report_path.parent.mkdir(parents=True, exist_ok=True)
        with self.report_path.open('x', encoding='utf-8') as output:
            output.write('{}')
        self.backend, self.now, self.wait = backend, now, wait
        self.timeout_seconds, self.max_steps = timeout_seconds, max_steps
        self.entry_settle_seconds, self.input_settle_seconds = entry_settle_seconds, input_settle_seconds
        self.numeric_price = numeric_price
        self.persistent_capture = persistent_capture
        self.fast_settle = fast_settle
        self.pixel_receipts, self.pipeline_receipts = pixel_receipts, pipeline_receipts
        self._report_writer = AsyncJsonWriter() if async_reports else None
        self._receipt_job = None
        self.receipt_listener = None
        self.receipt_settlements = []
        journal_path = Path(journal_directory or self.root / 'artifacts/m2_savedvalue_collection/journal').resolve()
        _require(journal_path.is_relative_to(self.root / 'artifacts'), 'COLLECTION_JOURNAL_PATH')
        self.journal = CollectionJournal(journal_path)
        if fast_settle:
            # Read/validate history once now, before any foreground lease, so
            # the first star decision uses the warm directory-signature cache.
            self.journal.decision_records()
        self.previous, self.pending = {}, None
        self.pending_geometry = None
        self.selected_lease = None
        self.segment_finished = False
        self.previous_received = self.previous_age_upper_ms = None
        self.last_preview = None
        self.last_price_image = None
        self.last_title_image = None
        self.last_catalog_image = None
        self.clicks = 0
        self.keys = 0
        self.steps = []
        self.report = dict(passed=False, steps=self.steps, enter_calls=0, leave_calls=0,
                           error=None, image_file_writes=0, session_mode='continuous', plan=plan,
                           timeout_seconds=timeout_seconds, max_steps=max_steps, numeric_price_enabled=numeric_price,
                           fast_settle_enabled=fast_settle,
                           input_settle_policy=('bounded_dynamic_readback_v1' if fast_settle else 'fixed'),
                           async_reports_enabled=async_reports, pixel_receipts_enabled=pixel_receipts,
                           pipelined_receipts_enabled=pipeline_receipts)
        self.deadline = None
        self._entered = self._finished = self._failed = False
        self._capture_attempts = []
        self._snapshot_cache = {}
        self.step_directory = self.report_path.with_suffix('.steps')
        self.step_directory.mkdir()
        self._write_report()

    @staticmethod
    def _write_json(path, document):
        temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
        with temporary.open('x', encoding='utf-8') as output:
            json.dump(document, output, ensure_ascii=False, indent=2)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)

    def _write_report(self):
        self.report.update(manual_clicks=self.clicks, key_presses=self.keys, pending_collection=self.pending,
                           pending_geometry=self.pending_geometry,
                           segment_finished=self.segment_finished, image_file_writes=0,
                           step_directory=str(self.step_directory),
                           receipt_in_flight=self._receipt_job is not None)
        writer = self._report_writer
        if self._finished and writer is not None:
            self._report_writer = None
            writer.close()
            self.report['report_writer'] = writer.metadata()
            writer = None
        def write(path, document):
            if writer is None:
                self._write_json(path, document)
            else:
                writer.submit(path, json.dumps(document, ensure_ascii=False).encode('utf-8'))
        if self.steps:
            path = self.step_directory / f'{len(self.steps):06d}.json'
            write(path, self.steps[-1])
        if self._finished:
            self._write_json(self.report_path, self.report)
        else:
            # Preserve complete evidence per step without O(N^2) rewriting of
            # all historical OCR packets during a continuous run. In-memory
            # report/steps remain full; the final file keeps the legacy shape.
            summary = dict(self.report)
            keys = ('kind', 'passed', 'status', 'error', 'reason', 'timings', 'capture_roundtrip_ms',
                    'started_mono_ms', 'finished_mono_ms', 'budget_seconds')
            summary['steps'] = [dict({key: step[key] for key in keys if key in step},
                                     step_record=str(self.step_directory / f'{index + 1:06d}.json'))
                                for index, step in enumerate(self.steps)]
            summary['step_payloads_external_until_finish'] = True
            write(self.report_path, summary)

    def flush_reports(self):
        if self._report_writer is not None:
            self._report_writer.flush()

    def __enter__(self):
        return self.enter()

    def enter(self):
        _require(not self._entered and not self._finished, 'COLLECTION_SESSION_ALREADY_ENTERED')
        self._entered = True
        self.deadline = self.now() + self.timeout_seconds
        self.report['enter_calls'] += 1
        started = self.now()
        try:
            if self.backend is None:
                self.backend = WindowsBackend(self.root, persistent_capture=self.persistent_capture)
            self.backend.enter()
            _require(self.backend.foreground(), 'BATCH_FOREGROUND_LOST')
            settle_started = self.now()
            # Hotkey started inside the game: no activation happened, so there
            # is no fullscreen transition to settle.
            if not getattr(self.backend, 'stay_in_target', False):
                self.wait(min(self.entry_settle_seconds, max(0, self.remaining_seconds())))
            self.report['entry_settle_ms'] = round((self.now() - settle_started) * 1000, 3)
            _require(self.remaining_seconds() > 0, 'BATCH_DEADLINE')
            _require(self.backend.foreground(), 'BATCH_FOREGROUND_LOST')
            width, height = self.backend.viewport()
            _require(type(width) is int and type(height) is int and width > 0 and height > 0,
                     'COLLECTION_ENTRY_VIEWPORT')
            motion_started = self.now()
            if getattr(self.backend, 'stay_in_target', False):
                # Started inside the game: the pointer already rests in it and
                # the first step only reads the page; the first input moves it
                # straight to its target (user 2026-10-09: no return trips).
                self.report['entry_neutral_policy'] = 'skipped_started_in_target_pointer_left_in_place'
            else:
                self.report['entry_neutral_policy'] = 'after_settle_checked_foreground_and_viewport'
                # WindowsBackend._position checks actual target ownership and
                # user input before, throughout, and after the existing motion.
                # A failure is terminal for this foreground lease, not retried.
                self.backend.neutral_pointer()
            self.report['entry_neutral_ms'] = round((self.now() - motion_started) * 1000, 3)
            _require(self.remaining_seconds() > 0, 'BATCH_DEADLINE')
            _require(self.backend.foreground(), 'BATCH_FOREGROUND_LOST')
            self.report['entry_ms'] = round((self.now() - started) * 1000, 3)
            self.report.update(self.backend.metadata())
            self._write_report()
            return self
        except BaseException as error:
            self.finish(error)
            raise

    def __exit__(self, exc_type, error, traceback):
        self.finish(error)
        return False

    def remaining_seconds(self):
        return max(0, self.deadline - self.now()) if self.deadline is not None else self.timeout_seconds

    def reset_segment(self):
        _require(self._entered and not self._finished and not self._failed, 'COLLECTION_SESSION_NOT_ACTIVE')
        self.settle_receipts()
        _require(self.pending is None, 'COLLECTION_PENDING_RECONCILIATION')
        _require(self.pending_geometry is None, 'COLLECTION_GEOMETRY_REOBSERVATION_REQUIRED')
        self.segment_finished = False
        self._write_report()

    def _age(self):
        _require(self.previous_received is not None and self.previous_age_upper_ms is not None,
                 'COLLECTION_OBSERVATION_AGE_UNPROVEN')
        return self.previous_age_upper_ms + (self.now() - self.previous_received) * 1000

    def _fresh(self):
        age = self._age()
        _require(_finite(age) and 0 <= age <= 5000, 'COLLECTION_OBSERVATION_EXPIRED')
        return age

    def _identity_arguments(self):
        identities = self.backend.identities
        if getattr(self.backend, 'return_policy', 'ide') == 'entry_foreground':
            # Probes are caller-owned and never restore a window; a hotkey
            # run's return window (often the game itself) is not sent.
            identities = {key: identities[key] for key in ('target_hwnd', 'target_pid')}
        arguments = []
        for key, value in identities.items():
            arguments += ['--' + key.replace('_', '-'), str(value)]
        return arguments

    def _command(self, step):
        _require(not ('card_id' in step and 'visible_index' in step), 'COLLECTION_DYNAMIC_CARD_SELECTOR_CONFLICT')
        _require(not (step.get('collection_layout') and 'card_index' in step), 'COLLECTION_DYNAMIC_LEGACY_INDEX_CONFLICT')
        if 'card_id' in step or 'visible_index' in step:
            _require(step.get('collection_layout') is True, 'COLLECTION_DYNAMIC_LAYOUT_REQUIRED')
        command = [str(self.root / 'dist/RelinkStudio/RelinkStudio.exe'), '--live-capture-check', '--focus-policy', 'caller-owned']
        command += self._identity_arguments()
        command += ['--frames', '1', '--ocr']
        for key, option in {'expected_page':'--expected-page', 'card_index':'--collection-card-index',
                            'card_id':'--collection-card-id', 'visible_index':'--collection-visible-index'}.items():
            if key in step:
                command += [option, str(step[key])]
        for key, option in {'preview':'--preview-stdout', 'market_anchors':'--ocr-market-anchors',
                            'ui_regions':'--ocr-ui-regions', 'filter_state':'--catalog-filter-state',
                            'collection_observation':'--collection-observation',
                            'purchase_observation':'--purchase-observation',
                            'collection_layout':'--collection-layout',
                            'selection_preflight':'--collection-selection-preflight',
                            'scroll_readiness':'--collection-scroll-readiness',
                            'collection_layout_profile':'--collection-layout-profile',
                            'collection_numeric_price':'--collection-numeric-price',
                            'collection_price_image':'--collection-price-image',
                            'collection_title_image':'--collection-title-image',
                            'collection_catalog_image':'--collection-catalog-image'}.items():
            if step.get(key):
                command.append(option)
        if 'receipt_reference' in step:
            command += ['--collection-receipt-reference', json.dumps(step['receipt_reference'],
                ensure_ascii=False, separators=(',', ':'), allow_nan=False)]
        if 'selection_point' in step:
            command += ['--collection-selection-point', json.dumps(step['selection_point'], separators=(',', ':'), allow_nan=False)]
        if 'countdown_watch_ms' in step:
            # Purchase 对表: the native watch reads the latest-frame stream of
            # the watchlist; text and timing evidence only.
            ms = step['countdown_watch_ms']
            _require(type(ms) is int and 500 <= ms <= 10000 and step.get('purchase_observation') is True
                     and step.get('expected_page') in (None, 'watchlist_listings'), 'PURCHASE_COUNTDOWN_WATCH_STEP')
            command += ['--purchase-countdown-watch', str(ms)]
            if step.get('stop_on_button'):
                command.append('--purchase-countdown-stop-on-button')
            if step.get('stop_on_green'):
                command.append('--purchase-countdown-stop-on-green')
            if step.get('stop_on_jump'):
                command.append('--purchase-countdown-stop-on-jump')
                if 'expect_zero_ms' in step:
                    _require(type(step['expect_zero_ms']) in (int, float) and math.isfinite(step['expect_zero_ms']),
                             'PURCHASE_COUNTDOWN_WATCH_STEP')
                    command += ['--purchase-countdown-expect-zero-ms', '%.1f' % step['expect_zero_ms']]
            if 'countdown_area' in step:
                _require(step['countdown_area'] in ('footer', 'dialog'), 'PURCHASE_COUNTDOWN_WATCH_STEP')
                command += ['--purchase-countdown-area', step['countdown_area']]
            for option in ('--reuse-capture-resources', '--collection-ready-stream'):
                if option not in command:
                    command.append(option)
        return command

    def _pending_receipt_reference(self):
        _require(self.pending is not None and self.pending_geometry is None, 'COLLECTION_RECEIPT_PENDING_REQUIRED')
        document = json.loads((self.journal.directory / (self.pending['key'] + '.json')).read_text('utf-8'))
        _require(document.get('key') == self.pending['key'] and document.get('status') == 'dispatched'
                 and document.get('sent') == 2, 'COLLECTION_RECEIPT_DISPATCH_UNPROVEN')
        before = self.pending['candidate']
        for key in ('product', 'condition', 'price', 'wear', 'row_index', 'source_frame_id',
                    'source_frame_sha256', 'card_id', 'card_bounds', 'fields_bounds', 'source_sha256'):
            _require(document.get('candidate', {}).get(key) == before.get(key), 'COLLECTION_RECEIPT_JOURNAL_CONFLICT')
        return receipt_reference(before)

    def _capture(self, step, remaining):
        step = dict(step)
        _require('receipt_reference' not in step, 'COLLECTION_RECEIPT_REFERENCE_INTERNAL')
        _require('selection_preflight' not in step, 'COLLECTION_SELECTION_PREFLIGHT_INTERNAL')
        _require('selection_point' not in step, 'COLLECTION_SELECTION_POINT_INTERNAL')
        _require('scroll_readiness' not in step, 'COLLECTION_SCROLL_READINESS_INTERNAL')
        if self.pending_geometry is not None or (self.pending is not None and self.pending['candidate'].get('geometry_mode') == 'observed_dynamic'):
            _require(step.get('expected_page') in (None, 'skin_listings'), 'COLLECTION_GEOMETRY_PAGE')
            step.update(collection_layout=True, collection_observation=True)
            step['expected_page'] = 'skin_listings'
        if step.get('collection_layout'):
            step['collection_observation'] = True
            if self.numeric_price:
                step['collection_numeric_price'] = True
        if step.get('receipt_if_pending') and self.pending is None:
            return dict(kind='capture', passed=True, skipped=True, reason='no_collection_dispatched')
        if (step.get('expect_collection_added') and self.pending is not None
                and self.pending['candidate'].get('geometry_mode') == 'observed_dynamic'):
            step['receipt_reference'] = self._pending_receipt_reference()
        if (self.fast_settle and getattr(self.backend,'reuse_capture',False)
                and self.pending is None and self.pending_geometry is not None
                and self.pending_geometry['kind']=='selection'):
            step['selection_preflight']=True
            step['selection_point']=list(self.pending_geometry['lease']['point'])
        if (self.fast_settle and getattr(self.backend,'ready_stream',False)
                and getattr(self.backend,'reuse_capture',False) and self.pending is None
                and self.pending_geometry is not None and self.pending_geometry['kind']=='scroll'):
            step['scroll_readiness']=True
        if step.get('neutral_pointer'):
            self.backend.neutral_pointer()
            self.wait(min(.15, self.remaining_seconds()))
        command = self._command(step)
        refresh_origin=self.previous
        observed_frame_ids = set()
        require_new_layout_frame = False
        def once(budget):
            nonlocal require_new_layout_frame
            started = self.now()
            process = self.backend.capture(command, max(.1, min(20, budget)))
            result = json.loads(process.stdout)
            _require(isinstance(result, dict), 'COLLECTION_CAPTURE_RESULT')
            preview = result.pop('preview_png_base64', None)
            # The diagnostic may expose a narrow price bitmap for explicit
            # in-memory review. Remove the entire envelope before any packet,
            # attempt, step or summary can be written. Never keep a stale crop.
            self.last_price_image = result.pop('collection_price_image', None)
            self.last_title_image = result.pop('collection_title_image', None)
            self.last_catalog_image = result.pop('collection_catalog_image', None)
            # A lean capture has no full preview. Do not present the older
            # debug image as though it belonged to this newer observation.
            self.last_preview = preview
            received = self.now()
            roundtrip_ms = (received - started) * 1000
            self.previous = result
            self.previous_received = received
            self.previous_age_upper_ms = conservative_observation_age(result, roundtrip_ms)
            result['coordinator_observation'] = dict(capture_started_mono_ms=started * 1000,
                received_mono_ms=received * 1000, capture_roundtrip_ms=roundtrip_ms,
                age_upper_at_receive_ms=self.previous_age_upper_ms,
                age_basis='max(worker_age,coordinator_roundtrip+source_age+uncertainty)')
            passed = (process.returncode == 0 and result.get('capture_passed') is True
                      and result.get('ocr_passed') is True and result.get('focus_activation_requests') == 0
                      and result.get('focus_restore_requests') == 0)
            if result.get('collection_requested_card_error'):
                passed = False
            filter_retryable = False
            if step.get('expected_filter') is not None:
                result['filter_expectation_passed'] = (
                    filter_matches_under_pointer(result, step['expected_filter'], step['filter_pointer_key'])
                    if step.get('filter_pointer_key') else filter_expectation_matches(result, step['expected_filter']))
                passed = passed and result['filter_expectation_passed']
                filter_retryable = bool(not passed and process.returncode == 0
                    and result.get('focus_activation_requests') == 0 and result.get('focus_restore_requests') == 0
                    and filter_readback_unsettled(result, step['expected_filter']))
            if step.get('expected_condition_filter') is not None:
                actual = result.get('collection_condition_filter', {})
                result['condition_filter_passed'] = all(actual.get(k, {}).get('state') == v
                    for k, v in step['expected_condition_filter'].items())
                passed = passed and result['condition_filter_passed']
            base_passed = passed
            state_waiting = False
            frame_id = result.get('collection_observation', {}).get('frame_id')
            new_layout_frame = (not require_new_layout_frame
                                or (isinstance(frame_id, str) and frame_id not in observed_frame_ids))
            if isinstance(frame_id, str):
                observed_frame_ids.add(frame_id)
            if not new_layout_frame:
                result['collection_layout_passed'] = False
                result['collection_geometry_error'] = 'COLLECTION_REOBSERVATION_REQUIRED'
                passed = False
            if passed and step.get('collection_layout') and result.get('startup_page', {}).get('page') == 'skin_listings' and result['startup_page'].get('overlay') == 'none':
                try:
                    observe_layout(result)
                    if self.pending_geometry is not None:
                        pending = self.pending_geometry
                        if pending['kind'] == 'selection':
                            rebound = rebind_selected(result, pending['rule'], pending['lease'])
                            self.selected_lease = rebound['lease']
                        else:
                            rebound = rebind_after_scroll(result, pending['rule'], pending['lease'])
                            self.selected_lease = None
                        result['collection_geometry_rebind'] = dict(kind=pending['kind'], passed=True, evidence=rebound)
                        self.pending_geometry = None
                    result['collection_layout_passed'] = True
                except (ValueError, KeyError) as error:
                    result['collection_layout_passed'] = False
                    result['collection_geometry_error'] = str(error)
                    state_waiting = bool(self.fast_settle and self.pending_geometry is not None
                        and str(error) in ('COLLECTION_REOBSERVATION_REQUIRED',
                            'COLLECTION_SELECTION_GEOMETRY_CHANGED', 'COLLECTION_SCROLL_PROGRESS_UNPROVEN')
                        and unchanged_layout_waiting(result, self.pending_geometry))
                    if (self.fast_settle and not state_waiting and self.pending_geometry is not None
                            and str(error) == 'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
                        waiting = selection_readiness_waiting(result, self.pending_geometry)
                        if waiting is not None:
                            result['collection_selection_readiness'] = waiting
                            state_waiting = True
                    passed = False
            receipt_only = False
            if (base_passed and new_layout_frame and not passed and 'receipt_reference' in step
                    and self.pending is not None and self.pending_geometry is None
                    and result.get('collection_layout', {}).get('complete') is False
                    and result.get('collection_layout', {}).get('selected_card_proof') is not None):
                try:
                    validate_receipt_geometry(result, self.pending['candidate'])
                    result['collection_receipt_geometry_passed'] = True
                    result['collection_geometry_scope'] = 'receipt_only'
                    self.selected_lease = None
                    receipt_only = True
                    passed = True
                except (ValueError, KeyError) as error:
                    result['collection_receipt_geometry_passed'] = False
                    result['collection_receipt_geometry_error'] = str(error)
                    result['collection_geometry_error'] = str(error)
            if step.get('expect_collection_added'):
                result['collection_receipt_passed'] = False
                if self.pending is not None and passed:
                    try:
                        after = match_selected_first_card(result, self.pending['rule'],
                                                          receipt_before=self.pending['candidate'],
                                                          receipt_only=receipt_only)
                        result['collection_receipt_passed'] = receipt_matches(self.pending['candidate'], after, result)
                        before = self.pending['candidate']
                        if before.get('geometry_mode') == 'observed_dynamic':
                            result['collection_receipt_passed'] = (result['collection_receipt_passed']
                                and after.get('geometry_mode') in ('observed_dynamic', 'receipt_only_observed_dynamic')
                                and same_card_geometry(before, after))
                            state_waiting = bool(self.fast_settle
                                and not result['collection_receipt_passed']
                                and same_item_white_waiting(before, after, result))
                    except (ValueError, KeyError) as error:
                        result['collection_receipt_error'] = str(error)
                    if result['collection_receipt_passed']:
                        kind = 'toast_and_gold' if result['startup_page'].get('anchor_checks', {}).get('collection.added') else 'same_item_white_to_gold'
                        result['collection_receipt_kind'] = kind
                        self.journal.update(self.pending['key'], 'confirmed', receipt=after,
                                            receipt_kind=kind, record=str(self.report_path))
                        self.pending = None
                elif self.pending is not None:
                    result['collection_receipt_error'] = result.get('collection_geometry_error',
                                                                   'COLLECTION_RECEIPT_OBSERVATION_INVALID')
                passed = passed and result['collection_receipt_passed']
            # An incomplete unrelated tail may also obscure a favorite receipt.
            # Reobserve only; retain the journal reservation and never dispatch
            # that star again. A complete fresh same-item/gold packet is still
            # mandatory. Malformed frames, drift and identity conflicts stop.
            receipt_retry_allowed = (self.pending is None or (step.get('expect_collection_added')
                and self.pending['candidate'].get('geometry_mode') == 'observed_dynamic'
                and exact_label_target(result, 'product_title', self.pending['rule']['product_name']) is not None))
            layout_retryable = bool(base_passed and new_layout_frame and step.get('collection_layout')
                and not passed and result.get('collection_receipt_geometry_passed') is not True
                and receipt_retry_allowed
                and result.get('collection_geometry_error') == 'COLLECTION_LAYOUT_UNPROVEN'
                and transient_layout_failure(result))
            receipt_retryable = bool(base_passed and new_layout_frame and step.get('expect_collection_added')
                and self.pending is not None and receipt_retry_allowed
                and (result.get('collection_layout_passed') is True
                     or result.get('collection_receipt_geometry_passed') is True)
                and result.get('collection_receipt_error') == 'COLLECTION_PRICE_MISSING')
            state_retryable = bool(base_passed and new_layout_frame and state_waiting
                and not passed and _finite(self.previous_age_upper_ms)
                and 0 <= self.previous_age_upper_ms <= 5000)
            price_readiness = (local_price_readiness_waiting(result, self.pending_geometry)
                if self.fast_settle and self.pending is None and step.get('collection_layout')
                and step.get('selected_geometry_required') and process.returncode == 0
                and new_layout_frame and _finite(self.previous_age_upper_ms)
                and 0 <= self.previous_age_upper_ms <= 5000 else None)
            if (price_readiness is None and self.fast_settle and self.pending is None
                    and self.pending_geometry is None and step.get('collection_layout')
                    and step.get('expected_page')=='skin_listings'
                    and not step.get('expect_collection_added') and process.returncode==0
                    and new_layout_frame and _finite(self.previous_age_upper_ms)
                    and 0<=self.previous_age_upper_ms<=5000):
                price_readiness=(local_price_refresh_waiting(result,refresh_origin)
                                 or local_field_unread_listing_waiting(result))
            price_retryable = bool(price_readiness and not passed)
            if price_retryable:
                result['collection_price_readiness'] = price_readiness
            if state_retryable:
                result['collection_state_waiting'] = (result['collection_selection_readiness']['reason']
                    if 'collection_selection_readiness' in result else 'unchanged_geometry_or_same_item_white')
            preflight_retryable=bool(step.get('selection_preflight') and self.pending is None
                and process.returncode==0 and new_layout_frame and self.pending_geometry is not None
                and result.get('focus_activation_requests')==0 and result.get('focus_restore_requests')==0
                and _finite(self.previous_age_upper_ms) and 0<=self.previous_age_upper_ms<=5000
                and selection_preflight_waiting(result,self.pending_geometry))
            # The native wheel stability gate ran out of time on frames still
            # re-rendering: observe again (no input), bounded by attempts.
            # The desktop duplication lost its frame source for a moment (live
            # 2026-10-10 00:53/00:54: E_DXGI_ACQUIRE twice, the next capture
            # fine; the native side drops its cached duplication on any failure).
            # Read again, never for a timed countdown watch.
            acquire_retryable = bool(process.returncode != 0 and result.get('error') in TRANSIENT_CAPTURE_ERRORS
                                     and 'countdown_watch_ms' not in step
                                     and result.get('focus_activation_requests', 0) == 0
                                     and result.get('focus_restore_requests', 0) == 0)
            scroll_retryable = bool(step.get('scroll_readiness') and self.pending is None
                and self.pending_geometry is not None and self.pending_geometry['kind'] == 'scroll'
                and process.returncode == 1 and result.get('error') == 'E_COLLECTION_SCROLL_NOT_STABLE'
                and result.get('target_foreground_retained') is True
                and result.get('focus_activation_requests') == 0 and result.get('focus_restore_requests') == 0)
            require_new_layout_frame = (layout_retryable or receipt_retryable or state_retryable or price_retryable
                                        or preflight_retryable or scroll_retryable)
            observed = dict(kind='capture', passed=bool(passed), command=command, result=result,
                            stderr=process.stderr.decode('utf-8', errors='replace'), exit_status=process.returncode,
                            capture_roundtrip_ms=round(roundtrip_ms, 3),
                            collection_layout_retryable=layout_retryable,
                            collection_receipt_retryable=receipt_retryable,
                            collection_state_retryable=state_retryable,
                            collection_selection_preflight_retryable=preflight_retryable,
                            collection_price_retryable=price_retryable,
                            collection_scroll_readiness_retryable=scroll_retryable,
                            catalog_filter_retryable=filter_retryable,
                            capture_acquire_retryable=acquire_retryable)
            self._capture_attempts.append(observed)
            self.steps[-1]['capture_attempts'] = list(self._capture_attempts)
            self._write_report()
            return observed
        return stabilize_capture(once, self.backend.foreground,
            attempts=step.get('attempts', 3 if step.get('expected_page') or step.get('collection_layout')
                              else 1 if 'countdown_watch_ms' in step else 2),
            deadline=self.now() + remaining, now=self.now, wait=self.wait,
            retry_observation=lambda observed: (observed.get('collection_layout_retryable') is True
                                               or observed.get('collection_receipt_retryable') is True
                                               or observed.get('collection_state_retryable') is True
                                               or observed.get('collection_selection_preflight_retryable') is True
                                               or observed.get('collection_price_retryable') is True
                                               or observed.get('collection_scroll_readiness_retryable') is True
                                               or observed.get('catalog_filter_retryable') is True
                                               or observed.get('capture_acquire_retryable') is True),
            retry_delay=lambda observed: (.035 if observed.get('collection_selection_preflight_retryable') is True
                                          or observed.get('collection_scroll_readiness_retryable') is True
                                          else .3 if observed.get('capture_acquire_retryable') is True else .15))

    def _snapshot(self, step):
        path = (self.root / step['snapshot']).resolve()
        _require(path.is_relative_to(self.root / 'artifacts') and path.suffix == '.json', 'COLLECTION_SNAPSHOT_PATH')
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        expected = step.get('snapshot_sha256')
        if expected is not None:
            _require(isinstance(expected, str) and len(expected) == 64
                     and all(ch in '0123456789abcdef' for ch in expected), 'COLLECTION_SNAPSHOT_HASH')
            _require(digest == expected, 'COLLECTION_SNAPSHOT_CHANGED')
        if path in self._snapshot_cache:
            frozen = self._snapshot_cache[path]
            _require(digest == frozen['sha256'], 'COLLECTION_SNAPSHOT_CHANGED')
            snapshot = copy.deepcopy(frozen['document'])
        else:
            snapshot = json.loads(raw)
            self._snapshot_cache[path] = dict(sha256=digest, document=copy.deepcopy(snapshot))
        _require(snapshot.get('ready') is True and snapshot.get('mode') == 'collect_only'
                 and snapshot.get('purchase_phase_enabled') is False, 'COLLECTION_SNAPSHOT_MODE')
        rows = [row for row in snapshot['rows'] if row['row_index'] == step['row_index']]
        _require(len(rows) == 1, 'COLLECTION_SNAPSHOT_ROW')
        _require(type(rows[0].get('limit_raw')) is int and rows[0]['limit_raw'] == 0,
                 'COLLECTION_NONZERO_LIMIT_REQUIRES_COUNTER')
        _require(isinstance(snapshot.get('source_sha256'), str) and len(snapshot['source_sha256']) == 64
                 and all(ch in '0123456789abcdef' for ch in snapshot['source_sha256']), 'COLLECTION_SNAPSHOT_SOURCE')
        return snapshot, rows[0]

    def _perform(self, step, remaining):
        kind = step['kind']
        if self.segment_finished:
            return dict(kind=kind, passed=True, skipped=True, reason='price_limit_segment_finished')
        # A pipelined pixel receipt is settled inside the next selection's
        # dispatch guard (after its pointer motion, before any input). A
        # scroll lease reads the journal, so scrolling settles first.
        deferred_receipt = self._receipt_job is not None and kind == 'select_visible_card'
        if self._receipt_job is not None and not deferred_receipt:
            self._settle_receipt()
        if kind == 'capture':
            return self._capture(step, remaining)
        if deferred_receipt:
            _require(self.pending is not None and self.pending['key'] == self._receipt_job.pending_key,
                     'COLLECTION_PENDING_RECONCILIATION')
        else:
            _require(self.pending is None, 'COLLECTION_PENDING_RECONCILIATION')
        _require(self.pending_geometry is None, 'COLLECTION_GEOMETRY_REOBSERVATION_REQUIRED')
        _require(self.previous.get('collection_geometry_scope') != 'receipt_only',
                 'COLLECTION_FULL_LAYOUT_REOBSERVATION_REQUIRED')
        if step.get('skip_if_overlay') and self.previous.get('startup_page', {}).get('overlay') == step['skip_if_overlay']:
            return dict(kind=kind, passed=True, skipped=True, reason='desired_overlay_already_open')
        if step.get('skip_if_condition_state'):
            name, state = step['skip_if_condition_state']
            actual = self.previous.get('collection_condition_filter', {}).get(name, {}).get('state')
            _require(actual in ('checked', 'unchecked'), 'COLLECTION_CONDITION_STATE_UNKNOWN')
            if actual == state:
                return dict(kind=kind, passed=True, skipped=True, reason='condition_state_already_set')
        _require(self.previous.get('startup_page', {}).get('page') == step.get('expected_before') != 'unknown', 'COLLECTION_PAGE')
        _require(self.previous['startup_page']['overlay'] == step.get('expected_overlay', 'none'), 'COLLECTION_OVERLAY')
        if step.get('require_label'):
            required = step['require_label']
            _require(exact_label_target(self.previous, required['region'], required['label']) is not None, 'COLLECTION_TITLE_NOT_CONFIRMED')
        width, height = self.backend.viewport()
        _require((width, height) == (self.previous['frames'][-1]['width'], self.previous['frames'][-1]['height']), 'COLLECTION_VIEWPORT_CHANGED')
        if step.get('viewport'):
            _require([width, height] == step['viewport'], 'COLLECTION_VIEWPORT_CHANGED')
        collection = None
        geometry = None
        basis = dict(packet=self.previous, received=self.previous_received, age=self.previous_age_upper_ms)
        if kind in ('select_visible_card', 'scroll_visible_list'):
            lease = step.get('lease', {})
            _require(isinstance(lease, dict) and isinstance(lease.get('scope'), dict), 'COLLECTION_GEOMETRY_LEASE')
            rule = dict(lease['scope'], enabled=True, dictionary_resolved=True)
            self._fresh()
            if kind == 'select_visible_card':
                validated = validate_card_lease(self.previous, rule, lease)
                geometry = dict(kind='selection', lease=validated, rule=rule)
            else:
                records = self.journal.decision_records()
                validated = validate_scroll_lease(self.previous, rule, records, lease)
                geometry = dict(kind='scroll', lease=validated, rule=rule)
            if self.fast_settle:
                geometry['wait_layout'] = observe_layout(self.previous)
            point = validated['point']
            self.pending_geometry = geometry
            self.selected_lease = None
            self._write_report()
        elif kind == 'collect_selected':
            records = []
            for record in self.journal.decision_records():
                records.append(record)
                _require(record.get('status') in ('confirmed', 'confirmed_reconciliation'), 'COLLECTION_PENDING_RECONCILIATION')
            snapshot, rule = self._snapshot(step)
            collection = match_selected_first_card(self.previous, rule)
            if step.get('skip_ineligible') and not collection['eligible']:
                if step.get('end_segment_on_price_above'):
                    from decimal import Decimal
                    self.segment_finished = Decimal(collection['price']) > Decimal(rule['price_max'])
                return dict(kind=kind, passed=True, skipped=True, reason='rule_not_matched', candidate=collection)
            _require(collection['eligible'], 'COLLECTION_RULE_NOT_MATCHED')
            selected = self.previous['collection_selected_card']
            # The current verified star, not completed statistical history,
            # decides eligibility for a new attempt. Pending input still blocks
            # all repetition; the journal preserves every completed attempt.
            disposition = collection_disposition(collection, selected, records)
            if step.get('skip_favorited') and selected['favorite_warm_fraction'] >= .05 and selected['favorite_bright_fraction'] <= .02:
                return dict(kind=kind, passed=True, skipped=True, reason='already_favorited', candidate=collection)
            age = self._fresh()
            _require(selected['favorite_warm_fraction'] == 0, 'COLLECTION_FAVORITE_ALREADY_COLORED')
            _require(selected['favorite_bright_fraction'] >= .04, 'COLLECTION_FAVORITE_WHITE_UNPROVEN')
            _require(remaining >= 7, 'COLLECTION_RECEIPT_TIME_BUDGET')
            collection.update(source_sha256=snapshot['source_sha256'], age_at_action_ms=age,
                receipt_status='pending', favorite_before=copy.deepcopy(selected),
                star_policy='observed_white_over_completed_history',
                history_disposition=copy.deepcopy(disposition))
            self.pending = dict(candidate=collection, rule=rule,
                key=self.journal.prepare(collection, dict(record=str(self.report_path), point=[2339, 320]),
                                         allow_confirmed_history=True))
            self._write_report()
            point = [2339, 320]
        elif kind == 'click_label':
            point = exact_label_target(self.previous, step['region'], step['label'])
            _require(point is not None, 'COLLECTION_LABEL_NOT_UNIQUE_OR_MISSING')
            point = list(point)
        elif kind == 'key':
            _require(step.get('key') in KEY_CODES, 'COLLECTION_KEY')
            point = None
        else:
            point = step['point']
        _require(kind == 'key' or (isinstance(point, (list, tuple)) and len(point) == 2
                 and all(type(v) is int for v in point) and 0 < point[0] < width and 0 < point[1] < height),
                 'COLLECTION_INPUT_POINT')
        receipt_outcome = None
        started = self.now()
        if collection is not None or geometry is not None:
            # fsync/report serialization also consume the unchanged 5 s budget.
            age = self._fresh()
            if collection is not None:
                collection['age_at_action_ms'] = age
        def dispatch_guard():
            if self._receipt_job is not None:
                # The pointer already moved; no input before this receipt and
                # an unchanged list behind the planned lease are proven.
                job_basis = self._receipt_job.basis
                outcome = self._settle_receipt()
                if outcome['layout_unchanged'] is not True:
                    # The receipt frames were taken while the pointer moved
                    # (hover effects may hide edges). Read the list once more,
                    # now with the pointer at rest, before any input.
                    outcome['layout_recheck'] = self._recheck_layout(job_basis)
                    _require(outcome['layout_recheck'] is True, 'COLLECTION_RECEIPT_LAYOUT_CHANGED')
            _require(self.backend.foreground(), 'BATCH_FOREGROUND_LOST')
            age = self._fresh()
            _require(self.remaining_seconds() > 0, 'BATCH_DEADLINE')
            if collection is not None:
                self._snapshot(step)  # Detect changes while the pointer moved.
                _require(self.remaining_seconds() >= 7, 'COLLECTION_RECEIPT_TIME_BUDGET')
                collection['age_at_action_ms'] = age
        if kind in ('click', 'click_label', 'collect_selected', 'select_visible_card'):
            click=self.backend.click
            if (self.fast_settle and kind in ('collect_selected','select_visible_card')
                    and getattr(self.backend,'fast_collection_motion',False)):
                click=self.backend.fast_collection_click
                if kind=='select_visible_card' and callable(getattr(self.backend,'fast_selection_click',None)):
                    click=self.backend.fast_selection_click
            try:
                sent = click(point, before_dispatch=dispatch_guard)
            except BaseException as error:
                # A star input that never reached SendInput (pointer motion or
                # a dispatch guard stopped first) leaves no attempt to
                # reconcile: retire our own prepared reservation with evidence.
                if (collection is not None and self.pending is not None
                        and getattr(self.backend, 'last_dispatch', 'unknown') is None):
                    archived = self.journal.release_unsent(self.pending['key'],
                        archive_directory=self.journal.directory.parent / 'released_unsent',
                        evidence=dict(reason='input_not_dispatched', error=str(error) or type(error).__name__,
                                      session_record=str(self.report_path), step_index=len(self.steps)))
                    self.steps[-1]['released_unsent_reservation'] = str(archived)
                    self.pending = None
                raise
            self.previous = {}
            if collection is not None:
                self.journal.update(self.pending['key'], 'dispatched' if sent == 2 else 'input_uncertain', sent=sent)
            _require(sent == 2, 'COLLECTION_INPUT_UNCERTAIN')
            self.clicks += 1
            if (collection is not None and self.pixel_receipts and getattr(self.backend, 'pixel_receipt', False)
                    and collection.get('geometry_mode') == 'observed_dynamic'):
                receipt_outcome = self._start_pixel_receipt(basis)
        elif kind in ('scroll', 'scroll_visible_list'):
            delta = geometry['lease']['delta'] if geometry is not None else step['delta']
            # Batch deltas are available only after the dynamic lease has
            # passed validate_scroll_lease above. Generic navigation keeps its
            # existing bound and cannot opt in by supplying calibration fields.
            allowed_deltas = ((-600, -480, -120, 120, 480, 600) if geometry is not None
                              else (-360, -240, -120, 120, 240, 360))
            _require(type(delta) is int and delta in allowed_deltas, 'COLLECTION_SCROLL_BOUND')
            scroll = self.backend.scroll
            if (kind == 'scroll_visible_list' and self.fast_settle and getattr(self.backend, 'fast_collection_motion', False)
                    and callable(getattr(self.backend, 'fast_collection_scroll', None))):
                scroll = self.backend.fast_collection_scroll
            sent = scroll(point, delta, before_dispatch=dispatch_guard)
            self.previous = {}
            _require(sent == 1, 'COLLECTION_INPUT_UNCERTAIN')
        elif kind == 'key':
            sent = self.backend.key(KEY_CODES[step['key']], before_dispatch=dispatch_guard)
            self.previous = {}
            _require(sent == 2, 'COLLECTION_INPUT_UNCERTAIN')
            self.keys += 1
        else:
            self.backend.hover(point)
        input_ms = (self.now() - started) * 1000
        motion = getattr(self.backend, 'last_motion', None)
        motion_ms = float(motion.get('elapsed_ms', 0)) if isinstance(motion, dict) else 0
        dispatch_ms = max(0, input_ms - motion_ms)
        # A successfully dispatched selection/star immediately requests its
        # next fresh frame. The existing selected-geometry/receipt gates and
        # bounded readiness retries establish readiness, not an unconditional
        # 120/80 ms pause. This is our measured wait policy, not a recovered
        # BBZPS clickinterval mapping. Navigation and scrolling stay unchanged.
        # With the native latest-frame scroll stability gate, the wheel needs
        # no blind pre-wait: the gate requires 3 stable frames over 100 ms.
        dynamic_settle = dict(select_visible_card=0.0, collect_selected=0.0,
                              scroll_visible_list=0.0 if getattr(self.backend, 'ready_stream', False) else .18)
        reduced_settle = self.fast_settle and kind in dynamic_settle
        # Only dynamic actions have a mandatory geometry/receipt barrier. Do
        # not shorten generic navigation whose next expected state is broader.
        target_settle = min(self.input_settle_seconds, dynamic_settle[kind]) if reduced_settle else self.input_settle_seconds
        if kind == 'hover':
            # No input reached the game; only the left control's glow fades.
            target_settle = min(self.input_settle_seconds, .1)
        settle = min(target_settle, max(.01, remaining / 4), self.remaining_seconds())
        settle_started = self.now()
        if settle > 0:
            self.wait(settle)
        returned = dict(kind=kind, point=list(point) if point is not None else None, passed=True,
                        timings=dict(input_dispatch_ms=round(dispatch_ms, 3), motion_ms=round(motion_ms, 3),
                                     settle_wait_ms=round((self.now() - settle_started) * 1000, 3)))
        returned['settle_policy'] = 'bounded_dynamic_readback_v1' if reduced_settle else 'fixed'
        if reduced_settle and kind in ('select_visible_card', 'collect_selected'):
            returned['settle_policy'] = 'immediate_fresh_readback_v2'
            returned['post_click_fixed_wait_ms'] = 0
        if motion is not None:
            returned['motion'] = copy.deepcopy(motion)
        dispatch = getattr(self.backend, 'last_dispatch', None)
        if kind in ('click', 'click_label', 'collect_selected', 'select_visible_card', 'key') and isinstance(dispatch, dict):
            returned['input_dispatch'] = copy.deepcopy(dispatch)
        if collection is not None:
            returned['collection_attempt'] = collection
            returned['collection_attempt_key'] = self.pending['key'] if self.pending is not None else receipt_outcome['key']
            if receipt_outcome is not None:
                returned['receipt_mode'] = receipt_outcome['mode']
                returned['receipt_outcome'] = copy.deepcopy(receipt_outcome)
        if geometry is not None:
            returned['geometry_action'] = geometry
        return returned

    def _window_lost(self, error):
        """BATCH_FOREGROUND_LOST / COLLECTION_TARGET_OCCLUDED when a step failed
        because the game left the front or was covered (the native capture
        refuses such frames, also in receipt and layout re-checks; review
        wf_993b38d0-6b8 / wf_b0b8309e-39d), else None: it is no read failure,
        and the F2 cycle must stop instead of bringing the game back."""
        if not isinstance(error, Exception) or str(error) in ('COLLECTION_STOP_REQUESTED', 'BATCH_FOREGROUND_LOST',
                                                              'COLLECTION_TARGET_OCCLUDED'):
            return None
        natives = [str((attempt.get('result') or {}).get('error') or '') for attempt in self._capture_attempts]
        if 'E_WINDOW_OCCLUDED' in natives:
            return 'COLLECTION_TARGET_OCCLUDED'
        if any(native in WINDOW_LOST_ERRORS for native in natives):
            return 'BATCH_FOREGROUND_LOST'
        try:
            return 'BATCH_FOREGROUND_LOST' if self.backend.foreground() is False else None
        except Exception:
            return None

    def perform(self, step, remaining=None):
        _require(self._entered and not self._finished and not self._failed, 'COLLECTION_SESSION_NOT_ACTIVE')
        if self.stop_requested is not None and self.stop_requested():
            self._failed = True
            self.report.update(error='COLLECTION_STOP_REQUESTED', stop_requested=True)
            self._write_report()
            raise RuntimeError('COLLECTION_STOP_REQUESTED')
        _require(isinstance(step, dict) and step.get('kind') in KINDS, 'COLLECTION_STEP_KIND')
        _require(len(self.steps) < self.max_steps, 'COLLECTION_SESSION_STEP_LIMIT')
        budget = self.remaining_seconds()
        if remaining is not None:
            _require(_finite(remaining) and remaining > 0, 'COLLECTION_STEP_BUDGET')
            budget = min(budget, remaining)
        started = self.now()
        draft = dict(kind=step['kind'], passed=False, step=copy.deepcopy(step), status='started',
                     started_mono_ms=started * 1000, budget_seconds=budget)
        self.steps.append(draft)
        if hasattr(self.backend, 'last_motion'):
            self.backend.last_motion = None
        if hasattr(self.backend, 'last_dispatch'):
            self.backend.last_dispatch = None  # never attribute an older step's input to this one
        self._capture_attempts = []
        self._write_report()
        try:
            _require(budget > 0, 'BATCH_DEADLINE')
            _require(self.backend.foreground(), 'BATCH_FOREGROUND_LOST')
            observed = self._perform(step, budget)
            draft.update(observed)
            _require(observed.get('passed') is True, 'BATCH_STEP_FAILED')
            _require(self.backend.foreground(), 'BATCH_FOREGROUND_LOST')
            draft['status'] = 'finished'
        except BaseException as error:
            lost = self._window_lost(error)
            self._failed = True
            draft.update(passed=False, status='failed', error=lost or str(error) or type(error).__name__)
            # A partly sent input (one event of a pair, a release cleanup)
            # must stay in the record even though the step failed.
            dispatch = getattr(self.backend, 'last_dispatch', None)
            if isinstance(dispatch, dict) and 'input_dispatch' not in draft:
                draft['input_dispatch'] = copy.deepcopy(dispatch)
            self.report['error'] = draft['error']
            if lost:
                raise RuntimeError(lost) from error
            raise
        finally:
            motion = getattr(self.backend, 'last_motion', None)
            if motion is not None:
                draft['motion'] = copy.deepcopy(motion)
                draft.setdefault('timings', {})['motion_ms'] = motion.get('elapsed_ms')
            draft.setdefault('timings', {})['total_ms'] = round((self.now() - started) * 1000, 3)
            draft['finished_mono_ms'] = self.now() * 1000
            self._write_report()
            if self.step_listener is not None:
                try:
                    self.step_listener(len(self.steps), draft)
                except Exception:
                    pass
        return draft

    def settle_receipts(self):
        """Complete any in-flight pixel receipt now (journal + listener)."""
        if self._receipt_job is None:
            return None
        _require(self._entered and not self._finished and not self._failed, 'COLLECTION_SESSION_NOT_ACTIVE')
        try:
            outcome = self._settle_receipt()
        except BaseException as error:
            self._failed = True
            self.report['error'] = self.report.get('error') or str(error) or type(error).__name__
            self._write_report()
            raise
        self._write_report()
        return outcome

    def _restore_basis(self, basis):
        self.previous = basis['packet']
        self.previous_received = basis['received']
        self.previous_age_upper_ms = basis['age']

    def _pixel_receipt_command(self, reference):
        command = [str(self.root / 'dist/RelinkStudio/RelinkStudio.exe'), '--live-capture-check',
                   '--focus-policy', 'caller-owned']
        command += self._identity_arguments()
        return command + ['--frames', '1', '--collection-pixel-receipt', '--collection-receipt-reference',
                          json.dumps(reference, ensure_ascii=False, separators=(',', ':'), allow_nan=False),
                          '--reuse-capture-resources', '--collection-ready-stream']

    def _start_pixel_receipt(self, basis):
        """Request the pixel receipt right after the dispatched star input.

        Pipelined: return at once. The next card/scroll lease may be planned
        from the candidate's selection packet (``basis``), but its input is
        dispatched only after _settle_receipt confirmed the gold star on the
        same card AND an unchanged visible list. Otherwise settle in place.
        """
        _require(self._receipt_job is None and self.pending is not None, 'COLLECTION_RECEIPT_PENDING_REQUIRED')
        reference = receipt_reference(self.pending['candidate'])
        command = self._pixel_receipt_command(reference)
        timeout = max(.5, min(3.0, self.remaining_seconds()))
        self._receipt_job = PixelReceiptJob(self.backend.capture, command, timeout,
            pending_key=self.pending['key'], basis=basis, started=self.now())
        if self.pipeline_receipts:
            self._restore_basis(basis)
            return dict(mode='pipelined_pixel', pending=True)
        return self._settle_receipt()

    def _record_settlement(self, settlement):
        if self.steps:
            self.steps[-1].setdefault('receipt_settlements', []).append(settlement)

    def _settle_receipt(self):
        job = self._receipt_job
        if job is None:
            return None
        self._receipt_job = None
        _require(self.pending is not None and self.pending['key'] == job.pending_key,
                 'COLLECTION_RECEIPT_PENDING_REQUIRED')
        wait_started = self.now()
        job.thread.join(job.timeout + 2)
        _require(job.done.is_set(), 'COLLECTION_PIXEL_RECEIPT_TIMEOUT')
        settlement = dict(kind='pixel_receipt', command=job.command, attempt_key=job.pending_key,
                          started_mono_ms=job.started * 1000, join_wait_ms=round((self.now() - wait_started) * 1000, 3),
                          request_roundtrip_ms=(round((job.returned_monotonic - job.started) * 1000, 3)
                                                if self.now is time.monotonic and job.returned_monotonic else None))
        before = self.pending['candidate']
        validated = packet = None
        if job.error is not None:
            settlement['error'] = str(job.error) or type(job.error).__name__
            self._record_settlement(settlement)
            # A failed/poisoned capture worker cannot provide a fallback read.
            raise RuntimeError('COLLECTION_PIXEL_RECEIPT_TRANSPORT:' + settlement['error'])
        settlement['exit_status'] = job.process.returncode
        try:
            packet = json.loads(job.process.stdout)
            settlement['result'] = packet
            validated = validate_pixel_receipt(packet, before, job.basis['packet'])
        except (ValueError, KeyError, TypeError) as error:
            settlement['error'] = str(error)
        if validated is not None:
            self.journal.update(job.pending_key, 'confirmed', receipt=pixel_receipt_record(before, validated),
                                receipt_kind='pixel_same_card_white_to_gold', record=str(self.report_path))
            self.pending = None
            outcome = dict(mode='pixel', key=job.pending_key, layout_unchanged=validated['layout_unchanged'],
                           frame_id=validated['frame_id'], frame_sha256=validated['frame_sha256'],
                           scope=validated['scope'])
            if validated['layout_unchanged']:
                self._restore_basis(job.basis)
            else:
                # The star is confirmed; a complete current observation is
                # required before any further input.
                self.previous = {}
        else:
            outcome = self._full_receipt_fallback(job)
        settlement['outcome'] = outcome
        self._record_settlement(settlement)
        self.receipt_settlements.append(dict(outcome, join_wait_ms=settlement['join_wait_ms']))
        if self.receipt_listener is not None:
            self.receipt_listener(dict(outcome, candidate=copy.deepcopy(before)))
        return outcome

    def _recheck_layout(self, basis):
        """A new complete listing observation equivalent to ``basis``."""
        _require(self.pending is None, 'COLLECTION_PENDING_RECONCILIATION')
        saved_geometry = self.pending_geometry
        self.pending_geometry = None
        try:
            # Its own small budget: a transient first frame (page not yet
            # classified while the favorite toast animates: 5 of 9 recorded
            # rechecks) must not leave a single read for the layout itself.
            observed = self._capture(dict(kind='capture', expected_page='skin_listings', collection_observation=True,
                                          collection_layout=True, attempts=4), max(1, self.remaining_seconds()))
        finally:
            self.pending_geometry = saved_geometry
        mode = 'layout_recheck'
        if observed.get('passed') is not True:
            # This read proves list geometry only; no price from it is used.
            # The selected card's price below the unchanged .99 gate (hotkey
            # run 2026-10-08 22:27, identical layout) or an empty condition
            # read leaves the geometry proven by the same validated frame:
            # clean native exit, no geometry error, same-frame selected card.
            # Any other failure stops.
            layout, reason = (None, None)
            if (observed.get('exit_status') == 0 and not self.previous.get('collection_geometry_error')
                    and _selected_card_same_frame(self.previous)):
                layout, reason = _local_field_unread_observation(self.previous)
            if layout is None:
                self.receipt_settlements.append(dict(mode=mode, layout_unchanged=False,
                                                     capture_error=self.previous.get('local_title_error')
                                                     or self.previous.get('page_error') or self.previous.get('error')))
                return False
            mode = ('layout_recheck_geometry_only_price_unread' if reason == 'local_price_confidence_pending'
                    else 'layout_recheck_geometry_only_condition_unread')
        try:
            same = layouts_equivalent(observe_layout(basis['packet']), observe_layout(self.previous))
        except (ValueError, KeyError, TypeError):
            same = False
        record = dict(mode=mode, layout_unchanged=same)
        if mode != 'layout_recheck':
            record['price_accepted'] = False
        self.receipt_settlements.append(record)
        return same

    def _full_receipt_fallback(self, job):
        """Original complete receipt observation of the same dispatched item."""
        saved_geometry = self.pending_geometry
        self.pending_geometry = None
        try:
            observed = self._capture(dict(kind='capture', expected_page='skin_listings', collection_layout=True,
                expect_collection_added=True, receipt_if_pending=True), max(1, self.remaining_seconds()))
        finally:
            self.pending_geometry = saved_geometry
        _require(observed.get('passed') is True and self.pending is None, 'COLLECTION_RECEIPT_UNCONFIRMED')
        unchanged = False
        if self.previous.get('collection_geometry_scope') != 'receipt_only':
            try:
                unchanged = layouts_equivalent(observe_layout(job.basis['packet']), observe_layout(self.previous))
            except (ValueError, KeyError, TypeError):
                unchanged = False
        projection = self.previous.get('collection_observation', {})
        return dict(mode='full_fallback', key=job.pending_key, layout_unchanged=unchanged,
                    frame_id=projection.get('frame_id'), frame_sha256=projection.get('frame_sha256'),
                    scope=self.previous.get('collection_geometry_scope', 'visible_layout'))

    def finish(self, error=None):
        if self._finished:
            return self.report
        if self._receipt_job is not None and self._entered:
            try:
                self._settle_receipt()
            except BaseException as settle_error:
                self._failed = True
                self.report['error'] = self.report.get('error') or str(settle_error) or type(settle_error).__name__
        self._finished = True
        if error is not None:
            self._failed = True
            self.report['error'] = self.report.get('error') or str(error) or type(error).__name__
        if self.pending is not None and not self.report.get('error'):
            self._failed = True
            self.report['error'] = 'COLLECTION_PENDING_RECONCILIATION'
        if self.pending_geometry is not None and not self.report.get('error'):
            self._failed = True
            self.report['error'] = 'COLLECTION_GEOMETRY_REOBSERVATION_REQUIRED'
        restored = False
        if self._entered and self.backend is not None:
            self.report['leave_calls'] += 1
            try:
                restored = bool(self.backend.leave())
            except BaseException as restore_error:
                self.report['restore_error'] = str(restore_error) or type(restore_error).__name__
            try:
                self.report.update(self.backend.metadata())
                if self.report.get('capture_worker_close_error'):
                    self._failed = True
                    self.report['error'] = self.report.get('error') or 'COLLECTION_CAPTURE_WORKER_CLOSE'
            except Exception as metadata_error:
                self.report['metadata_error'] = str(metadata_error)
        self.report.update(passed=self._entered and not self._failed and restored,
                           ide_restored=restored, timestamp=datetime.datetime.now().astimezone().isoformat(),
                           runner_command=[sys.executable, *sys.argv], session_finished=True)
        self.report['exit_status'] = 0 if self.report['passed'] else 1
        self._write_report()
        return self.report

    def preview_jpeg(self):
        if not self.last_preview:
            return None
        from PIL import Image
        image = Image.open(io.BytesIO(base64.b64decode(self.last_preview)))
        output = io.BytesIO()
        image.save(output, format='JPEG', quality=68)
        return base64.b64encode(output.getvalue()).decode('ascii')
