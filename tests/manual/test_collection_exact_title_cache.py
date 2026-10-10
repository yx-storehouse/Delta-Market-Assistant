"""Fresh-pixel title equivalence is not reuse of old item/price observations."""
import base64
import copy
import hashlib
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from PIL import Image
from collection_local_title import LocalTitleRecognizer
from test_collection_local_title import packet


def fresh(p, frame='next', digest='b'*64):
    p=copy.deepcopy(p)
    p['frames'][0]['sha256']=digest
    for key in ('collection_observation','collection_title_image'):
        p[key].update(frame_id=frame,frame_sha256=digest)
    return p


class ExactTitleCacheTests(unittest.TestCase):
    def setUp(self):
        self.calls=0
        def engine(image):
            self.calls+=1
            return SimpleNamespace(txts=['P90'],scores=[.995],boxes=[[[5,5],[85,5],[85,50],[5,50]]])
        self.reader=LocalTitleRecognizer(Path('.'),engine=engine)

    def test_exact_new_pixels_rebind_to_current_frame_without_inference(self):
        first=packet();self.reader.apply(first)
        second=fresh(packet());self.reader.apply(second)
        self.assertEqual(self.calls,1)
        m=second['local_title_ocr']
        self.assertTrue(m['recognition_cache_hit'])
        self.assertEqual(m['source_frame_id'],'next')
        self.assertEqual(m['source_frame_sha256'],'b'*64)
        self.assertEqual(m['original_read_frame_id'],'test-frame')
        self.assertFalse(m['expected_text_supplied'])

    def test_different_pixel_forces_model_even_if_expected_name_same(self):
        self.reader.apply(packet());p=fresh(packet())
        stream=io.BytesIO();Image.new('RGB',(100,60),'white').save(stream,format='PNG')
        data=stream.getvalue();p['collection_title_image'].update(
            png_base64=base64.b64encode(data).decode(),png_sha256=hashlib.sha256(data).hexdigest())
        self.reader.apply(p)
        self.assertEqual(self.calls,2);self.assertFalse(p['local_title_ocr']['recognition_cache_hit'])

    def test_same_pixels_moved_bounds_are_not_old_geometry(self):
        self.reader.apply(packet());p=fresh(packet());p['collection_title_image']['bounds'][0]+=10
        self.reader.apply(p);self.assertEqual(self.calls,2)
        self.assertEqual(p['collection_observation']['regions'][0]['words'][0]['x'],690)

    def test_corrupt_envelope_never_uses_cached_words(self):
        for field,value in [('frame_id','stale'),('png_sha256','c'*64),('frame_sha256','d'*64)]:
            with self.subTest(field=field):
                self.reader.apply(packet());p=fresh(packet());p['collection_title_image'][field]=value
                with self.assertRaises(ValueError):self.reader.apply(p)
                self.assertFalse(p['collection_observation']['regions'][0]['ok'])
                self.assertIsNone(self.reader._title_cache)

    def test_missing_crop_does_not_supply_old_words(self):
        self.reader.apply(packet());p=fresh(packet());p.pop('collection_title_image')
        p['collection_observation']['regions'][0].update(ok=False,words=[])
        self.reader.apply(p);self.assertFalse(p['collection_observation']['regions'][0]['ok'])

    def test_result_mutation_does_not_poison_cache(self):
        p=packet();self.reader.apply(p);p['collection_observation']['regions'][0]['words'][0]['text']='BAD'
        q=fresh(packet());self.reader.apply(q)
        self.assertEqual(q['collection_observation']['regions'][0]['words'][0]['text'],'P90')

    def test_low_confidence_is_never_cached(self):
        reader=LocalTitleRecognizer(Path('.'),engine=lambda _:SimpleNamespace(
            txts=['P90'],scores=[.96],boxes=[[[5,5],[85,5],[85,50],[5,50]]]))
        reader.apply(packet());self.assertIsNone(reader._title_cache)

    def test_hot_cache_still_decodes_size_frame_and_actual_bytes(self):
        for mutation in ('size','viewport','same_frame','observation_same_frame','base64'):
            with self.subTest(mutation=mutation):
                self.reader.apply(packet())
                p=fresh(packet())
                if mutation=='size':p['collection_title_image']['bounds'][2]+=1
                elif mutation=='viewport':p['frames'][0]['width']=700
                elif mutation=='same_frame':p['collection_title_image']['same_frame']=False
                elif mutation=='observation_same_frame':p['collection_observation']['same_frame']=False
                else:p['collection_title_image']['png_base64']='broken!'
                calls=self.calls
                with self.assertRaises(ValueError):self.reader.apply(p)
                self.assertEqual(self.calls,calls)
                self.assertIsNone(self.reader._title_cache)
                self.assertFalse(p['collection_observation']['regions'][0]['ok'])
                self.assertFalse(p['local_title_ocr']['ok'])

    def test_cache_hit_result_mutations_never_change_following_hit(self):
        self.reader.apply(packet())
        second=fresh(packet());self.reader.apply(second)
        second['local_title_ocr']['words'][0].update(text='BAD',x=-1,score=1)
        second['local_title_ocr']['rejected'].append(dict(text='INJECTED'))
        second['collection_observation']['regions'][0]['windows_observation']['words'][0]['text']='BAD-NATIVE'
        third=fresh(packet(),frame='third',digest='c'*64);self.reader.apply(third)
        self.assertEqual(self.calls,1)
        self.assertEqual(third['collection_observation']['regions'][0]['words'][0]['text'],'P90')
        self.assertEqual(third['collection_observation']['regions'][0]['words'][0]['x'],680)
        self.assertEqual(third['local_title_ocr']['rejected'],[])
        self.assertEqual(third['local_title_ocr']['source_frame_id'],'third')
        self.assertEqual(third['local_title_ocr']['source_frame_sha256'],'c'*64)
        self.assertEqual(third['local_title_ocr']['original_read_frame_id'],'test-frame')

    def test_catalog_reads_do_not_use_or_replace_title_cache(self):
        self.reader.apply(packet())
        for index in range(2):
            p=fresh(packet(),frame='catalog-'+str(index))
            p['collection_catalog_image']=p.pop('collection_title_image')
            p['collection_catalog_image'].update(schema='collection-catalog-image-v1',kind='catalog_names')
            p['collection_observation']['regions'][0]['kind']='catalog_names'
            self.reader.apply_catalog(p)
            self.assertFalse(p['local_catalog_ocr']['recognition_cache_hit'])
        self.assertEqual(self.calls,3)
        p=fresh(packet(),frame='title-again');self.reader.apply(p)
        self.assertEqual(self.calls,3)
        self.assertTrue(p['local_title_ocr']['recognition_cache_hit'])

    def test_title_hit_never_changes_price_wear_or_their_image_envelopes(self):
        self.reader.apply(packet())
        p=fresh(packet())
        untouched_regions=[dict(kind='card_fields',words=[dict(text='999')],ok=False),
                           dict(kind='selected_detail',words=[dict(text='A(1.23)')],ok=True)]
        p['collection_observation']['regions'].extend(copy.deepcopy(untouched_regions))
        untouched=dict(collection_price_image={'new_current_price':True},
                       local_price_ocr={'ok':False,'source_frame_id':'next'},
                       collection_selected_card={'selected':False})
        p.update(copy.deepcopy(untouched))
        self.reader.apply(p)
        self.assertEqual(self.calls,1)
        self.assertEqual(p['collection_observation']['regions'][1:],untouched_regions)
        self.assertEqual({key:p[key] for key in untouched},untouched)

    def test_different_png_encoding_requires_model_even_for_same_decoded_pixels(self):
        from PIL.PngImagePlugin import PngInfo
        self.reader.apply(packet());p=fresh(packet())
        meta=PngInfo();meta.add_text('synthetic-test','same-image-other-encoding')
        stream=io.BytesIO();Image.new('RGB',(100,60),'gray').save(stream,format='PNG',pnginfo=meta)
        raw=stream.getvalue()
        p['collection_title_image'].update(png_base64=base64.b64encode(raw).decode(),
            png_sha256=hashlib.sha256(raw).hexdigest())
        self.reader.apply(p)
        self.assertEqual(self.calls,2)
        self.assertFalse(p['local_title_ocr']['recognition_cache_hit'])

    def test_failed_new_read_drops_old_success_instead_of_falling_back(self):
        self.reader.apply(packet());original_engine=self.reader.engine
        def fail(image):
            self.calls+=1
            raise RuntimeError('synthetic model failure')
        self.reader.engine=fail
        p=fresh(packet());p['collection_title_image']['bounds'][0]+=10
        with self.assertRaisesRegex(RuntimeError,'synthetic model failure'):self.reader.apply(p)
        self.assertIsNone(self.reader._title_cache)
        self.assertEqual(p['collection_observation']['regions'][0]['words'],[])
        self.assertFalse(p['collection_observation']['regions'][0]['ok'])
        self.reader.engine=original_engine
        q=fresh(packet(),frame='after-failure',digest='c'*64);self.reader.apply(q)
        self.assertEqual(self.calls,3)
        self.assertFalse(q['local_title_ocr']['recognition_cache_hit'])

    def test_same_screen_hash_with_new_acquisition_id_does_not_invent_new_pixels(self):
        self.reader.apply(packet())
        p=fresh(packet(),frame='independent-same-pixels',digest='a'*64)
        self.reader.apply(p)
        self.assertEqual(self.calls,1)
        self.assertEqual(p['local_title_ocr']['source_frame_id'],'independent-same-pixels')
        self.assertEqual(p['local_title_ocr']['source_frame_sha256'],'a'*64)
        self.assertEqual(p['local_title_ocr']['evidence_basis'],
                         'verified_exact_current_roi_bytes_same_bounds')


if __name__=='__main__':unittest.main()
