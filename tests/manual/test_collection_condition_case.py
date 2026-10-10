"""Literal condition labels with ASCII-only grade case, no inferred grade."""
import copy
import json
from pathlib import Path
import unittest

from collection_candidate import match_selected_first_card
from collection_journal import candidate_key
from test_collection_dynamic import dynamic_packet, rule

ROOT = Path(__file__).resolve().parents[2]


def with_condition(label, *, detail_grade='S', digest='a'):
    packet = dynamic_packet(digest)
    fields = next(region for region in packet['collection_observation']['regions']
                  if region['kind'] == 'card_fields')
    fields['words'][0]['text'] = label
    detail = next(region for region in packet['collection_observation']['regions']
                  if region['kind'] == 'selected_detail')
    detail['words'][0]['text'] = detail_grade + '(0.187079)'
    return packet


class ConditionCaseTests(unittest.TestCase):
    def test_only_observed_ascii_grade_is_case_normalized(self):
        for grade in 'sSaAbBcC':
            with self.subTest(grade=grade):
                expected = '成色' + grade.upper()
                packet = with_condition('成色' + grade, detail_grade=grade.upper())
                frozen = copy.deepcopy(packet)
                candidate = match_selected_first_card(packet, dict(rule(), condition_label=expected))
                self.assertEqual(candidate['condition'], expected)
                self.assertTrue(candidate['eligible'])
                evidence = candidate['condition_evidence']
                self.assertEqual(evidence['raw_text'], '成色' + grade)
                self.assertEqual(evidence['raw_grade'], grade)
                self.assertEqual(evidence['canonical_text'], expected)
                self.assertEqual(evidence['method'], 'literal_prefix_ascii_grade_case')
                self.assertEqual(evidence['case_changed'], grade.islower())
                self.assertEqual(evidence['independent_detail_condition'], expected)
                self.assertEqual(evidence['frame_id'], candidate['source_frame_id'])
                self.assertEqual(evidence['frame_sha256'], candidate['source_frame_sha256'])
                self.assertEqual(packet, frozen)

    def test_lowercase_card_and_conflicting_uppercase_detail_are_rejected(self):
        for card_grade, detail_grade in (('s', 'A'), ('a', 'B'), ('b', 'C'), ('c', 'S')):
            with self.subTest(card_grade=card_grade, detail_grade=detail_grade):
                with self.assertRaisesRegex(ValueError, '^COLLECTION_WEAR_ASSOCIATION$'):
                    match_selected_first_card(with_condition('成色' + card_grade,
                        detail_grade=detail_grade), dict(rule(), condition_label='成色' + card_grade.upper()))

    def test_rule_grade_is_checked_independently_after_detail(self):
        with self.assertRaisesRegex(ValueError, '^COLLECTION_CONDITION_MISMATCH$'):
            match_selected_first_card(with_condition('成色s'), dict(rule(), condition_label='成色A'))
        accepted = match_selected_first_card(with_condition('成色s'), dict(rule(), condition_label='仅磨损'))
        self.assertEqual(accepted['condition'], '成色S')

    def test_missing_prefix_extra_letters_and_digits_are_not_repaired(self):
        labels = ('s', 'S', '色s', '成s', '成色', '成色ss', '成色Sa', '成色5',
                  '成色8', '成色0', '成色D', '成色r', '成色 s', '成色s ', '成色s\n',
                  ' 成色s', '成色S级', '品質S', '成\u200b色s')
        for label in labels:
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, '^COLLECTION_CARD_CONDITION$'):
                match_selected_first_card(with_condition(label), rule())

    def test_non_ascii_homographs_and_compatibility_letters_are_rejected(self):
        for grade in ('Ｓ', 'ｓ', 'ſ', 'Ѕ', 'ѕ', 'Α', 'А', 'В', 'С', 'Ϲ', '𝑆', 'ᴀ', 'ß'):
            with self.subTest(grade=grade), self.assertRaisesRegex(ValueError, '^COLLECTION_CARD_CONDITION$'):
                match_selected_first_card(with_condition('成色' + grade), rule())

    def test_no_global_uppercase_of_detail_text(self):
        with self.assertRaisesRegex(ValueError, '^COLLECTION_WEAR_ASSOCIATION$'):
            match_selected_first_card(with_condition('成色s', detail_grade='s'), rule())

    def test_cross_frame_detail_and_frame_projection_remain_rejected(self):
        for changed in ('frame_id', 'frame_sha256', 'same_frame'):
            packet = with_condition('成色s')
            detail = next(region for region in packet['collection_observation']['regions']
                          if region['kind'] == 'selected_detail')
            detail[changed] = False if changed == 'same_frame' else 'different-frame'
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, '^COLLECTION_FRAME_BINDING$'):
                match_selected_first_card(packet, rule())

    def test_condition_roi_binding_remains_required(self):
        packet = with_condition('成色s')
        fields = next(region for region in packet['collection_observation']['regions']
                      if region['kind'] == 'card_fields')
        fields['words'][0]['x'] += 200
        with self.assertRaisesRegex(ValueError, '^COLLECTION_DYNAMIC_UNASSOCIATED_FIELD$'):
            match_selected_first_card(packet, rule())

    def test_raw_evidence_is_copied_and_historical_identity_key_does_not_change(self):
        upper = match_selected_first_card(with_condition('成色S'), rule())
        lower_packet = with_condition('成色s', digest='b')
        lower = match_selected_first_card(lower_packet, rule(), receipt_before=upper)
        self.assertEqual(candidate_key(upper), candidate_key(lower))
        lower['condition_evidence']['raw_words'][0]['text'] = 'changed only in returned evidence'
        fields = next(region for region in lower_packet['collection_observation']['regions']
                      if region['kind'] == 'card_fields')
        self.assertEqual(fields['words'][0]['text'], '成色s')

    def test_over_price_does_not_skip_condition_or_detail_checks(self):
        for label, detail, error in (('成色5', 'S', 'COLLECTION_CARD_CONDITION'),
                                     ('成色s', 'A', 'COLLECTION_WEAR_ASSOCIATION')):
            packet = with_condition(label, detail_grade=detail)
            fields = next(region for region in packet['collection_observation']['regions']
                          if region['kind'] == 'card_fields')
            fields['words'][-1]['text'] = '1700'
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, '^' + error + '$'):
                match_selected_first_card(packet, rule())


class RecordedLowercaseConditionTests(unittest.TestCase):
    def test_actual_run02_1700_price_case_packet(self):
        source = ROOT / 'artifacts/collection_speed/run_02/session.json'
        if not source.is_file():
            self.skipTest('local recorded collection_speed/run_02 packet absent')
        document = json.loads(source.read_text(encoding='utf-8'))
        self.assertEqual(document['steps'][42]['error'], 'COLLECTION_CARD_CONDITION')
        packet = document['steps'][41]['result']
        self.assertEqual(packet['collection_observation']['frame_id'],
                         'dxgi:512f2bf0-beca-4c79-9891-af367de2f563')
        self.assertEqual(packet['collection_observation']['frame_sha256'],
                         'aa82048e92025887425e5f26cd5e8b21b1be2885e5f33d37b349a07178a1ba4d')
        fields = next(region for region in packet['collection_observation']['regions']
                      if region['kind'] == 'card_fields')
        self.assertEqual([word['text'] for word in fields['words']], ['成', '色', 's', '1,700'])
        self.assertEqual(fields['words'][-1]['score'], .99994)
        frozen = copy.deepcopy(packet)
        candidate = match_selected_first_card(packet, rule())
        self.assertEqual(candidate['condition'], '成色S')
        self.assertEqual(candidate['price'], '1700')
        self.assertEqual(candidate['wear'], '0.381022')
        self.assertFalse(candidate['eligible'])
        self.assertEqual(candidate['condition_evidence']['raw_text'], '成色s')
        self.assertEqual([word['text'] for word in candidate['condition_evidence']['raw_words']],
                         ['成', '色', 's'])
        self.assertEqual(packet, frozen)


if __name__ == '__main__':
    unittest.main()
