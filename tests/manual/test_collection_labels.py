import json
from pathlib import Path
import unittest
from collection_labels import exact_label_target
ROOT=Path(__file__).resolve().parents[2]
def observation(words):
 return {'frames':[{'sha256':'a'*64}],'collection_observation':{'same_frame':True,'frame_sha256':'a'*64,
  'regions':[{'kind':'catalog_names','ok':True,'truncated':False,'words':words}]}}
def word(text,x,y=10,w=20,h=20):return dict(text=text,x=x,y=y,width=w,height=h)
class Labels(unittest.TestCase):
 def test_exact_name_and_stroke(self):
  words=[word('AUG',10,w=40),word('枪',52),word('一',74,y=19,w=7,h=2),word('天',84),word('命',106)]
  self.assertIsNotNone(exact_label_target(observation(words),'catalog_names','AUG枪-天命'))
  self.assertIsNone(exact_label_target(observation(words),'catalog_names','AUG枪-天明'))
 def test_fullwidth_chinese_one_not_rewritten(self):
  words=[word('枪',10),word('一',32,y=19,w=20,h=2),word('年',54)]
  self.assertIsNone(exact_label_target(observation(words),'catalog_names','枪-年'))
 def test_same_frame_latin_title_uses_observed_digits(self):
  o=observation([word('P',10,w=12),word('枪',52)])
  o['collection_observation']['regions'][0]['kind']='product_title'
  o['collection_observation']['regions'].append(dict(kind='product_title_latin',ok=True,truncated=False,words=[word('P90',10,w=40)]))
  self.assertIsNotNone(exact_label_target(o,'product_title','P90枪'))
  self.assertIsNone(exact_label_target(o,'product_title','P9'))
  o['collection_observation']['frame_sha256']='b'*64
  self.assertIsNone(exact_label_target(o,'product_title','P90枪'))
 def test_duplicate_and_stale(self):
  o=observation([word('AUG',10,w=40),word('AUG',10,y=60,w=40)])
  self.assertIsNone(exact_label_target(o,'catalog_names','AUG'))
  o=observation([word('AUG',10,w=40)]);o['collection_observation']['frame_sha256']='b'*64
  self.assertIsNone(exact_label_target(o,'catalog_names','AUG'))
 def test_projection_failures(self):
  for key,value in [('ok',False),('truncated',True)]:
   o=observation([word('AUG',10,w=40)]);o['collection_observation']['regions'][0][key]=value
   self.assertIsNone(exact_label_target(o,'catalog_names','AUG'))
 def test_recorded_catalogue(self):
  path=ROOT/'artifacts/m2_savedvalue_collection/live_first_product_v2.json'
  if not path.exists():self.skipTest('Local calibration record not present')
  data=json.loads(path.read_text(encoding='utf-8'));o=data['steps'][-1]['result']
  for name in ['AUG突击步枪-天命','P90冲锋枪-天命','SR-25射手步枪-天命']:
   self.assertIsNotNone(exact_label_target(o,'catalog_names',name),name)
if __name__=='__main__':unittest.main(verbosity=2)
