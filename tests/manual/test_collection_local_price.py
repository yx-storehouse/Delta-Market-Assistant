import base64
import copy
import hashlib
import io
from pathlib import Path
from types import SimpleNamespace
import unittest

from PIL import Image
from collection_local_title import LocalTitleRecognizer
from collection_local_price import LocalPriceRecognizer, price_words


def packet():
    stream = io.BytesIO()
    Image.new('RGB', (149, 42), 'gray').save(stream, format='PNG')
    data = stream.getvalue()
    card = dict(id='card', selected=True, fields_bounds=[997, 525, 864, 42],
                price_bounds=[1712, 525, 149, 42], condition_bounds=[1000, 525, 150, 42])
    measurement = dict(schema='numeric-roi-v1', same_frame=True, frame_id='test',
        source_region=card['price_bounds'], source_frame_bytes_unchanged=True, image_file_writes=0,
        crop_bounds=[1812, 537, 42, 19], ink_bounds=[1813, 538, 40, 17],
        reliable_digit_span_minimum=3,
        reliable_digit_span_bounds=[[1813, 538, 12, 17], [1828, 538, 11, 17], [1842, 538, 11, 17]])
    return dict(frames=[dict(width=2560, height=1440, sha256='a'*64)],
        collection_selected_card=dict(selected=True, same_frame=True, card_id='card',
                                      frame_id='test', frame_sha256='a'*64),
        collection_layout=dict(complete=True, same_frame=True, frame_id='test', frame_sha256='a'*64, cards=[card]),
        collection_observation=dict(same_frame=True, frame_id='test', frame_sha256='a'*64,
            regions=[dict(kind='card_fields', card_id='card', bounds=card['fields_bounds'], ok=False,
                error='E_LOCAL_PRICE_PENDING', truncated=False,
                words=[dict(text='成色A', x=1010, y=538, width=65, height=17)],
                field_attempts=[dict(field='condition_bounds', ok=True, same_frame=True, frame_id='test'),
                                dict(field='price_bounds', ok=False, error='E_LOCAL_PRICE_PENDING')])]),
        collection_price_image=dict(schema='collection-price-image-v1', kind='price', raw_roi=True,
            coordinate_space='client_physical_px', same_frame=True, frame_id='test', frame_sha256='a'*64,
            card_id='card', image_file_writes=0, bounds=card['price_bounds'], numeric_preprocess=measurement,
            png_sha256=hashlib.sha256(data).hexdigest(), png_base64=base64.b64encode(data).decode()))


class LocalPriceTests(unittest.TestCase):
    def provider(self, text='480', score=1., box=None):
        model = LocalTitleRecognizer(Path('.'), engine=lambda image: SimpleNamespace(
            txts=[text], scores=[score], boxes=[box or [[0, 0], [126, 0], [126, 57], [0, 57]]]))
        return LocalPriceRecognizer(model)

    def test_complete_observed_digits_with_actual_detector_box(self):
        p = packet(); self.provider().apply(p)
        region = p['collection_observation']['regions'][0]
        self.assertTrue(region['ok'])
        self.assertEqual([w['text'] for w in region['words']], ['成色A', '480'])
        self.assertEqual(region['words'][1]['x'], 1812)
        self.assertEqual(region['words'][1]['width'], 42)
        self.assertFalse(p['local_price_ocr']['expected_value_supplied'])
        self.assertNotIn('png_base64', str(p['local_price_ocr']))

    def test_actual_model_output_not_corrected_to_expected_amount(self):
        p = packet(); self.provider('490').apply(p)
        self.assertEqual(p['local_price_ocr']['words'][0]['text'], '490')

    def test_cjk_box_letter_and_decimal_are_not_replaced(self):
        for text in ['48囗', '48O', '48o', '48.0', '48 0', '４８０']:
            with self.subTest(text=text):
                p = packet()
                with self.assertRaisesRegex(ValueError, 'LOCAL_PRICE_TEXT'):
                    self.provider(text).apply(p)
                self.assertFalse(p['collection_observation']['regions'][0]['ok'])

    def test_leading_or_trailing_missing_digit_even_with_wide_box_rejected(self):
        for text in ['48', '80']:
            with self.assertRaisesRegex(ValueError, 'DIGITS_MISSING'):
                self.provider(text).apply(packet())

    def test_low_confidence_empty_and_bad_boxes_rejected(self):
        with self.assertRaisesRegex(ValueError, 'CONFIDENCE'):
            self.provider(score=.989).apply(packet())
        for box in [[[-1, 0], [126, 0], [126, 57], [0, 57]],
                    [[0, 0], [126, 0], [126, float('nan')], [0, 57]]]:
            with self.assertRaisesRegex(ValueError, 'BOX'):
                self.provider(box=box).apply(packet())
        with self.assertRaisesRegex(ValueError, 'INCOMPLETE'):
            self.provider(box=[[20, 0], [126, 0], [126, 57], [20, 57]]).apply(packet())

    def test_raw_confidence_bool_or_string_not_coerced_to_number(self):
        for score in (True, False, '1.0', None, float('inf'), float('nan')):
            with self.subTest(score=score):
                with self.assertRaisesRegex(ValueError, 'CONFIDENCE_TYPE'):
                    self.provider(score=score).apply(packet())

    def test_cross_frame_card_region_image_measurement_rejected_before_model(self):
        changes = [('collection_price_image', 'frame_id', 'old'),
            ('collection_price_image', 'frame_sha256', 'b'*64),
            ('collection_price_image', 'png_sha256', 'b'*64),
            ('collection_price_image', 'card_id', 'wrong'),
            ('collection_selected_card', 'selected', False),
            ('collection_layout', 'frame_sha256', 'b'*64)]
        for section, key, value in changes:
            p = packet(); p[section][key] = value
            with self.assertRaises(ValueError): self.provider().apply(p)
        for key, value in [('frame_id', 'old'), ('crop_bounds', [1700, 520, 300, 42]),
                           ('reliable_digit_span_minimum', 0), ('source_frame_bytes_unchanged', False)]:
            p = packet(); p['collection_price_image']['numeric_preprocess'][key] = value
            with self.assertRaises(ValueError): self.provider().apply(p)

    def test_native_condition_failure_not_masked_by_good_number(self):
        p = packet()
        p['collection_observation']['regions'][0]['field_attempts'][0]['ok'] = False
        with self.assertRaisesRegex(ValueError, 'CONDITION_UNPROVEN'): self.provider().apply(p)

    def test_nested_condition_rereads_keep_one_authoritative_field(self):
        p = packet()
        condition = p['collection_observation']['regions'][0]['field_attempts'][0]
        condition['selected_attempt'] = 2
        condition['condition_refinement_attempts'] = [
            dict(ok=True, same_frame=True, frame_id='test', literal_condition=False,
                 words=[dict(text='|'), dict(text='成色A')]),
            dict(ok=True, same_frame=True, frame_id='test', literal_condition=True,
                 words=[dict(text='成色A')])]
        original = copy.deepcopy(condition)
        self.provider().apply(p)
        region = p['collection_observation']['regions'][0]
        self.assertTrue(region['ok'])
        readings = [a for a in region['field_attempts'] if a['field'] == 'condition_bounds']
        self.assertEqual(readings, [original])
        self.assertEqual([w['text'] for w in region['words']], ['成色A', '480'])

    def test_failed_refinement_authoritative_field_blocks_price_model(self):
        p = packet()
        condition = p['collection_observation']['regions'][0]['field_attempts'][0]
        condition.update(ok=False, error='E_COLLECTION_CONDITION_REFINEMENT',
            selected_attempt=2, condition_refinement_attempts=[dict(ok=True), dict(ok=False)])
        provider = self.provider()
        def forbidden(_image):
            self.fail('failed authoritative condition must stop before the price model')
        provider.model.engine = forbidden
        with self.assertRaisesRegex(ValueError, 'CONDITION_UNPROVEN'):
            provider.apply(p)

    def test_no_implicit_provider_switch_or_pending_image_omission(self):
        p = packet(); p['collection_observation']['regions'][0]['error'] = 'E_OCR_NUMERIC_TEXT'
        with self.assertRaisesRegex(ValueError, 'PROVIDER_NOT_SELECTED'): self.provider().apply(p)
        p = packet(); del p['collection_price_image']
        with self.assertRaisesRegex(ValueError, 'IMAGE_MISSING'): self.provider().apply(p)

    def test_nonlisting_no_image_no_change(self):
        p = dict(frames=[]); original = copy.deepcopy(p)
        self.provider().apply(p)
        self.assertEqual(p, original)


if __name__ == '__main__':
    unittest.main()
