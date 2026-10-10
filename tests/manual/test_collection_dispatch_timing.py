"""SendInput call-boundary telemetry using inert ctypes structures/fake APIs."""
import ctypes
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from collection_live_session import WindowsBackend


class Mouse(ctypes.Structure):
    _fields_ = [('dwFlags', ctypes.c_ulong)]


class Data(ctypes.Structure):
    _fields_ = [('mi', Mouse)]


class Input(ctypes.Structure):
    _fields_ = [('data', Data)]


class DispatchTimingTests(unittest.TestCase):
    def backend(self, sent=2):
        backend = WindowsBackend.__new__(WindowsBackend)
        backend.c, backend.Input = ctypes, Input
        backend.last_dispatch = {'stale': True}
        events = []
        backend._position = lambda point, purpose: events.append(('move', point, purpose))
        backend.foreground = lambda: True
        backend._idle_input = lambda: True
        def dispatch(count, pair, size):
            events.append(('dispatch', count, size))
            if count == 2:
                self.assertEqual([x.data.mi.dwFlags for x in pair], [2, 4])
            return sent if count == 2 else 1
        backend.u = SimpleNamespace(SendInput=dispatch)
        return backend, events

    def test_api_boundaries_exclude_motion_and_keep_actual_return_count(self):
        for sent in (0, 1, 2):
            backend, events = self.backend(sent)
            with self.subTest(sent=sent), patch('collection_live_session.time.monotonic', side_effect=(12.5, 12.507)):
                result = backend.click([123, 456], before_dispatch=lambda: events.append(('guard',)))
            self.assertEqual(result, sent)
            self.assertEqual(events[:2], [('move', [123, 456], 'click'), ('guard',)])
            got = backend.last_dispatch
            self.assertEqual(got['started_mono_ms'], 12500)
            self.assertAlmostEqual(got['returned_mono_ms'], 12507)
            self.assertEqual(got['expected_events'], 2)
            self.assertEqual(got['returned_events'], sent)
            self.assertEqual(got['semantics'], 'API_call_boundary_not_hardware_delivery_timestamp')
            self.assertEqual(len([e for e in events if e[0] == 'dispatch']), 2 if sent == 1 else 1)

    def test_guard_failure_has_no_dispatch_or_stale_call_time(self):
        backend, events = self.backend()
        backend._idle_input = lambda: False
        with self.assertRaisesRegex(RuntimeError, 'INPUT_GUARD_CHANGED'):
            backend.click([0, 0])
        self.assertIsNone(backend.last_dispatch)
        self.assertFalse(any(e[0] == 'dispatch' for e in events))

    def test_motion_failure_clears_previous_dispatch(self):
        backend, events = self.backend()
        def interrupted(*args, **kwargs):
            raise RuntimeError('interrupted')
        backend._position = interrupted
        with self.assertRaisesRegex(RuntimeError, 'interrupted'):
            backend.click([0, 0])
        self.assertIsNone(backend.last_dispatch)
        self.assertEqual(events, [])


if __name__ == '__main__':
    unittest.main()


class PlannedTargetTests(DispatchTimingTests):
    """Review 2026-10-09: a long dispatch guard must not end in a click elsewhere."""

    def guarded(self, cursor_after_guard, check_after_guard=True, selection=False, scroll=False):
        backend, events = self.backend()
        state = dict(cursor=(400, 500), check=True)
        def position(point, purpose):
            events.append(('move', point, purpose))
            backend._planned_target = ((400, 500), lambda: state['check'])
        backend._position = position
        backend._cursor = lambda: state['cursor']
        def guard():
            events.append(('guard',))
            state.update(cursor=cursor_after_guard, check=check_after_guard)
        if scroll:
            backend.Input = Input
            return backend, events, lambda: backend.scroll([1, 2], -480, before_dispatch=guard)
        click = backend.fast_selection_click if selection else backend.click
        return backend, events, lambda: click([1, 2], before_dispatch=guard)

    def test_pointer_nudged_during_the_guard_sends_nothing(self):
        for selection in (False, True):
            backend, events, act = self.guarded((401, 500), selection=selection)
            with self.subTest(selection=selection), self.assertRaisesRegex(RuntimeError, 'CURSOR_MOVED_BEFORE_INPUT'):
                act()
            self.assertFalse(any(e[0] == 'dispatch' for e in events))

    def test_window_change_during_the_guard_sends_nothing(self):
        backend, events, act = self.guarded((400, 500), check_after_guard=False)
        with self.assertRaisesRegex(RuntimeError, 'TARGET_CHANGED_BEFORE_INPUT'):
            act()
        self.assertFalse(any(e[0] == 'dispatch' for e in events))

    def test_unchanged_target_is_clicked_once(self):
        backend, events, act = self.guarded((400, 500))
        self.assertEqual(act(), 2)
        self.assertEqual([e[0] for e in events], ['move', 'guard', 'dispatch'])
