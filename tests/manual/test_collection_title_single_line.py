"""Known listing title route; image/frame checks precede either OCR route."""
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


def listing():
    p=packet();out=io.BytesIO();Image.new('RGB',(530,54),'gray').save(out,format='PNG');raw=out.getvalue()
    p['startup_page']=dict(page='skin_listings',overlay='none')
    p['collection_title_image'].update(bounds=[1915,238,530,54],png_base64=base64.b64encode(raw).decode(),
                                      png_sha256=hashlib.sha256(raw).hexdigest())
    return p


class ListingTitleSingleLineTests(unittest.TestCase):
    def reader(self,text='AUG突击步枪-黑银先锋',score=.995,**options):
        self.lines=self.detections=0
        def line(image):
            self.lines+=1;self.assertEqual(image.size,(530,54))
            return SimpleNamespace(txts=[text],scores=[score])
        def full(image):
            self.detections+=1
            return SimpleNamespace(txts=['FULL'],scores=[.998],boxes=[[[10,10],[100,10],[100,40],[10,40]]])
        return LocalTitleRecognizer(Path('.'),engine=full,line_reader=line,**options)

    def test_current_whole_roi_read_without_detector_or_expected_product(self):
        r=self.reader();p=listing();r.apply(p)
        self.assertEqual((self.lines,self.detections),(1,0))
        w=p['collection_observation']['regions'][0]['words'][0]
        self.assertEqual(w['text'],'AUG突击步枪-黑银先锋')
        self.assertEqual([w[k] for k in ('x','y','width','height')],[1915,238,530,54])
        self.assertIn('not_detector_boxes',p['local_title_ocr']['word_bounds_basis'])
        self.assertFalse(p['local_title_ocr']['single_line_attempt']['expected_text_supplied'])

    def test_confident_different_name_is_not_retried_until_expected(self):
        r=self.reader('DIFFERENT');p=listing();r.apply(p)
        self.assertEqual((self.lines,self.detections),(1,0))
        self.assertEqual(p['collection_observation']['regions'][0]['words'][0]['text'],'DIFFERENT')

    def test_low_confidence_or_multiline_uses_original_detection(self):
        for text,score in (('AUG',.96),('',.999),('AUG\nP90',.999)):
            r=self.reader(text,score);p=listing();r.apply(p)
            self.assertEqual((self.lines,self.detections),(1,1))
            self.assertEqual(p['local_title_ocr']['word_bounds_basis'],'actual_detector_boxes')

    def test_other_page_overlay_bounds_and_disabled_route_keep_detector(self):
        for change in ('page','overlay','bounds','disabled'):
            r=self.reader(fast_title=change!='disabled');p=listing()
            if change=='page':p['startup_page']['page']='skin_home'
            if change=='overlay':p['startup_page']['overlay']='listing_filter'
            if change=='bounds':p['collection_title_image']['bounds'][0]-=1
            r.apply(p);self.assertEqual((self.lines,self.detections),(0,1))

    def test_bad_source_never_reaches_either_route(self):
        for field,value in (('frame_id','old'),('png_sha256','c'*64),('same_frame',False)):
            r=self.reader();p=listing();p['collection_title_image'][field]=value
            with self.assertRaises(ValueError):r.apply(p)
            self.assertEqual((self.lines,self.detections),(0,0))

    def test_exact_fresh_cache_retains_truthful_bounds_basis(self):
        r=self.reader();p=listing();r.apply(p);q=listing()
        q['frames'][0]['sha256']='b'*64
        for key in ('collection_observation','collection_title_image'):q[key].update(frame_id='new',frame_sha256='b'*64)
        r.apply(q)
        self.assertEqual((self.lines,self.detections),(1,0))
        self.assertTrue(q['local_title_ocr']['recognition_cache_hit'])
        self.assertIn('not_detector_boxes',q['local_title_ocr']['word_bounds_basis'])

    def test_direct_line_api_never_mutates_generic_detector_setting(self):
        class Engine:
            use_det=True
            def load_img(self,image):return image
            def recognize_txt(self,images):
                assert len(images)==1
                return SimpleNamespace(txts=['AUG'],scores=[.999])
        e=Engine();r=LocalTitleRecognizer(Path('.'),engine=e)
        r.recognize_line(Image.new('RGB',(530,54),'gray'))
        self.assertTrue(e.use_det)

    def test_invalid_option_rejected(self):
        for value in (1,None,'yes'):
            with self.assertRaisesRegex(ValueError,'FAST_OPTION'):self.reader(fast_title=value)


if __name__=='__main__':unittest.main()
