"""Offline audit regressions; only isolated temporary fixtures are written."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

path = Path(__file__).resolve().parents[2] / 'docs/business_rebuild/scripts/audit_collection_timing.py'
spec = importlib.util.spec_from_file_location('collection_timing_audit', path)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class TimingAuditTests(unittest.TestCase):
    def test_nearest_rank(self):
        result = audit.stats(list(range(1, 21)))
        self.assertEqual(result, dict(n=20, min=1, median=10.5, p95=19, max=20))

    def test_empty_is_unknown_not_zero(self):
        self.assertEqual(audit.stats([]), dict(n=0, min=None, median=None, p95=None, max=None))

    def test_one_value(self):
        self.assertEqual(audit.stats([223])['p95'], 223)

    def test_invalid_measurements(self):
        for value in (True, -1, float('inf'), float('nan'), '12'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                audit.stats([value])

    def test_final_attempt_not_duplicated(self):
        first, second = {'result': {'id': 1}}, {'result': {'id': 2}}
        result = audit.capture_attempts({'kind': 'capture', 'result': second['result'],
                                         'attempts': [first, second]})
        self.assertEqual(result, [first, second])

    def test_non_capture_excluded(self):
        self.assertEqual(audit.capture_attempts({'kind': 'click', 'result': {}}), [])

    def test_skipped_receipt_is_not_an_observation(self):
        self.assertEqual(audit.capture_attempts({'kind': 'capture', 'passed': True,
            'skipped': True, 'reason': 'price_limit_segment_finished'}), [])

    def test_old_flat_record(self):
        step = {'kind': 'capture', 'result': {'id': 1}}
        self.assertEqual(audit.capture_attempts(step), [step])

    def test_foreground_excludes_other_windows(self):
        record = {'identities': {'target_hwnd': 2}, 'foreground_transitions': [
            {'hwnd': 1, 'monotonic_ms': 50}, {'hwnd': 2, 'monotonic_ms': 100},
            {'hwnd': 1, 'monotonic_ms': 200}, {'hwnd': 2, 'monotonic_ms': 300},
            {'hwnd': 1, 'monotonic_ms': 350}]}
        self.assertEqual(audit.foreground_ms(record), 150)

    def test_open_foreground_interval_unknown(self):
        self.assertIsNone(audit.foreground_ms({'identities': {'target_hwnd': 2},
            'foreground_transitions': [{'hwnd': 1, 'monotonic_ms': 50}, {'hwnd': 2, 'monotonic_ms': 100}]}))

    def test_no_foreground_measurement_unknown(self):
        self.assertIsNone(audit.foreground_ms({}))

    def test_clock_order(self):
        with self.assertRaises(ValueError):
            audit.foreground_ms({'identities': {'target_hwnd': 2}, 'foreground_transitions': [
                {'hwnd': 2, 'monotonic_ms': 200}, {'hwnd': 1, 'monotonic_ms': 100}]})

    def test_windows_record_path_comparison(self):
        self.assertEqual(audit.record_key('C:\\Example\\live_1.json'),
                         audit.record_key('c:/example/live_1.json'))

    def test_missing_evidence_is_not_zero_confirmations(self):
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(ValueError):
            audit.summarize(Path(directory), Path(directory) / 'missing_journal')

    def test_full_summary_dedup_skip_and_journal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            series, journal = root / 'series', root / 'journal'
            series.mkdir()
            journal.mkdir()
            path = series / 'live_1.json'
            result = {'frames': [{'capture_ms': 200}], 'ocr': {'frame_age_at_result_ms': 3000}}
            data = {'passed': True, 'identities': {'target_hwnd': 2}, 'foreground_transitions': [
                {'hwnd': 2, 'monotonic_ms': 10}, {'hwnd': 1, 'monotonic_ms': 110}], 'steps': [
                    {'kind': 'capture', 'passed': True, 'result': result,
                     'attempts': [{'passed': True, 'result': result}]},
                    {'kind': 'collect_selected', 'collection_attempt': {'eligible': True}},
                    {'kind': 'capture', 'passed': True, 'skipped': True}]}
            path.write_text(json.dumps(data), encoding='utf-8')
            jpath = journal / 'one.json'
            jpath.write_text(json.dumps({'key': 'one', 'status': 'confirmed',
                'evidence': {'record': str(path.resolve())}}), encoding='utf-8')
            report = audit.summarize(series, journal)
            self.assertEqual(report['capture_ms']['n'], 1)
            self.assertEqual(report['batches'][0]['observations'], 1)
            self.assertEqual(report['batches'][0]['confirmed_new_collections'], 1)
            self.assertEqual(report['source_sha256'][str(jpath)], audit.digest(jpath))
            self.assertFalse(report['game_input_sent'])


if __name__ == '__main__':
    unittest.main()
