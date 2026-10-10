import base64
import copy
import hashlib
import io
from pathlib import Path
from types import SimpleNamespace
import unittest

from PIL import Image
from collection_local_title import LocalTitleRecognizer, decode_title, mapped_words


def packet():
    stream = io.BytesIO()
    Image.new('RGB', (100, 60), 'gray').save(stream, format='PNG')
    data = stream.getvalue()
    return dict(frames=[dict(width=2560, height=1440, sha256='a'*64)],
        collection_observation=dict(same_frame=True, frame_id='test-frame', frame_sha256='a'*64,
            regions=[dict(kind='product_title', ok=True, truncated=False,
                          words=[dict(text='P', x=682, y=248, width=22, height=35)])]),
        collection_title_image=dict(schema='collection-title-image-v1', kind='product_title', raw_roi=True,
            same_frame=True, image_file_writes=0, coordinate_space='client_physical_px',
            frame_id='test-frame', frame_sha256='a'*64, bounds=[675, 238, 100, 60],
            png_sha256=hashlib.sha256(data).hexdigest(), png_base64=base64.b64encode(data).decode()))


class LocalTitleTests(unittest.TestCase):
    def model(self, text='P90', score=.995, box=None):
        return lambda image: SimpleNamespace(txts=[text], scores=[score],
            boxes=[box or [[5, 5], [85, 5], [85, 50], [5, 50]]])

    def test_actual_model_text_mapped_without_expected_name(self):
        p = packet()
        LocalTitleRecognizer(Path('.'), engine=self.model()).apply(p)
        region = p['collection_observation']['regions'][0]
        self.assertEqual(region['words'][0]['text'], 'P90')
        self.assertEqual(region['words'][0]['x'], 680)
        self.assertEqual(region['words'][0]['y'], 243)
        self.assertEqual(region['windows_observation']['words'][0]['text'], 'P')
        self.assertFalse(p['local_title_ocr']['expected_text_supplied'])
        self.assertNotIn('png_base64', str(p['local_title_ocr']))

    def test_confusable_model_text_is_not_fixed(self):
        p = packet()
        LocalTitleRecognizer(Path('.'), engine=self.model('P9O')).apply(p)
        self.assertEqual(p['collection_observation']['regions'][0]['words'][0]['text'], 'P9O')

    def test_fixed_confidence_rejects_low_confidence_without_windows_fallback(self):
        p = packet()
        LocalTitleRecognizer(Path('.'), engine=self.model(score=.96)).apply(p)
        self.assertFalse(p['collection_observation']['regions'][0]['ok'])
        self.assertEqual(p['local_title_ocr']['rejected'][0]['reason'], 'below_fixed_0.97_threshold')

    def test_bad_frame_or_hash_never_reaches_engine(self):
        for field, value in [('frame_id', 'old-frame'), ('frame_id', None),
                             ('frame_sha256', 'b'*64), ('png_sha256', 'c'*64)]:
            with self.subTest(field=field):
                p = packet(); p['collection_title_image'][field] = value
                def forbidden(image): self.fail('invalid source reached model')
                with self.assertRaises(ValueError): LocalTitleRecognizer(Path('.'), engine=forbidden).apply(p)
                self.assertFalse(p['collection_observation']['regions'][0]['ok'])

    def test_invalid_boxes_rejected_not_clamped_into_valid_words(self):
        for box in ([[[-1, 0], [10, 0], [10, 10], [-1, 10]]],
                    [[[0, 0], [101, 0], [101, 10], [0, 10]]],
                    [[[0, 0], [float('nan'), 0], [10, 10], [0, 10]]]):
            with self.assertRaises(ValueError): mapped_words(['P90'], [.999], box, [675, 238, 100, 60])

    def test_decoded_size_is_bound_to_declared_roi(self):
        p = packet(); p['collection_title_image']['bounds'][2] = 101
        with self.assertRaisesRegex(ValueError, 'DIMENSIONS'):
            decode_title(p, p['collection_title_image'])

    def test_missing_image_leaves_original_untouched(self):
        p = packet(); p.pop('collection_title_image'); before = copy.deepcopy(p)
        LocalTitleRecognizer(Path('.'), engine=self.model()).apply(p)
        self.assertEqual(p, before)

    def test_pending_native_title_requires_real_local_observation(self):
        p = packet()
        region = p['collection_observation']['regions'][0]
        region.update(ok=False, error='E_LOCAL_TEXT_PENDING', words=[])
        LocalTitleRecognizer(Path('.'), engine=self.model()).apply(p)
        self.assertTrue(region['ok'])
        self.assertEqual(region['words'][0]['text'], 'P90')
        self.assertFalse(region['windows_observation']['ok'])
        self.assertEqual(region['windows_observation']['words'], [])

    def test_pending_title_without_crop_stays_unusable(self):
        p = packet()
        p.pop('collection_title_image')
        region = p['collection_observation']['regions'][0]
        region.update(ok=False, error='E_LOCAL_TEXT_PENDING', words=[])
        LocalTitleRecognizer(Path('.'), engine=self.model()).apply(p)
        self.assertFalse(region['ok'])
        self.assertEqual(region['words'], [])

    def test_duplicate_title_region_stops(self):
        p = packet(); p['collection_observation']['regions'] *= 2
        with self.assertRaisesRegex(ValueError, 'REGION_COUNT'):
            LocalTitleRecognizer(Path('.'), engine=self.model()).apply(p)


if __name__ == '__main__':
    unittest.main()
