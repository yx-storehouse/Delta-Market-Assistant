import copy
import json
from pathlib import Path
import unittest
from collection_candidate import match_selected_first_card
ROOT=Path(__file__).resolve().parents[2]
class Candidate(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  p=ROOT/'artifacts/m2_savedvalue_collection/live_condition_s_v2.json'
  if not p.exists():raise unittest.SkipTest('local calibration packet absent')
  cls.packet=json.loads(p.read_text(encoding='utf-8'))['steps'][-1]['result']
  cls.rule=dict(enabled=True,dictionary_resolved=True,product_name='AUG突击步枪-天命',condition_label='成色S',price_min='10',price_max='600',max_wear='5',row_index=0)
 def test_actual_selected_association(self):
  r=match_selected_first_card(self.packet,self.rule);self.assertTrue(r['eligible']);self.assertEqual(r['price'],'230');self.assertEqual(r['wear'],'0.187079')
 def test_boundaries(self):
  for key,val,eligible in [('price_min','231',False),('price_max','229',False),('price_max','230',True),('max_wear','0.187078',False),('max_wear','0.187079',True)]:
   rule=dict(self.rule);rule[key]=val;self.assertEqual(match_selected_first_card(self.packet,rule)['eligible'],eligible)
 def test_detached_digit_is_not_silently_discarded(self):
  p=copy.deepcopy(self.packet)
  for r in p['collection_observation']['regions']:
   if r['kind']=='first_card_fields':r['words'].append(dict(text='8',x=909,y=538,width=15,height=19))
  with self.assertRaises(ValueError):match_selected_first_card(p,self.rule)
 def test_recorded_currency_speck_is_not_price(self):
  p=copy.deepcopy(self.packet)
  for r in p['collection_observation']['regions']:
   if r['kind']=='first_card_fields':r['words'].append(dict(text='，',x=928,y=548,width=2,height=6))
  self.assertEqual(match_selected_first_card(p,self.rule)['price'],'230')
 def test_alternate_wear_must_observe_decimal_and_agree(self):
  p=copy.deepcopy(self.packet)
  detail=next(r for r in p['collection_observation']['regions'] if r['kind']=='selected_detail')
  alternate=copy.deepcopy(detail);alternate['kind']='selected_detail_precise'
  p['collection_observation']['regions'].append(alternate)
  detail['words']=[w for w in detail['words'] if w['text']!='．']
  self.assertEqual(match_selected_first_card(p,self.rule)['wear'],'0.187079')
  alternate['words']=[w for w in alternate['words'] if w['text']!='．']
  with self.assertRaises(ValueError):match_selected_first_card(p,self.rule)
 def test_same_frame_prefix_and_observed_numeric_suffix(self):
  p=copy.deepcopy(self.packet)
  detail=next(r for r in p['collection_observation']['regions'] if r['kind']=='selected_detail')
  detail['words']=[w for w in detail['words'] if w['text']!='．']
  p['collection_observation']['regions'].append(dict(kind='selected_detail_precise',ok=True,truncated=False,words=[dict(text='0.187079)',x=2244,y=660,width=111,height=20)]))
  self.assertEqual(match_selected_first_card(p,self.rule)['wear'],'0.187079')
  detail['words']=[w for w in detail['words'] if w['text']!='〔']
  with self.assertRaises(ValueError):match_selected_first_card(p,self.rule)
 def test_wrong_product_condition_and_selection(self):
  for key,value in [('product_name','P90冲锋枪-天命'),('condition_label','成色A'),('enabled',False),('dictionary_resolved',False)]:
   rule=dict(self.rule);rule[key]=value
   with self.assertRaises(ValueError):match_selected_first_card(self.packet,rule)
  p=copy.deepcopy(self.packet);p['collection_selected_card']['selected']=False
  with self.assertRaises(ValueError):match_selected_first_card(p,self.rule)
 def test_stale_binding_and_missing_digits(self):
  p=copy.deepcopy(self.packet);p['collection_observation']['frame_sha256']='other'
  with self.assertRaises(ValueError):match_selected_first_card(p,self.rule)
  p=copy.deepcopy(self.packet)
  for r in p['collection_observation']['regions']:
   if r['kind']=='first_card_fields':r['words'][-1]['text']='0230'
  with self.assertRaises(ValueError):match_selected_first_card(p,self.rule)
if __name__=='__main__':unittest.main(verbosity=2)
