"""Offline multi-segment collection; real coordinator/geometry, no OS input."""
import copy
import hashlib
import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from run_collection_trial import CollectionTrial, validate_resume, write_progress, main
from test_collection_all_rules_review import GeometrySession, rule, snapshot
from test_collection_trial import listing


HASH = 'b' * 64


class ResumeTrial(CollectionTrial):
    def __init__(self, session, rows, *, resume=None, checkpoint=None, completed_limit=None):
        super().__init__(session, snapshot(rows), 'artifacts/resume/input.json', snapshot_sha256=HASH,
                         resume=resume, checkpoint=checkpoint)
        self.opened = []
        self.startup_calls = 0
        self.completed_limit = completed_limit
        self.fail_open_row = None

    def startup(self):
        self.startup_calls += 1

    def open_product(self, current):
        self.opened.append(current['row_index'])
        if current['row_index'] == self.fail_open_row:
            raise RuntimeError('REVIEW_PRODUCT_NOT_OBSERVED')

    def condition(self, current):
        self.session.reset_segment()
        self.session.begin(current)

    def scan_row(self, current):
        super().scan_row(current)
        if self.completed_limit is not None and len(self.summary['rows']) >= self.completed_limit:
            self.session.remaining = 11


def run_segment(trial):
    try:
        trial.run()
    except RuntimeError as error:
        trial.stopped(error)
    return copy.deepcopy(trial.summary)


def first_completed_summary():
    rows = [rule(0), rule(1)]
    trial = ResumeTrial(GeometrySession({0: [dict(price=601)]}), rows, completed_limit=1)
    summary = run_segment(trial)
    assert summary['completed_row_indices'] == [0]
    return rows, summary


class ResumeTests(unittest.TestCase):
    def test_twenty_one_enabled_rules_nine_products_continue_across_three_segments(self):
        enabled = list(range(0, 42, 2))
        rows = [rule(i, f'configured-product-{(i // 2) % 9}', i in enabled) for i in range(50)]
        previous = None
        opened = []
        total_new = 0
        for segment, end in enumerate((7, 14, 21), 1):
            start = 0 if previous is None else len(previous['rows'])
            values = {i: [dict(price=230), dict(price=601)] for i in enabled[start:end]}
            session = GeometrySession(values)
            trial = ResumeTrial(session, rows, resume=previous, completed_limit=end)
            previous = run_segment(trial)
            opened.extend(trial.opened)
            total_new += previous['confirmed_new']
            self.assertEqual(trial.opened, enabled[start:end])
            self.assertEqual(previous['segment_index'], segment)
            self.assertEqual(previous['completed_row_indices'], enabled[:end])
            self.assertEqual(previous['next_row_index'], enabled[end] if end < 21 else None)
            self.assertEqual(previous['confirmed_new'], 7)
            self.assertEqual(previous['cycle_totals']['confirmed_new'], end)
            self.assertEqual(previous['task_file_fully_completed'], end == 21)
            self.assertEqual(previous['status'], 'completed' if end == 21 else 'segment_budget_exhausted')
            self.assertFalse(previous['exhaustive_market_scan'])
            self.assertFalse(previous['purchase_phase_started'])
        self.assertEqual(opened, enabled)
        self.assertEqual(total_new, 21)
        self.assertEqual(len({r['product_name'] for r in rows if r['enabled']}), 9)
        self.assertEqual(previous['disabled_rows_skipped'], 29)

    def test_unfinished_row_reobserves_and_preserves_star_instead_of_skipping(self):
        rows = [rule(0), rule(1)]
        first = ResumeTrial(GeometrySession({0: [dict(price=230)]}), rows)
        first.emit = lambda event: setattr(first.session, 'remaining', 11) if event['event'] == 'favorite_confirmed' else None
        partial = run_segment(first)
        self.assertEqual(partial['rows'], [])
        self.assertEqual(partial['next_row_index'], 0)
        self.assertEqual(partial['confirmed_new'], 1)
        second = ResumeTrial(GeometrySession({0: [dict(price=230, gold=True), dict(price=601)],
                                              1: [dict(price=601)]}), rows, resume=partial)
        second.run()
        self.assertEqual(second.opened, [0, 1])
        self.assertEqual(second.startup_calls, 1)
        self.assertEqual(second.summary['already_favorited'], 1)
        self.assertEqual(second.summary['confirmed_new'], 0)
        self.assertEqual(second.summary['cycle_totals']['confirmed_new'], 1)
        self.assertEqual(second.session.collected_rows, [])
        self.assertTrue(second.summary['task_file_fully_completed'])

    def test_completed_boundary_is_persisted_before_next_row_failure(self):
        snapshots = []
        rows = [rule(0), rule(1), rule(2)]
        first = ResumeTrial(GeometrySession({0: [dict(price=601)]}), rows,
                            checkpoint=lambda value: snapshots.append(value))
        first.fail_open_row = 1
        summary = run_segment(first)
        self.assertTrue(any(s['completed_row_indices'] == [0] and s['current_row_index'] is None for s in snapshots))
        self.assertEqual(summary['status'], 'blocked')
        self.assertEqual(summary['next_row_index'], 1)
        second = ResumeTrial(GeometrySession({1: [dict(price=601)], 2: [dict(price=601)]}), rows, resume=summary)
        second.run()
        self.assertEqual(second.opened, [1, 2])
        self.assertEqual([r['completion_segment_index'] for r in second.summary['rows']], [1, 2, 2])

    def test_strict_byte_snapshot_hash_rejects_reformatted_snapshot(self):
        rows, prior = first_completed_summary()
        with self.assertRaisesRegex(ValueError, 'SNAPSHOT_OR_RULES_CHANGED'):
            validate_resume(snapshot(rows), 'c' * 64, prior)

    def test_rule_price_condition_title_or_enabled_change_rejects_resume(self):
        rows, prior = first_completed_summary()
        for field, value in (('price_max', '601'), ('max_wear', '4'), ('condition_label', '成色A'),
                              ('product_name', 'AUG突击步枪-另一个'), ('enabled', False)):
            changed = copy.deepcopy(rows)
            changed[1][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'SNAPSHOT_OR_RULES_CHANGED'):
                validate_resume(snapshot(changed), HASH, prior)

    def test_source_or_config_hash_mismatch_rejects_resume(self):
        rows, prior = first_completed_summary()
        for name in ('source_sha256', 'config_sha256'):
            changed = copy.deepcopy(prior)
            changed[name] = 'c' * 64
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'SNAPSHOT_OR_RULES_CHANGED'):
                validate_resume(snapshot(rows), HASH, changed)

    def test_completed_rows_must_be_contiguous_and_unchanged(self):
        rows, prior = first_completed_summary()
        for mutation in ('duplicate', 'wrong_row', 'wrong_fingerprint', 'wrong_cursor', 'false_complete'):
            changed = copy.deepcopy(prior)
            if mutation == 'duplicate': changed['rows'].append(copy.deepcopy(changed['rows'][0]))
            elif mutation == 'wrong_row': changed['rows'][0]['row_index'] = 1
            elif mutation == 'wrong_fingerprint': changed['rows'][0]['rule_fingerprint'] = '0' * 64
            elif mutation == 'wrong_cursor': changed['next_row_index'] = None
            else: changed['task_file_fully_completed'] = True
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, 'COLLECTION_RESUME_'):
                validate_resume(snapshot(rows), HASH, changed)

    def test_boundary_needs_above_price_and_own_frame_evidence(self):
        rows, prior = first_completed_summary()
        for key, value in (('price', '600'), ('price', 'NaN'), ('price', '30'), ('wear', 'NaN'),
                           ('product', 'other'), ('condition', '成色C'), ('row_index', 1),
                           ('source_frame_id', ''), ('source_frame_sha256', None)):
            changed = copy.deepcopy(prior)
            changed['rows'][0]['boundary_candidate'][key] = value
            with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, 'BOUNDARY_EVIDENCE'):
                validate_resume(snapshot(rows), HASH, changed)

    def test_incomplete_timer_record_is_never_a_row_completion(self):
        rows, prior = first_completed_summary()
        prior['rows'][0]['status'] = 'time_budget_complete'
        with self.assertRaisesRegex(ValueError, 'BOUNDARY_EVIDENCE'):
            validate_resume(snapshot(rows), HASH, prior)

    def test_legacy_summary_without_fingerprints_is_not_resumable(self):
        rows, prior = first_completed_summary()
        prior.pop('rule_fingerprints')
        with self.assertRaisesRegex(ValueError, 'SNAPSHOT_OR_RULES_CHANGED'):
            validate_resume(snapshot(rows), HASH, prior)

    def test_completed_cycle_resume_does_not_observe_or_dispatch_again(self):
        rows = [rule(0)]
        first = ResumeTrial(GeometrySession({0: [dict(price=601)]}), rows)
        first.run()
        second = ResumeTrial(GeometrySession({}), rows, resume=first.summary)
        second.run()
        self.assertEqual(second.startup_calls, 0)
        self.assertEqual(second.opened, [])
        self.assertEqual(second.session.steps, [])
        self.assertTrue(second.summary['task_file_fully_completed'])
        self.assertEqual(second.summary['cycle_totals'], first.summary['cycle_totals'])

    def test_unresolved_journal_blocks_resume_before_navigation(self):
        rows, prior = first_completed_summary()
        with tempfile.TemporaryDirectory() as directory:
            journal = Path(directory)
            (journal / 'pending.json').write_text(json.dumps(dict(status='dispatched')), encoding='utf-8')
            session = GeometrySession({1: [dict(price=601)]})
            session.journal.directory = journal
            second = ResumeTrial(session, rows, resume=prior)
            with self.assertRaisesRegex(RuntimeError, 'PENDING_RECONCILIATION'):
                second.run()
            self.assertEqual(second.startup_calls, 0)
            self.assertEqual(session.steps, [])

    def test_snapshot_object_is_copied_before_external_edit(self):
        data = snapshot([rule(0)])
        trial = CollectionTrial(GeometrySession({}), data, 'artifacts/test.json', snapshot_sha256=HASH)
        data['rows'][0]['price_max'] = '9999'
        self.assertEqual(trial.snapshot['rows'][0]['price_max'], '600')

    def test_atomic_progress_reopen_and_failed_replace_preserve_old_checkpoint(self):
        rows, prior = first_completed_summary()
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'summary.json'
            write_progress(target, prior)
            before = target.read_bytes()
            self.assertEqual(json.loads(before), prior)
            with patch('run_collection_trial.os.replace', side_effect=OSError('injected replace failure')):
                with self.assertRaises(OSError): write_progress(target, dict(prior, status='running'))
            self.assertEqual(target.read_bytes(), before)

    def test_cli_resume_hash_rejected_before_output_or_session_creation(self):
        rows, prior = first_completed_summary()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / 'artifacts'
            artifacts.mkdir()
            source = artifacts / 'snapshot.json'
            source.write_text(json.dumps(snapshot(rows)), encoding='utf-8')
            resume = artifacts / 'prior.json'
            resume.write_text(json.dumps(prior), encoding='utf-8')
            target = artifacts / 'next'
            with patch('run_collection_trial.ROOT', root), patch('sys.argv', [
                    'run_collection_trial.py', '--snapshot', str(source), '--output', str(target), '--resume', str(resume)]):
                with self.assertRaisesRegex(ValueError, 'SNAPSHOT_OR_RULES_CHANGED'):
                    main()
            self.assertFalse(target.exists())

    def test_cli_budget_exit_two_then_resume_completes_without_rescanning(self):
        sessions = []
        rows = [rule(0), rule(1)]
        class CliSession(GeometrySession):
            def __init__(self, *args, **kwargs):
                self.numeric_price_option = kwargs.get('numeric_price')
                self.segment_number = len(sessions)
                super().__init__({self.segment_number: [dict(price=601)]})
                self.report = {}
                sessions.append(self)
            def __enter__(self): return self
            def __exit__(self, kind, value, traceback):
                self.report.update(ide_restored=True)
        class CliTrial(CollectionTrial):
            def startup(self): pass
            def open_product(self, current): pass
            def condition(self, current):
                self.session.reset_segment()
                self.session.begin(current)
            def scan_row(self, current):
                super().scan_row(current)
                if self.session.segment_number == 0:
                    self.session.remaining = 11
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / 'artifacts'
            artifacts.mkdir()
            source = artifacts / 'snapshot.json'
            source.write_text(json.dumps(snapshot(rows)), encoding='utf-8')
            raw = source.read_bytes()
            outputs = [artifacts / 'run_01', artifacts / 'run_02']
            with patch('run_collection_trial.ROOT', root), patch('run_collection_trial.CollectionTrial', CliTrial), \
                    patch('collection_live_session.ForegroundSession', CliSession):
                for index, expected_exit in enumerate((2, 0)):
                    arguments = ['run_collection_trial.py', '--snapshot', str(source), '--output', str(outputs[index]), '--seconds', '900']
                    if index:
                        arguments += ['--resume', str(outputs[index - 1] / 'summary.json')]
                        arguments += ['--numeric-price']
                    with patch('sys.argv', arguments), redirect_stdout(io.StringIO()):
                        self.assertEqual(main(), expected_exit)
            first = json.loads((outputs[0] / 'summary.json').read_text('utf-8'))
            final = json.loads((outputs[1] / 'summary.json').read_text('utf-8'))
            self.assertEqual(first['completed_row_indices'], [0])
            self.assertFalse(first['task_file_fully_completed'])
            self.assertEqual(final['completed_row_indices'], [0, 1])
            self.assertEqual(final['snapshot_sha256'], hashlib.sha256(raw).hexdigest())
            self.assertEqual(final['resume_from']['sha256'], hashlib.sha256((outputs[0] / 'summary.json').read_bytes()).hexdigest())
            self.assertEqual([s['row_index'] for s in sessions[1].steps if s['kind'] == 'collect_selected'], [1])
            self.assertTrue(final['task_file_fully_completed'])
            self.assertTrue(final['ide_restored'])
            self.assertIs(sessions[0].numeric_price_option, False)
            self.assertIs(sessions[1].numeric_price_option, True)


class ConditionContextTests(unittest.TestCase):
    def packet(self, title='P90冲锋枪-天命', *, overlay='none'):
        value = listing()
        value['startup_page']['overlay'] = overlay
        value['collection_observation']['regions'][0]['words'][0]['text'] = title
        value['collection_observation']['regions'].append(dict(kind='listing_controls', ok=True, truncated=False,
            words=[dict(text='不限公示期', x=527, y=255, width=105, height=20),
                   dict(text='默认排序', x=1153, y=253, width=83, height=20)]))
        value['collection_condition_filter'] = {key: dict(state='checked' if key == 'S' else 'unchecked')
                                               for key in ('all', 'S', 'A', 'B', 'C')}
        return value

    def setup_trial(self):
        current = rule(0, 'P90冲锋枪-天命')
        session = GeometrySession({})
        session.previous = self.packet()
        trial = CollectionTrial(session, snapshot([current]), 'artifacts/test.json', snapshot_sha256=HASH)
        return current, session, trial

    def test_missing_digit_in_title_stops_before_condition_click(self):
        current, session, trial = self.setup_trial()
        session.previous = self.packet(title='P9冲锋枪-天命')
        with self.assertRaisesRegex(RuntimeError, 'LISTING_TITLE_NOT_CONFIRMED'):
            trial.condition(current)
        self.assertEqual(session.steps, [])

    def test_missing_control_stops_before_condition_click(self):
        current, session, trial = self.setup_trial()
        session.previous['collection_observation']['regions'][-1]['words'].pop()
        with self.assertRaisesRegex(RuntimeError, 'LISTING_CONTROL_NOT_CONFIRMED'):
            trial.condition(current)
        self.assertEqual(session.steps, [])

    def test_close_condition_menu_requires_fresh_exact_title(self):
        current, session, trial = self.setup_trial()
        replies = [self.packet(overlay='listing_filter'), self.packet(title='P9冲锋枪-天命')]
        def perform(step):
            session.steps.append(copy.deepcopy(step))
            if step['kind'] == 'capture': session.previous = replies.pop(0)
            return dict(passed=True)
        session.perform = perform
        with self.assertRaisesRegex(RuntimeError, 'LISTING_TITLE_NOT_CONFIRMED'):
            trial.condition(current)
        self.assertEqual([s['kind'] for s in session.steps], ['click', 'capture', 'click', 'capture'])
        self.assertFalse(any(s['kind'] == 'collect_selected' for s in session.steps))

    def test_close_condition_menu_requires_fresh_control_labels(self):
        current, session, trial = self.setup_trial()
        after = self.packet()
        after['collection_observation']['regions'][-1]['words'][1]['text'] = '价格降序'
        replies = [self.packet(overlay='listing_filter'), after]
        def perform(step):
            session.steps.append(copy.deepcopy(step))
            if step['kind'] == 'capture': session.previous = replies.pop(0)
            return dict(passed=True)
        session.perform = perform
        with self.assertRaisesRegex(RuntimeError, 'LISTING_CONTROL_NOT_CONFIRMED'):
            trial.condition(current)
        self.assertFalse(any(s['kind'] == 'collect_selected' for s in session.steps))


if __name__ == '__main__':
    unittest.main()
