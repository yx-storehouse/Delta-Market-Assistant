"""Unreadable selected-card field: fresh reads only, through the real _capture path.

Recorded frames (tests/fixtures): the 2026-10-08 22:45 selection read whose
"成色S" came back as zero words, and the 22:27 receipt recheck whose price
read below .99 on an unchanged list. FakeBackend only; no game, no input.
"""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import collection_live_session as live
import test_collection_live_session as fx

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures'
ADDED = ('coordinator_observation', 'filter_expectation_passed', 'collection_layout_passed', 'collection_geometry_error',
         'collection_geometry_rebind', 'collection_selection_readiness', 'collection_receipt_passed',
         'collection_receipt_error', 'collection_receipt_geometry_passed', 'collection_receipt_geometry_error',
         'collection_geometry_scope', 'collection_price_readiness', 'collection_state_waiting', 'collection_receipt_kind')


def raw(result):
    """The native/backend packet as the capture worker returns it."""
    packet = copy.deepcopy(result)
    for key in ADDED:
        packet.pop(key, None)
    return packet


def renew(packet, tag):
    """Same pixels' evidence under a new frame identity (a later frame)."""
    frame_id = packet['collection_observation']['frame_id']
    digest = packet['collection_observation']['frame_sha256']
    text = json.dumps(packet, ensure_ascii=False)
    text = text.replace(frame_id, 'dxgi:00000000-0000-0000-0000-' + tag.rjust(12, '0')).replace(digest, (tag * 64)[:64])
    return json.loads(text)


def readable(packet, tag):
    """A later frame in which the condition label was read normally."""
    good = renew(packet, tag)
    good['capture_passed'] = True
    good.pop('local_title_error', None)
    region = next(r for r in good['collection_observation']['regions'] if r.get('kind') == 'card_fields')
    condition = dict(text='成色S', x=127, y=1133, width=60, height=20)
    price = [dict(text='300', score=.99998, x=937.0, y=1138.0, width=39.0, height=17.0)]
    region.update(ok=True, error='', words=[condition] + price, price_provider='RapidOCR3.9.2/PP-OCRv6-local-CPU')
    local = good['local_price_ocr']
    local.update(ok=True, words=price)
    local.pop('error', None)
    local['native_observation']['words'] = [condition]
    for attempt in local['native_observation']['field_attempts']:
        if attempt['field'] == 'condition_bounds':
            attempt['word_count'] = 1
    return good


class FieldUnreadCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        recorded = json.loads((FIXTURES / 'hotkey_condition_unread.json').read_text('utf-8'))
        cls.unread = raw(recorded['result'])
        cls.geometry = recorded['pending_geometry']

    def capture(self, replies, *, selection=True):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = fx.Clock()
        backend = fx.FakeBackend(clock, [copy.deepcopy(reply) for reply in replies])
        backend.reuse_capture = backend.ready_stream = True
        session = live.ForegroundSession('artifacts/field-unread/run.json', root=Path(temporary.name), backend=backend,
                                         now=clock.now, wait=clock.wait, fast_settle=True, numeric_price=True)
        session.enter()
        self.addCleanup(session.finish)
        session.pending = None
        if selection:
            session.pending_geometry = copy.deepcopy(self.geometry)
            step = dict(kind='capture', expected_page='skin_listings', collection_observation=True,
                        collection_layout=True, selected_geometry_required=True)
        else:
            session.pending_geometry, session.previous = None, {}
            step = dict(kind='capture', expected_page='skin_listings', collection_observation=True, collection_layout=True)
        session.steps.append(dict(kind='capture', step=step))
        session._capture_attempts = []
        return session, session._capture(step, 30)

    def test_selection_read_retries_and_a_readable_frame_rebinds_normally(self):
        session, observed = self.capture([self.unread, readable(self.unread, 'a')])
        self.assertTrue(observed['passed'])
        self.assertEqual(observed['attempt_count'], 2)
        first = observed['attempts'][0]
        self.assertTrue(first['collection_price_retryable'])
        self.assertEqual(first['result']['collection_price_readiness']['reason'], 'local_condition_unread_pending')
        self.assertIsNone(session.pending_geometry)
        self.assertIsNotNone(session.selected_lease)
        self.assertEqual(session.clicks, 0)

    def test_selection_read_that_stays_unread_or_repeats_its_frame_stops(self):
        session, observed = self.capture([self.unread, renew(self.unread, '1'), renew(self.unread, '2')])
        self.assertFalse(observed['passed'])
        self.assertEqual(observed['attempt_count'], 3)
        self.assertIsNotNone(session.pending_geometry)
        self.assertIsNone(session.selected_lease)
        session, observed = self.capture([self.unread, self.unread])
        self.assertFalse(observed['passed'])
        self.assertEqual(observed['attempt_count'], 2)

    def test_rule_start_listing_read_also_retries(self):
        session, observed = self.capture([self.unread, readable(self.unread, 'b')], selection=False)
        self.assertTrue(observed['passed'])
        self.assertEqual(observed['attempt_count'], 2)
        self.assertEqual(observed['attempts'][0]['result']['collection_price_readiness']['reason'],
                         'local_condition_unread_listing_pending')
        session, observed = self.capture([self.unread, renew(self.unread, '3'), renew(self.unread, '4')],
                                         selection=False)
        self.assertFalse(observed['passed'])
        self.assertEqual(observed['attempt_count'], 3)

    def test_a_stray_condition_word_is_never_retried(self):
        stray = copy.deepcopy(self.unread)
        stray['local_price_ocr']['native_observation']['words'] = [dict(text='成色A', x=130, y=1130, width=40, height=20)]
        for selection in (True, False):
            session, observed = self.capture([stray, readable(self.unread, 'c')], selection=selection)
            self.assertFalse(observed['passed'])
            self.assertEqual(observed['attempt_count'], 1)


class PriceClippedCaptureTests(FieldUnreadCaptureTests):
    """F2 cycle 2026-10-09 (20261009-212123 step 312): the bottom card's
    price crop read as clipped ink once right after its selection."""

    @classmethod
    def setUpClass(cls):
        recorded = json.loads((FIXTURES / 'hotkey_price_clipped.json').read_text('utf-8'))
        cls.unread = raw(recorded['result'])
        cls.geometry = recorded['pending_geometry']

    def test_selection_read_retries_and_a_readable_frame_rebinds_normally(self):
        session, observed = self.capture([self.unread, readable(self.unread, 'a')])
        self.assertTrue(observed['passed'], observed['result'].get('collection_geometry_error'))
        self.assertEqual(observed['attempt_count'], 2)
        first = observed['attempts'][0]
        self.assertEqual(first['result']['collection_price_readiness']['reason'], 'local_price_clipped_pending')
        self.assertIsNone(session.pending_geometry)
        self.assertEqual(session.clicks, 0)

    def test_rule_start_listing_read_also_retries(self):
        session, observed = self.capture([self.unread, readable(self.unread, 'b')], selection=False)
        self.assertTrue(observed['passed'])
        self.assertEqual(observed['attempts'][0]['result']['collection_price_readiness']['reason'],
                         'local_price_clipped_listing_pending')

    def test_a_stray_condition_word_is_never_retried(self):
        # Another native price error, or an unproven condition read, is not this case.
        for change in ('native_error', 'condition'):
            other = copy.deepcopy(self.unread)
            native = other['local_price_ocr']['native_observation']
            if change == 'native_error':
                native['error'] = 'E_NUMERIC_TOO_WIDE'
            else:
                next(a for a in native['field_attempts'] if a['field'] == 'condition_bounds')['ok'] = False
            for selection in (True, False):
                session, observed = self.capture([other, readable(self.unread, 'c')], selection=selection)
                self.assertFalse(observed['passed'])
                self.assertEqual(observed['attempt_count'], 1)


class ReceiptRecheckCaptureTests(unittest.TestCase):
    """_recheck_layout through the real _capture/stabilize path."""

    @classmethod
    def setUpClass(cls):
        recorded = json.loads((FIXTURES / 'hotkey_recheck_price_unread.json').read_text('utf-8'))
        cls.basis, cls.recheck = recorded['basis'], raw(recorded['recheck'])

    def session(self, replies):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = fx.Clock()
        backend = fx.FakeBackend(clock, [copy.deepcopy(reply) for reply in replies])
        backend.reuse_capture = backend.ready_stream = True
        session = live.ForegroundSession('artifacts/recheck-e2e/run.json', root=Path(temporary.name), backend=backend,
                                         now=clock.now, wait=clock.wait, fast_settle=True, numeric_price=True)
        session.enter()
        self.addCleanup(session.finish)
        session.pending = session.pending_geometry = None
        session.steps.append(dict(kind='select_visible_card', step=dict(kind='select_visible_card')))
        session._capture_attempts = []
        return session

    def test_recorded_price_unread_recheck_proves_the_unchanged_list(self):
        # The plain listing read first asks for fresh frames; when every one
        # stays unread, the last validated frame still proves the geometry.
        session = self.session([self.recheck, renew(self.recheck, '1'), renew(self.recheck, '2'), renew(self.recheck, '3')])
        self.assertTrue(session._recheck_layout(dict(packet=copy.deepcopy(self.basis))))
        self.assertEqual(len(session._capture_attempts), 4)
        self.assertTrue(session._capture_attempts[0]['collection_price_retryable'])
        self.assertEqual(session.receipt_settlements[-1]['mode'], 'layout_recheck_geometry_only_price_unread')

    def test_a_transient_first_frame_does_not_use_up_the_recheck(self):
        # 5 of 9 recorded rechecks began with an unclassified page while the
        # favorite toast animated; the layout itself still gets three reads.
        unknown = dict(copy.deepcopy(self.recheck), _exit=1, page_match_passed=False,
                       page_error='E_DIAGNOSTIC_PAGE_MISMATCH')
        unknown['startup_page'] = dict(unknown.get('startup_page') or {}, page='unknown')
        session = self.session([unknown, self.recheck, renew(self.recheck, '1'), renew(self.recheck, '2')])
        self.assertTrue(session._recheck_layout(dict(packet=copy.deepcopy(self.basis))))
        self.assertEqual(len(session._capture_attempts), 4)

    def test_a_failed_native_exit_or_moved_list_still_stops(self):
        crashed = dict(copy.deepcopy(self.recheck), _exit=1)
        session = self.session([crashed])
        self.assertFalse(session._recheck_layout(dict(packet=copy.deepcopy(self.basis))))
        moved = copy.deepcopy(self.recheck)
        for card in moved['collection_layout']['cards']:
            card['bounds'][1] -= 87
        session = self.session([moved])
        self.assertFalse(session._recheck_layout(dict(packet=copy.deepcopy(self.basis))))


if __name__ == '__main__':
    unittest.main()
