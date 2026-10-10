"""Same-frame Chinese prefix / English decimal fusion, using run_04 token boxes."""
import copy
import unittest

from collection_candidate import match_selected_first_card
from test_collection_dynamic import dynamic_packet, rule


def sample():
    result = dynamic_packet()
    regions = result['collection_observation']['regions']
    regions[-1]['words'][0]['text'] = '成色B'
    detail = next(r for r in regions if r['kind'] == 'selected_detail')
    detail['words'] = [dict(text='B', x=2214, y=662, width=12, height=17),
        dict(text='〔', x=2228, y=660, width=6, height=20),
        dict(text='1', x=2244, y=662, width=11, height=17),
        dict(text='463027', x=2264, y=662, width=82, height=17),
        dict(text='〕', x=2349, y=660, width=6, height=20)]
    regions.append(dict(kind='selected_detail_precise', ok=True, truncated=False, words=[
        dict(text='BC', x=2213.5716241811388, y=660.0774359262796,
             width=20.867355872865712, height=21.19184933175495),
        dict(text='1.463027)', x=2243.6224234764572, y=659.2425589393032,
             width=111.83617244564142, height=23.57395162777146)]))
    current = dict(rule(), condition_label='成色B', max_wear='5')
    return result, current


def parts(packet):
    regions = packet['collection_observation']['regions']
    return (next(r for r in regions if r['kind'] == 'selected_detail'),
            next(r for r in regions if r['kind'] == 'selected_detail_precise'))


class WearPrefixTests(unittest.TestCase):
    def test_real_run04_boxes_fuse_without_changing_original_tokens(self):
        packet, current = sample()
        before = copy.deepcopy(packet)
        result = match_selected_first_card(packet, current)
        self.assertEqual(result['wear'], '1.463027')
        self.assertEqual(result['condition'], '成色B')
        self.assertEqual(result['wear_source'], 'chinese_prefix_english_decimal')
        evidence = result['wear_evidence']['readings'][0]
        self.assertEqual(evidence['prefix_union'], [2214,660,20,20])
        self.assertEqual([w['text'] for w in evidence['prefix_words']], ['B','〔'])
        self.assertEqual([w['text'] for w in evidence['excluded_english_prefix']], ['BC'])
        self.assertEqual([w['text'] for w in evidence['decimal_words']], ['1.463027)'])
        self.assertTrue(evidence['digit_sequence_agreed'])
        self.assertEqual(packet, before)

    def test_missing_chinese_open_bracket_is_not_replaced_with_english_c(self):
        packet, current = sample()
        primary, english = parts(packet)
        primary['words'].pop(1)
        self.assertEqual(english['words'][0]['text'], 'BC')
        with self.assertRaisesRegex(ValueError, 'WEAR_ASSOCIATION'):
            match_selected_first_card(packet, current)

    def test_missing_english_decimal_is_never_invented(self):
        packet, current = sample()
        parts(packet)[1]['words'][1]['text'] = '1463027)'
        with self.assertRaisesRegex(ValueError, 'WEAR_ASSOCIATION'):
            match_selected_first_card(packet, current)

    def test_excluded_prefix_must_not_contain_any_digit(self):
        for text in ('B1C', 'B１C', '1'):
            packet, current = sample()
            parts(packet)[1]['words'][0]['text'] = text
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, 'WEAR_(PREFIX_DIGIT|ASSOCIATION)'):
                match_selected_first_card(packet, current)

    def test_prefix_and_digits_in_one_word_cannot_be_split_into_fictitious_boxes(self):
        packet, current = sample()
        english = parts(packet)[1]
        english['words'] = [dict(text='BC1.463027)', x=2213.57, y=659.24, width=142, height=23.57)]
        with self.assertRaisesRegex(ValueError, 'WEAR_ASSOCIATION'):
            match_selected_first_card(packet, current)

    def test_prefix_box_must_fit_actual_union_plus_two_pixels(self):
        for change in (dict(x=2211.99), dict(y=657.99), dict(width=24), dict(height=24)):
            packet, current = sample()
            parts(packet)[1]['words'][0].update(change)
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'WEAR_ASSOCIATION'):
                match_selected_first_card(packet, current)

    def test_prefix_gap_stays_within_fourteen_and_baseline_within_six(self):
        for change in (dict(x=2248.01), dict(y=665), dict(x=2233)):
            packet, current = sample()
            parts(packet)[1]['words'][1].update(change)
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'WEAR_ASSOCIATION'):
                match_selected_first_card(packet, current)

    def test_wrong_chinese_or_english_grade_is_not_discarded(self):
        for side in ('chinese', 'english'):
            packet, current = sample()
            primary, english = parts(packet)
            (primary if side == 'chinese' else english)['words'][0]['text'] = 'A' if side == 'chinese' else 'AC'
            with self.subTest(side=side), self.assertRaises(ValueError):
                match_selected_first_card(packet, current)

    def test_truncated_or_conflicting_english_digits_are_not_repaired(self):
        for text in ('1.46302)', '1.463028)', '0.463027)', '.463027)', '1.463027'):
            packet, current = sample()
            parts(packet)[1]['words'][1]['text'] = text
            with self.subTest(text=text), self.assertRaises(ValueError):
                match_selected_first_card(packet, current)

    def test_extra_chinese_digit_cannot_be_dropped(self):
        packet, current = sample()
        parts(packet)[0]['words'][2]['text'] = '11'
        with self.assertRaisesRegex(ValueError, 'WEAR_CONFLICT'):
            match_selected_first_card(packet, current)

    def test_two_complete_readings_still_reject_conflict(self):
        packet, current = sample()
        primary, english = parts(packet)
        primary['words'] = [dict(text='B(1.463028)', x=2214, y=660, width=140, height=20)]
        english['words'] = [dict(text='B(1.463027)', x=2214, y=660, width=140, height=20)]
        with self.assertRaisesRegex(ValueError, 'WEAR_CONFLICT'):
            match_selected_first_card(packet, current)

    def test_explicit_cross_frame_region_or_projection_is_rejected(self):
        for side, key, value in (('english','frame_id','old'), ('english','frame_sha256','f'*64),
                                  ('primary','same_frame',False), ('projection','frame_sha256','f'*64)):
            packet, current = sample()
            primary, english = parts(packet)
            target = english if side == 'english' else primary if side == 'primary' else packet['collection_observation']
            target[key] = value
            with self.subTest(side=side, key=key), self.assertRaisesRegex(ValueError, 'FRAME_BINDING|PRODUCT_MISMATCH'):
                match_selected_first_card(packet, current)


if __name__ == '__main__':
    unittest.main()
