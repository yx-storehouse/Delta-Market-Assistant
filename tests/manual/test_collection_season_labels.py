"""Offline season resolution regressions; no capture backend or game input."""
import copy
import hashlib
import json
import unittest
from dataclasses import FrozenInstanceError
from pathlib import Path
from unittest.mock import patch

from collection_season_labels import (
    EVIDENCE, GAME_LABEL, MAPPING_ID, SOURCE_LABEL, VERIFIED_PRODUCTS,
    resolve_season_label,
)
from run_collection_trial import CollectionTrial, rule_fingerprint
from test_collection_trial import FakeSession, row, snapshot


def black_silver(product=VERIFIED_PRODUCTS[0]):
    return dict(row(), product_id=product[0], product_name=product[1],
                season_id='S11', season_label=SOURCE_LABEL)


class SeasonLabelTests(unittest.TestCase):
    def test_actual_exporter_spellings_for_rows_12_14_16(self):
        for index, product_id, name in (
                (12, '11102', 'AUG突击步枪-黑银先锋'),
                (14, '11103', 'ASVal突击步枪-黑银先锋'),
                (16, '11104', 'P90冲锋枪-黑银先锋')):
            with self.subTest(row_index=index):
                rule = dict(black_silver((product_id, name)), row_index=index)
                before = copy.deepcopy(rule)
                self.assertEqual(resolve_season_label(rule).game_label, GAME_LABEL)
                self.assertEqual(rule, before)

    def test_current_real_snapshot_rows_resolve_without_changing_bytes(self):
        path = Path(__file__).resolve().parents[2] / 'artifacts/collection_full_cycle/input/task_snapshot.json'
        if not path.exists():
            self.skipTest('Optional local live-run snapshot is not distributed with unit tests')
        before = path.read_bytes()
        rows = {rule['row_index']: rule for rule in json.loads(before)['rows']}
        for index, product_id in ((12, '11102'), (14, '11103'), (16, '11104')):
            with self.subTest(row_index=index):
                self.assertEqual(rows[index]['product_id'], product_id)
                self.assertEqual(resolve_season_label(rows[index]).game_label, GAME_LABEL)
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), hashlib.sha256(before).digest())

    def test_three_exact_known_products_resolve(self):
        for product in VERIFIED_PRODUCTS:
            with self.subTest(product=product):
                result = resolve_season_label(black_silver(product))
                self.assertEqual(result.source_label, SOURCE_LABEL)
                self.assertEqual(result.game_label, GAME_LABEL)
                self.assertEqual(result.mapping_id, MAPPING_ID)
                self.assertEqual(result.evidence, EVIDENCE)

    def test_unverified_id_name_season_or_provenance_is_not_changed(self):
        cases = (
            dict(product_id='99999'), dict(product_id=11102),
            dict(product_id='11103'), dict(product_name='AUG突击步枪-天命'),
            dict(product_name='AUG突击步枪 - 黑银先锋'),
            dict(product_name='AUG突击步枪-黑银先锋-极品'),
            dict(product_id='11103', product_name='AS  Val突击步枪-黑银先锋'),
            dict(product_id='11103', product_name='asval突击步枪-黑银先锋'),
            dict(product_id='11102', product_name='ASVal突击步枪-黑银先锋'),
            dict(season_id='S12'), dict(season_label='疾风魅影 S2'),
            dict(dictionary_resolved=False), dict(dictionary_resolved=1),
            dict(product_id='11100', product_name='MK4冲锋枪-疾风魅影-极品'),
            dict(product_id='11101', product_name='MK4冲锋枪-疾风魅影-优品'),
        )
        for change in cases:
            with self.subTest(change=change):
                rule = dict(black_silver(), **change)
                result = resolve_season_label(rule)
                self.assertEqual(result.game_label, rule['season_label'])
                self.assertFalse(result.corrected)
                self.assertEqual(result.evidence, ())

    def test_missing_identity_fields_are_not_inferred(self):
        for field in ('product_id', 'product_name', 'season_id', 'dictionary_resolved'):
            rule = black_silver()
            del rule[field]
            self.assertEqual(resolve_season_label(rule).game_label, SOURCE_LABEL)

    def test_existing_game_spelling_is_identity_without_fake_evidence(self):
        rule = dict(black_silver(), season_label=GAME_LABEL)
        result = resolve_season_label(rule)
        self.assertFalse(result.corrected)
        self.assertEqual(result.game_label, GAME_LABEL)
        self.assertEqual(result.event_fields()['season_label_evidence'], [])

    def test_source_row_and_rule_fingerprint_stay_byte_equivalent(self):
        rule = black_silver()
        before = json.dumps(rule, ensure_ascii=False, sort_keys=True)
        digest = rule_fingerprint(rule)
        result = resolve_season_label(rule)
        fields = result.event_fields()
        fields['season_label_evidence'].append('test-mutation')
        self.assertEqual(result.evidence, EVIDENCE)
        self.assertEqual(json.dumps(rule, ensure_ascii=False, sort_keys=True), before)
        self.assertEqual(rule_fingerprint(rule), digest)
        with self.assertRaises(FrozenInstanceError):
            result.game_label = SOURCE_LABEL

    def test_corrected_active_season_reuses_title_and_audits_source(self):
        rule = black_silver()
        data = snapshot()
        data['rows'] = [rule]
        before = copy.deepcopy(data)
        events = []
        session = FakeSession()
        trial = CollectionTrial(session, data, 'unused.json', emit=events.append)
        trial.active_season = GAME_LABEL
        with patch('run_collection_trial.exact_label_target', return_value=(100, 100)):
            trial.open_product(rule)
        self.assertEqual(session.steps, [])
        self.assertEqual(data, before)
        self.assertEqual(trial.snapshot, before)
        event = events[0]
        self.assertEqual(event['event'], 'catalog_season_label_resolved')
        self.assertEqual(event['source_season_label'], SOURCE_LABEL)
        self.assertEqual(event['game_season_label'], GAME_LABEL)
        self.assertEqual(event['season_label_evidence'], list(EVIDENCE))

    def test_literal_source_active_season_is_not_treated_as_verified_game_season(self):
        trial = CollectionTrial(FakeSession(), snapshot(), 'unused.json')
        trial.active_season = SOURCE_LABEL
        trial.filter_for = lambda rule: (_ for _ in ()).throw(RuntimeError('FILTER_REQUIRED'))
        with patch('run_collection_trial.exact_label_target', return_value=(100, 100)):
            with self.assertRaisesRegex(RuntimeError, 'FILTER_REQUIRED'):
                trial.open_product(black_silver())

    def test_filter_selects_exact_game_label_and_readback_without_mutation(self):
        rule = black_silver()
        before = copy.deepcopy(rule)
        session = FakeSession()
        session.previous = {'startup_page': {'page': 'skin_home'}}
        events, selections, searches = [], [], []
        trial = CollectionTrial(session, snapshot(), 'unused.json', emit=events.append)
        state = {'complete': True, 'season_label': '棱镜攻势 S2', 'checkboxes': {
            key: {'state': 'unchecked', 'bounds': [100 * index, 500, 30, 30]} for index, key in
            enumerate(('owned', 'unowned', 'legendary', 'epic', 'rare', 'common'))}}
        def capture(page=None, **options):
            frame = copy.deepcopy(state)
            frame.update(valid_page=True, same_frame=True, frame_id='fake:filter', frame_sha256='f' * 64)
            session.previous = {'startup_page': {'page': page or 'catalog_filter'},
                                'catalog_filter_state': frame, 'frames': [{'sha256': 'f' * 64}]}
        def select(region, label):
            selections.append((region, label))
            state['season_label'] = label
        def click(point, *args, **kwargs):
            for box in state['checkboxes'].values():
                if box['bounds'][0] + 15 == point[0] and point[1] == 515:
                    box['state'] = 'checked' if box['state'] == 'unchecked' else 'unchecked'
        trial.capture = capture
        trial.click = click
        trial.hover = lambda *args, **kwargs: None
        trial.click_label = select
        trial.find_season = searches.append
        trial.filter_for(rule)
        self.assertEqual(searches, [GAME_LABEL])
        self.assertEqual(selections, [('season_options', GAME_LABEL)])
        self.assertEqual(trial.active_season, GAME_LABEL)
        self.assertEqual({key: box['state'] for key, box in state['checkboxes'].items() if key in ('owned', 'unowned')},
                         dict(owned='checked', unowned='checked'))
        self.assertEqual(rule, before)
        self.assertEqual(events[-1]['source_season_label'], SOURCE_LABEL)
        self.assertEqual(events[-1]['game_season_label'], GAME_LABEL)
        self.assertEqual(events[-1]['season_label_evidence'], list(EVIDENCE))


if __name__ == '__main__':
    unittest.main()
