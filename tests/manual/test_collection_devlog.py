"""Development log, failure records and run diagnosis; fakes and recorded frames only."""
import base64
import copy
import datetime
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import collection_devlog as devlog
import run_collection_hotkey as hotkey

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures'


def recorded_condition_failure():
    """The recorded 2026-10-08 22:45 selection capture, as a failed step record."""
    fixture = json.loads((FIXTURES / 'hotkey_condition_unread.json').read_text('utf-8'))
    return dict(kind='capture', status='failed', passed=False, error='BATCH_STEP_FAILED',
                step=dict(kind='capture', expected_page='skin_listings', selected_geometry_required=True),
                timings=dict(total_ms=281.0),
                capture_attempts=[dict(kind='capture', passed=False, exit_status=0, capture_roundtrip_ms=281.0,
                                       result=fixture['result'], collection_price_retryable=False)])


def tiny_png():
    from PIL import Image
    stream = io.BytesIO()
    Image.new('RGB', (40, 20), (200, 200, 200)).save(stream, format='PNG')
    return base64.b64encode(stream.getvalue()).decode('ascii')


class DevLogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.now = datetime.datetime(2026, 10, 9, 1, 52, 3, 123000)

    def test_lines_are_readable_and_written_by_the_background_writer(self):
        log = devlog.DevLog(self.directory, clock=lambda: self.now)
        log.context = '20261009-015203'
        log.info('run begin', rules=21, empty='', none=None, nested={'a': [1, 2]})
        log.warn('step s1#3 capture failed', attempts='a1:FAIL LOCAL_PRICE_CONFIDENCE')
        self.assertTrue(log.flush())
        lines = (self.directory / '2026-10-09.log').read_text('utf-8').splitlines()
        self.assertEqual(lines[0], '01:52:03.123 INFO  [runner 20261009-015203] run begin rules=21 nested={"a":[1,2]}')
        self.assertIn('WARN  [runner 20261009-015203] step s1#3 capture failed attempts=a1:FAIL', lines[1])
        log.close()

    def test_exceptions_carry_their_traceback_and_old_days_are_pruned(self):
        (self.directory / '2026-09-20.log').write_text('old\n', 'utf-8')
        (self.directory / '2026-10-01.gui.log').write_text('recent\n', 'utf-8')
        log = devlog.DevLog(self.directory, clock=lambda: self.now, background=False)
        try:
            raise RuntimeError('COLLECTION_RECEIPT_LAYOUT_CHANGED')
        except RuntimeError as error:
            log.exception('segment failed', error)
        text = (self.directory / '2026-10-09.log').read_text('utf-8')
        self.assertIn('ERROR [runner] segment failed error=COLLECTION_RECEIPT_LAYOUT_CHANGED', text)
        self.assertIn('TRACE [runner] Traceback (most recent call last):', text)
        self.assertFalse((self.directory / '2026-09-20.log').exists())
        self.assertTrue((self.directory / '2026-10-01.gui.log').exists())

    def test_an_unwritable_log_never_raises(self):
        blocker = self.directory / 'file'
        blocker.write_text('x', 'utf-8')
        log = devlog.DevLog(blocker / 'logs', background=False)
        log.info('lost line')
        self.assertEqual(log.errors, 1)

    def test_long_values_are_bounded_and_newlines_flattened(self):
        line = devlog.format_line(self.now, 'INFO', 'runner', 'x', dict(text='a\nb', long='y' * 1000))
        self.assertIn('text=a | b', line)
        self.assertLess(len(line), 400)
        self.assertTrue(line.endswith('\n') and line.count('\n') == 1)


class StepSummaryTests(unittest.TestCase):
    def test_recorded_failure_names_its_codes_and_failing_region(self):
        fields = devlog.step_fields(121, recorded_condition_failure())
        self.assertEqual(fields['error'], 'BATCH_STEP_FAILED')
        self.assertEqual(fields['page'], 'skin_listings')
        self.assertIn('a1:FAIL/281ms LOCAL_PRICE_UNEXPECTED_NATIVE_WORDS', fields['attempts'])
        self.assertIn('{card_fields:LOCAL_PRICE_UNEXPECTED_NATIVE_WORDS}', fields['attempts'])
        self.assertEqual(devlog.step_level(recorded_condition_failure()), 'WARN')

    def test_action_steps_show_point_candidate_receipt_and_settlement(self):
        step = dict(kind='collect_selected', status='finished', passed=True, point=[2337, 320],
                    timings=dict(total_ms=94.0), motion=dict(elapsed_ms=60.3), receipt_mode='pipelined_pixel',
                    collection_attempt=dict(condition='成色S', price='300', wear='0.24', eligible=True),
                    receipt_settlements=[dict(mode='pixel', layout_unchanged=True)])
        fields = devlog.step_fields(5, step)
        self.assertEqual(fields['candidate'], dict(condition='成色S', price='300', wear='0.24', eligible=True))
        self.assertEqual((fields['point'], fields['receipt'], fields['settled']), ([2337, 320], 'pipelined_pixel', ['pixel:same']))
        self.assertEqual(devlog.step_level(step), 'INFO')
        self.assertEqual(devlog.event_fields(dict(event='favorite_confirmed', row=12, receipt_mode='pixel',
                                                  candidate=dict(price='300', wear='0.2'))),
                         dict(row=12, price='300', wear='0.2', receipt='pixel'))


class FailureRecordTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.run = Path(self.temporary.name) / '20261009-015203'
        (self.run / 'segment_01/session.steps').mkdir(parents=True)

    def session(self):
        ok = dict(kind='select_visible_card', status='finished', passed=True, point=[173, 950], timings=dict(total_ms=93.0))
        return SimpleNamespace(steps=[ok, recorded_condition_failure()], report=dict(error='BATCH_STEP_FAILED'),
                               pending=None, pending_geometry=dict(kind='selection'), receipt_settlements=[],
                               last_price_image=dict(png_base64=tiny_png(), bounds=[834, 1123, 149, 42]),
                               last_title_image=None, last_catalog_image=None)

    def test_failure_folder_has_summary_full_step_crops_and_screenshot(self):
        backend = SimpleNamespace(metadata=lambda: dict(identities=dict(target_hwnd=1), capture_transport=dict(pid=9)))
        preview = (dict(capture_passed=True, note='preview packet', startup_page=dict(page='skin_listings'),
                        collection_observation=dict(regions=[dict(kind='card_fields', ok=True,
                            words=[dict(text='成色S'), dict(text='300')])])), b'\xff\xd8jpeg', None)
        folder = devlog.write_failure_dossier(self.run, error=RuntimeError('BATCH_STEP_FAILED'), session=self.session(),
            backend=backend, trial=SimpleNamespace(summary=dict(status='blocked', current_row_index=12, rows=[1] * 12)),
            events=[dict(event='favorite_confirmed', row=12)], preview=preview, extra=dict(detail='x'))
        self.assertEqual(folder, self.run / 'failure')
        record = json.loads((folder / 'failure.json').read_text('utf-8'))
        self.assertEqual(record['error'], 'BATCH_STEP_FAILED')
        self.assertEqual(record['failing_step_index'], 2)
        self.assertEqual(record['failing_step']['capture_attempts'][0]['result']['local_title_error'],
                         'LOCAL_PRICE_UNEXPECTED_NATIVE_WORDS')
        self.assertEqual(record['previous_steps'][0]['point'], [173, 950])
        self.assertEqual(sorted(record['files']), ['failure_preview.jpg', 'price.png'])
        self.assertEqual(record['crops']['price.png'], dict(bounds=[834, 1123, 149, 42]))
        self.assertEqual((folder / 'failure_preview.jpg').read_bytes(), b'\xff\xd8jpeg')
        self.assertNotIn('png_base64', (folder / 'failure.json').read_text('utf-8'))
        text = (folder / 'failure.txt').read_text('utf-8')
        self.assertIn('故障记录：failure/', text)
        self.assertIn("本地价格识别：texts=['300']", text)
        self.assertIn('失败后重读：页面 skin_listings，读图通过；选中卡字段 ["成色S","300"]', text)

    def test_preview_is_read_only_and_skipped_when_the_game_is_not_in_front(self):
        commands = []
        def capture(command, timeout):
            commands.append(command)
            return SimpleNamespace(stdout=json.dumps(dict(capture_passed=True, preview_png_base64=tiny_png(),
                collection_price_image=dict(png_base64='x'))).encode())
        session = SimpleNamespace(_command=lambda step: ['RelinkStudio.exe', '--live-capture-check', '--collection-layout'])
        packet, jpeg, error = devlog.capture_failure_preview(session, SimpleNamespace(foreground=lambda: True, capture=capture))
        self.assertIsNone(error)
        self.assertTrue(jpeg.startswith(b'\xff\xd8'))
        self.assertEqual(commands[0][-1], '--preview-stdout')
        self.assertNotIn('preview_png_base64', packet)
        self.assertNotIn('png_base64', json.dumps(packet))
        away = devlog.capture_failure_preview(session, SimpleNamespace(foreground=lambda: False, capture=capture))
        self.assertEqual(away, (None, None, 'game_not_in_front'))
        self.assertEqual(len(commands), 1)

    def test_diagnosis_reads_any_recorded_run(self):
        (self.run / 'result.json').write_text(json.dumps(dict(status='blocked', error='BATCH_STEP_FAILED',
            new_favorites=30, completed_rules=12, elapsed_ms=35453)), 'utf-8')
        (self.run / 'events.jsonl').write_text(json.dumps(dict(event='favorite_confirmed')) + '\n', 'utf-8')
        step = recorded_condition_failure()
        (self.run / 'segment_01/session.steps/000001.json').write_text(json.dumps(step), 'utf-8')
        text = devlog.summarize_run(self.run)
        self.assertIn('结果：blocked，原因 BATCH_STEP_FAILED（画面确认没有通过，未继续操作）', text)
        self.assertIn('本次新收藏 30 把；已完成任务 12 条', text)
        self.assertIn('读图失败（含已重试成功的）：LOCAL_PRICE_UNEXPECTED_NATIVE_WORDS×1', text)
        self.assertIn('#1 capture failed', text)
        output = io.StringIO()
        with patch('sys.stdout', output):
            self.assertEqual(devlog.main([str(self.run)]), 0)
        self.assertIn('收藏运行 20261009-015203', output.getvalue())


class RunnerRecordTests(unittest.TestCase):
    """run_collection writes the log lines and the failure folder end to end (fakes)."""

    def test_failed_run_logs_steps_and_saves_a_failure_record(self):
        import test_collection_hotkey as runner_tests
        case = runner_tests.RunnerLoopTests('test_user_stop_ends_the_run_without_another_lease')
        case.setUp()
        self.addCleanup(case.temporary.cleanup)
        def failed(trial, gate):
            trial.session.steps.append(recorded_condition_failure())
            trial.session.step_listener = None
            raise RuntimeError('BATCH_STEP_FAILED')
        real_open = hotkey.open_devlog
        opened = []
        def open_log():
            opened.append(real_open())
            return opened[-1]
        with patch.object(hotkey, 'open_devlog', open_log), \
             patch('collection_devlog.capture_failure_preview', return_value=(None, None, 'fake')):
            code, lines, result = case.run_main([failed])
        self.assertEqual(code, 1)
        folder = Path(result['failure_record'])
        self.assertTrue((folder / 'failure.json').is_file() and (folder / 'failure.txt').is_file())
        self.assertTrue(any(line.get('text', '').startswith('已保存故障记录') for line in lines))
        log_text = ''.join(path.read_text('utf-8') for path in (case.root / 'logs').glob('*.log'))
        for expected in ('single run start', 'run begin', 'segment failed error=BATCH_STEP_FAILED', 'run end',
                         'failure_record='):
            self.assertIn(expected, log_text)


if __name__ == '__main__':
    unittest.main()
