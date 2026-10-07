"""Historical evidence-contract checks, NOT real-game business acceptance."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
DATA = json.loads((ROOT / 'docs/business_rebuild/evidence/startup_flow_evidence.json').read_text(encoding='utf-8'))


class StartupEvidenceContract(unittest.TestCase):
    def session(self, file, line=10):
        return next(s for s in DATA['sessions'] if s['start']['file'] == 'log/' + file and s['start']['line'] == line)

    def test_all_historical_log_files_accounted_for(self):
        self.assertEqual(len(DATA['source_files']), 15)
        self.assertEqual(sum(f['bytes'] for f in DATA['source_files']), 1774992038)
        self.assertEqual(sum(f['lf_lines'] for f in DATA['source_files']), 10576696)
        self.assertEqual(sum(f['bytes'] == 0 for f in DATA['source_files']), 2)

    def test_run_entries_are_not_cold_starts(self):
        self.assertEqual(DATA['run_types']['开始执行: 全自动皮肤'], 97)
        self.assertEqual(DATA['run_types']['开始执行: 发送测试'], 3)
        self.assertEqual(DATA['run_entry_count'], 100)
        health = DATA['event_witnesses']['health_begin']
        self.assertEqual(health[1]['file'], health[2]['file'])
        self.assertNotEqual(health[1]['line'], health[2]['line'])

    def test_page_dispatch_includes_recovery_entries(self):
        for page in ['皮肤列表', '皮肤首页', '应用外观', '切换界面', '大战场', 'None', '首页', '筛选']:
            self.assertGreater(DATA['first_page_distribution'][page], 0)
        self.assertNotIn('交易行', DATA['first_page_distribution'])

    def test_initial_watchlist_precedes_filters(self):
        session = self.session('BBZ_20260924_090240.log')
        events = session['early_events']
        watch = next(i for i, e in enumerate(events) if e['message'] == '当前页面: 关注列表')
        filters = next(i for i, e in enumerate(events) if e['message'] == '当前页面: 筛选')
        self.assertLess(watch, filters)
        self.assertIn('当前页面: 皮肤首页', [e['message'] for e in events[watch:filters]])

    def test_noninitial_filter_state_is_not_blindly_toggled(self):
        events = self.session('BBZ_20260924_142356.log')['early_events']
        clear = next(i for i, e in enumerate(events) if e['message'].startswith('取消选择品质:'))
        select = next(i for i, e in enumerate(events) if e['message'].startswith('点击选择品质:'))
        self.assertLess(clear, select)

    def test_existing_list_resumes_before_filtering(self):
        events = self.session('BBZ_20260930_155706.log')['early_events']
        self.assertEqual(events[0]['message'], '当前页面: 皮肤列表')
        sync = next(i for i, e in enumerate(events) if e['message'].startswith('开始校准时间'))
        filters = next(i for i, e in enumerate(events) if e['message'] == '当前页面: 筛选')
        self.assertLess(sync, filters)
        self.assertTrue(any(e['message'] == '购买全部结束' for e in events[sync:filters]))

    def test_multiple_quality_requirements_are_preserved(self):
        events = self.session('BBZ_20260924_090240.log')['early_events']
        needed = {e['message'] for e in events if e['message'].startswith('需要选择品质:')}
        self.assertTrue({'需要选择品质: 史诗品阶', '需要选择品质: 稀有品阶'} <= needed)

    def test_market_wait_and_failure_witnesses_present(self):
        for key in ['market_closed', 'watchlist_full', 'invalid_recognition', 'purchase_timeout', 'capture_failure']:
            self.assertGreater(DATA['event_counts'][key], 0)
            self.assertTrue(DATA['event_witnesses'][key])

    def test_ocr_exports_are_explicit_anchors_not_full_frames(self):
        for key, items in DATA['anchor_witnesses'].items():
            for item in items:
                self.assertTrue(item['anchors'])
                self.assertTrue(all(anchor['words'] == key for anchor in item['anchors']))
        for session in DATA['sessions']:
            self.assertTrue(all('识别结果:' not in e['message'] for e in session['early_events']))

    def test_locators_have_exact_integrity_fields(self):
        for session in DATA['sessions']:
            for evidence in [session['start'], *session['early_events']]:
                self.assertEqual(len(evidence['line_sha256']), 64)
                self.assertGreater(evidence['line'], 0)
                self.assertGreaterEqual(evidence['byte_offset'], 0)
                self.assertGreater(evidence['byte_length'], 0)

    def test_report_preserves_all_39_steps(self):
        report = (ROOT / 'docs/business_rebuild/10_bbzps_first_startup_reconstruction.md').read_text(encoding='utf-8')
        identifiers = set(re.findall(r'S\d{2}', report))
        self.assertTrue({f'S{i:02d}' for i in range(1, 40)} <= identifiers)
        self.assertIn('未还原', report)
        self.assertIn('冷启动', report)
        self.assertIn('我的关注：48/150', report)

    def test_analysis_never_claims_sample_execution(self):
        self.assertFalse(DATA['sample_executed'])
        self.assertEqual(DATA['method'], 'static_text_only')


if __name__ == '__main__':
    unittest.main(verbosity=2)
