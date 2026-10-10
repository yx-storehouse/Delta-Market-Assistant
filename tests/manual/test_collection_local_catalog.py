"""Catalog provider is fixed before execution; names are never repaired."""
import copy
import json
from pathlib import Path
from types import SimpleNamespace
import unittest

from collection_local_title import LocalTitleRecognizer
from collection_labels import exact_label_target
from test_collection_local_title import packet as title_packet
import test_collection_title_memory as memory_tests
from collection_live_session import ForegroundSession
from test_collection_live_session import packet, capture
from test_collection_trial import FakeSession, snapshot
from run_collection_trial import CollectionTrial


def catalog_packet():
    p = title_packet()
    p['collection_catalog_image'] = p.pop('collection_title_image')
    p['collection_catalog_image'].update(schema='collection-catalog-image-v1', kind='catalog_names')
    p['collection_observation']['regions'][0].update(kind='catalog_names')
    return p


class CatalogProviderTests(unittest.TestCase):
    def model(self, text='AS Val突击步枪-黑银先锋', score=.999):
        return LocalTitleRecognizer(Path('.'), engine=lambda im: SimpleNamespace(
            txts=[text], scores=[score], boxes=[[[5,5],[95,5],[95,45],[5,45]]]))

    def test_actual_model_name_and_box_without_expected_text(self):
        p = catalog_packet()
        self.model().apply_catalog(p)
        r = p['collection_observation']['regions'][0]
        self.assertEqual(r['words'][0]['text'], 'AS Val突击步枪-黑银先锋')
        self.assertEqual(r['words'][0]['x'], 680)
        self.assertEqual(r['windows_observation']['words'][0]['text'], 'P')
        self.assertFalse(p['local_catalog_ocr']['expected_text_supplied'])
        self.assertIsNotNone(exact_label_target(p, 'catalog_names', 'ASVal突击步枪-黑银先锋'))

    def test_pending_native_text_stays_unusable_until_actual_local_read(self):
        p=catalog_packet()
        region=p['collection_observation']['regions'][0]
        region.update(ok=False,error='E_LOCAL_TEXT_PENDING',words=[])
        self.assertIsNone(exact_label_target(p,'catalog_names','ASVal突击步枪-黑银先锋'))
        self.model().apply_catalog(p)
        self.assertTrue(region['ok'])
        self.assertEqual(region['windows_observation']['error'],'E_LOCAL_TEXT_PENDING')
        self.assertEqual(region['windows_observation']['words'],[])
        self.assertIsNotNone(exact_label_target(p,'catalog_names','ASVal突击步枪-黑银先锋'))

    def test_pending_text_does_not_fall_back_after_local_low_confidence(self):
        p=catalog_packet()
        region=p['collection_observation']['regions'][0]
        region.update(ok=False,error='E_LOCAL_TEXT_PENDING',words=[])
        self.model(score=.5).apply_catalog(p)
        self.assertFalse(region['ok'])
        self.assertEqual(region['words'],[])

    def test_confusable_is_not_repaired_and_low_confidence_not_promoted(self):
        for text, score in [('AS Va[突击步枪-黑银先锋', .999), ('AS Val突击步枪-黑银先锋', .96)]:
            p = catalog_packet()
            self.model(text, score).apply_catalog(p)
            self.assertIsNone(exact_label_target(p, 'catalog_names', 'ASVal突击步枪-黑银先锋'))
            self.assertNotIn('png_base64', json.dumps(p['local_catalog_ocr']))

    def test_wrong_envelope_frame_hash_kind_and_region_stop(self):
        for key, value in [('frame_id','other'), ('png_sha256','b'*64), ('kind','product_title'),
                           ('schema','collection-title-image-v1'), ('bounds',[675,238,101,60])]:
            with self.subTest(key=key):
                p = catalog_packet(); p['collection_catalog_image'][key] = value
                with self.assertRaises(ValueError): self.model().apply_catalog(p)
        p = catalog_packet(); p['collection_observation']['regions'] *= 2
        with self.assertRaisesRegex(ValueError, 'REGION_COUNT'): self.model().apply_catalog(p)

    def test_absent_catalog_does_not_change_title_or_other_fields(self):
        p = title_packet(); old = copy.deepcopy(p)
        self.model().apply_catalog(p)
        self.assertEqual(p, old)

    def test_every_expected_home_capture_has_ui_regions_including_scroll(self):
        s = FakeSession(); trial = CollectionTrial(s, snapshot(), 'artifacts/test.json')
        trial.capture('skin_home')
        self.assertTrue(s.steps[-1]['ui_regions'])
        trial.capture('skin_listings')
        self.assertNotIn('ui_regions', s.steps[-1])


class CatalogMemoryTests(unittest.TestCase):
    setUp = memory_tests.TitleMemoryTests.setUp
    session = memory_tests.TitleMemoryTests.session
    assert_no_image_persistence = memory_tests.TitleMemoryTests.assert_no_image_persistence

    def test_catalog_envelope_is_stripped_for_success_and_failure(self):
        for success in (True, False):
            with self.subTest(success=success):
                p = packet()
                p.update(collection_catalog_image=self.envelope, ocr_passed=success)
                self.backend.replies = [p]
                s = ForegroundSession('artifacts/catalog/' + str(success) + '/session.json',
                    root=self.root, backend=self.backend, now=self.clock.now, wait=self.clock.wait)
                if success:
                    with s: s.perform(capture(collection_catalog_image=True, attempts=1))
                else:
                    with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
                        with s: s.perform(capture(collection_catalog_image=True, attempts=1))
                self.assertEqual(s.last_catalog_image, self.envelope)
                self.assertNotIn('collection_catalog_image', s.steps[-1]['result'])
                self.assert_no_image_persistence(s)

    def test_new_capture_clears_previous_catalog_memory(self):
        p = packet(); p['collection_catalog_image'] = self.envelope
        self.backend.replies = [p, packet('b')]
        s = self.session()
        with s:
            s.perform(capture(collection_catalog_image=True))
            self.assertIn('--collection-catalog-image', s.steps[-1]['command'])
            s.perform(capture())
            self.assertIsNone(s.last_catalog_image)
        self.assert_no_image_persistence(s)


if __name__ == '__main__':
    unittest.main()
