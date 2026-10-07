"""Collection-only selected-card association. No purchase command is defined."""
from decimal import Decimal,InvalidOperation
import re
import unicodedata
from collection_labels import exact_label_target
def _region(result,name):
 p=result.get('collection_observation',{})
 if p.get('same_frame') is not True or not result.get('frames') or p.get('frame_sha256')!=result['frames'][-1].get('sha256'):
  raise ValueError('COLLECTION_FRAME_BINDING')
 items=[r for r in p.get('regions',[]) if r.get('kind')==name]
 if len(items)!=1 or not items[0].get('ok') or items[0].get('truncated'):raise ValueError('COLLECTION_FIELD_ROI')
 return sorted(items[0]['words'],key=lambda x:x['x'])
def match_selected_first_card(result,rule):
 if not rule.get('enabled') or not rule.get('dictionary_resolved'):raise ValueError('COLLECTION_RULE_NOT_RESOLVED')
 if result.get('startup_page',{}).get('page')!='skin_listings' or result['startup_page']['overlay']!='none':raise ValueError('COLLECTION_PAGE')
 if not result.get('collection_selected_card',{}).get('selected'):raise ValueError('COLLECTION_SELECTED_CARD_UNPROVEN')
 if exact_label_target(result,'product_title',rule['product_name']) is None:raise ValueError('COLLECTION_PRODUCT_MISMATCH')
 fields=_region(result,'first_card_fields')
 index=result['collection_selected_card']['index']
 if type(index) is not int or not 0<=index<=5:raise ValueError('COLLECTION_CARD_INDEX')
 dx=(index%2)*877
 condition=''.join(unicodedata.normalize('NFKC',w['text']) for w in fields if 120+dx<=w['x']<200+dx)
 if not re.fullmatch('成色[SABC]',condition):raise ValueError('COLLECTION_CARD_CONDITION')
 candidates=[dict(w,text=unicodedata.normalize('NFKC',w['text'])) for w in fields if w['x']>=200+dx and w['height']<=22]
 if not candidates or not 972+dx<=candidates[-1]['x']+candidates[-1]['width']<=981+dx:raise ValueError('COLLECTION_PRICE_ALIGNMENT')
 # The amount is the right-aligned contiguous run. A tiny comma-like speck
 # on the currency artwork may precede it. Detached digits remain ambiguous;
 # do not silently discard a possible leading price digit.
 amount=[]
 for w in reversed(candidates):
  if amount and (amount[0]['x']-(w['x']+w['width'])>8 or abs(w['y']-amount[0]['y'])>4):break
  if not re.fullmatch(r'[0-9,.]+',w['text']):raise ValueError('COLLECTION_PRICE_TOKEN')
  amount.insert(0,w)
 for w in candidates[:len(candidates)-len(amount)]:
  if not (w['text']==',' and w['height']<=8 and amount[0]['x']-(w['x']+w['width'])>=5 and w['y']-amount[0]['y']>=6):
   raise ValueError('COLLECTION_PRICE_ASSOCIATION')
 price_text=''.join(w['text'] for w in amount)
 if not re.fullmatch(r'(?:0|[1-9][0-9]*|[1-9][0-9]{0,2}(?:,[0-9]{3})+)',price_text):raise ValueError('COLLECTION_PRICE_FORMAT')
 readings=[]
 primary_words=[w for w in _region(result,'selected_detail') if 2200<=w['x']<2370]
 kinds=['selected_detail']
 if any(r.get('kind')=='selected_detail_precise' for r in result['collection_observation']['regions']):kinds.append('selected_detail_precise')
 for kind in kinds:
  local=[w for w in _region(result,kind) if 2200<=w['x']<2370]
  detail=''.join(unicodedata.normalize('NFKC',w['text']) for w in local)
  detail=detail.replace('〔','(').replace('〕',')')
  if kind=='selected_detail_precise' and local and re.fullmatch(r'[0-9]+\.[0-9]{1,9}\)',detail):
   # English numeric OCR may omit A(. Recover only the actual, adjoining
   # prefix boxes from Chinese OCR on this same frame, never invent a dot.
   prefix=[w for w in primary_words if w['x']+w['width']<=local[0]['x']+1]
   prefix_text=''.join(unicodedata.normalize('NFKC',w['text']) for w in prefix).replace('〔','(')
   if re.fullmatch(r'[SABC]\(',prefix_text) and 0<=local[0]['x']-(prefix[-1]['x']+prefix[-1]['width'])<=14 and abs((prefix[-1]['y']+prefix[-1]['height']/2)-(local[0]['y']+local[0]['height']/2))<=6:
    detail=prefix_text+detail
  parsed=re.fullmatch(r'([SABC])\(([0-9]+\.[0-9]{1,9})\)',detail)
  if parsed:readings.append(parsed)
 if len({(m[1],m[2]) for m in readings})>1:raise ValueError('COLLECTION_WEAR_CONFLICT')
 match=readings[0] if readings else None
 if not match or '成色'+match[1]!=condition:raise ValueError('COLLECTION_WEAR_ASSOCIATION')
 if rule['condition_label'] not in (condition,'仅磨损'):raise ValueError('COLLECTION_CONDITION_MISMATCH')
 try:
  price=Decimal(price_text.replace(',',''));wear=Decimal(match[2])
  eligible=Decimal(rule['price_min'])<=price<=Decimal(rule['price_max']) and wear<=Decimal(rule['max_wear'])
 except (InvalidOperation,KeyError):raise ValueError('COLLECTION_RULE_NUMBER')
 return dict(eligible=eligible,product=rule['product_name'],condition=condition,price=str(price),wear=str(wear),
  row_index=rule['row_index'],source_frame_id=result['collection_observation']['frame_id'],
  source_frame_sha256=result['collection_observation']['frame_sha256'],purchase_phase_enabled=False)
