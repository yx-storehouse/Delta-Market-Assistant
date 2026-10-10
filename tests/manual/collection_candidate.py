"""Collection-only selected-card association. No purchase command is defined."""
from decimal import Decimal,InvalidOperation
import re
import unicodedata
import math
import hashlib
import json
from copy import deepcopy
from collection_labels import exact_label_target
from collection_scroll import observe_layout, same_card_geometry, layouts_equivalent
def _region(result,name):
 p=result.get('collection_observation',{})
 if p.get('same_frame') is not True or not result.get('frames') or p.get('frame_sha256')!=result['frames'][-1].get('sha256'):
  raise ValueError('COLLECTION_FRAME_BINDING')
 items=[r for r in p.get('regions',[]) if r.get('kind')==name]
 if len(items)!=1 or not items[0].get('ok') or items[0].get('truncated'):raise ValueError('COLLECTION_FIELD_ROI')
 if (('same_frame' in items[0] and items[0]['same_frame'] is not True)
  or any(k in items[0] and items[0][k]!=p.get(k) for k in ('frame_id','frame_sha256'))):
  raise ValueError('COLLECTION_FRAME_BINDING')
 return sorted(items[0]['words'],key=lambda x:x['x'])

def _inside(word,box,tolerance=1):
 if any(type(word.get(k)) not in (int,float) or not math.isfinite(word[k]) for k in ('x','y','width','height')):return False
 return (word['width']>0 and word['height']>0 and word['x']>=box[0]-tolerance and word['y']>=box[1]-tolerance
  and word['x']+word['width']<=box[0]+box[2]+tolerance and word['y']+word['height']<=box[1]+box[3]+tolerance)

def _hash(value):
 return isinstance(value,str) and len(value)==64 and all(c in '0123456789abcdef' for c in value)

def _rect(value):
 if not isinstance(value,(list,tuple)) or len(value)!=4 or any(type(v) is not int for v in value):
  raise ValueError('COLLECTION_RECEIPT_PROOF_GEOMETRY')
 if min(value[:2])<0 or min(value[2:])<=0:raise ValueError('COLLECTION_RECEIPT_PROOF_GEOMETRY')
 return list(value)

def _contains(outer,inner):
 return outer[0]<=inner[0] and outer[1]<=inner[1] and inner[0]+inner[2]<=outer[0]+outer[2] and inner[1]+inner[3]<=outer[1]+outer[3]

def receipt_reference(before):
 """Only observed pre-dispatch geometry; no inferred coordinates or new target."""
 if not isinstance(before,dict) or before.get('geometry_mode')!='observed_dynamic':
  raise ValueError('COLLECTION_RECEIPT_REFERENCE')
 reference=dict(frame_id=before.get('source_frame_id'),frame_sha256=before.get('source_frame_sha256'),
  card_id=before.get('card_id'),bounds=_rect(before.get('card_bounds')),fields_bounds=_rect(before.get('fields_bounds')))
 if (not isinstance(reference['frame_id'],str) or not reference['frame_id'] or not _hash(reference['frame_sha256'])
  or not isinstance(reference['card_id'],str) or not reference['card_id'] or len(reference['card_id'])>128
  or not _contains(reference['bounds'],reference['fields_bounds'])):raise ValueError('COLLECTION_RECEIPT_REFERENCE')
 return reference

def validate_receipt_geometry(result,before):
 """Validate one current measured card for an already-dispatched receipt only.

 This proof does NOT validate the visible list, establish global uniqueness,
 or yield a card-selection lease. Normal action parsing keeps observe_layout.
 """
 reference=receipt_reference(before)
 layout=result.get('collection_layout',{})
 projection=result.get('collection_observation',{})
 frames=result.get('frames')
 proof=layout.get('selected_card_proof',{})
 if (layout.get('schema')!='collection-layout-v1' or layout.get('detector')!='visible_edges'
  or layout.get('complete') is not False or not isinstance(proof,dict)
  or proof.get('schema')!='collection-selected-card-proof-v1' or proof.get('scope')!='receipt_only'
  or proof.get('proven') is not True or proof.get('actions_enabled') is not False
  or proof.get('unique_selection_proven') is not False or proof.get('local_selection_conflict_checked') is not True
  or proof.get('local_selection_check_scope')!='reference_horizontal_band_and_peer_column_only'
  or proof.get('unresolved_regions_may_contain_selection') is not True
  or proof.get('error') or proof.get('local_conflicting_card')):raise ValueError('COLLECTION_RECEIPT_PROOF_UNPROVEN')
 if proof.get('prior_card_reference')!=reference:raise ValueError('COLLECTION_RECEIPT_REFERENCE_BINDING')
 if not isinstance(frames,list) or not frames or not isinstance(frames[-1],dict):raise ValueError('COLLECTION_RECEIPT_PROOF_FRAME')
 frame=frames[-1];frame_id=projection.get('frame_id');digest=projection.get('frame_sha256')
 viewport=[frame.get('width'),frame.get('height')]
 if (not isinstance(frame_id,str) or not frame_id or not _hash(digest) or frame.get('sha256')!=digest
  or projection.get('same_frame') is not True or layout.get('same_frame') is not True or proof.get('same_frame') is not True
  or any(part.get('frame_id')!=frame_id or part.get('frame_sha256')!=digest for part in (layout,proof))
  or any(type(v) is not int or not 1<=v<=16384 for v in viewport) or layout.get('viewport')!=viewport):
  raise ValueError('COLLECTION_RECEIPT_PROOF_FRAME')
 if frame_id==reference['frame_id'] or digest==reference['frame_sha256']:raise ValueError('COLLECTION_REOBSERVATION_REQUIRED')
 screen=[0,0,*viewport]
 if 'listing_viewport' in layout and not _contains(screen,_rect(layout['listing_viewport'])):
  raise ValueError('COLLECTION_RECEIPT_PROOF_GEOMETRY')
 card=proof.get('card')
 if not isinstance(card,dict):raise ValueError('COLLECTION_RECEIPT_PROOF_GEOMETRY')
 if (card.get('selected') is not True or card.get('bounds_basis')!='observed'
  or not isinstance(card.get('id'),str) or not card['id'] or len(card['id'])>128
  or not isinstance(card.get('edges'),dict) or set(card['edges'])!={'top','bottom','left','right'}
  or any(value is not True for value in card['edges'].values())):raise ValueError('COLLECTION_RECEIPT_PROOF_GEOMETRY')
 for key in ('bounds','fields_bounds','condition_bounds','price_bounds'):_rect(card.get(key))
 bounds,fields=card['bounds'],card['fields_bounds']
 if (not _contains(screen,bounds) or not _contains(bounds,fields) or fields[1]-bounds[1]<=8 or bounds[2]<=8
  or any(not _contains(fields,card[key]) for key in ('condition_bounds','price_bounds'))):
  raise ValueError('COLLECTION_RECEIPT_PROOF_GEOMETRY')
 condition,price=card['condition_bounds'],card['price_bounds']
 if (max(condition[0],price[0])<min(condition[0]+condition[2],price[0]+price[2])
  and max(condition[1],price[1])<min(condition[1]+condition[3],price[1]+price[3])):
  raise ValueError('COLLECTION_RECEIPT_PROOF_GEOMETRY')
 if not same_card_geometry(before,card):raise ValueError('COLLECTION_RECEIPT_GEOMETRY_CONFLICT')
 selected=result.get('collection_selected_card',{})
 if (selected.get('scope')!='receipt_only' or selected.get('unique_selection_proven') is not False
  or selected.get('actions_enabled') is not False):raise ValueError('COLLECTION_RECEIPT_PROOF_SCOPE')
 identity=dict(frame_id=frame_id,frame_sha256=digest,layout_id='receipt-only:'+hashlib.sha256(
  json.dumps(proof,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode('utf-8')).hexdigest())
 return deepcopy(card),identity

def _finite_number(value):
 return type(value) in (int,float) and math.isfinite(value)

def validate_pixel_receipt(result,before,basis=None):
 """Bind a pixel-only receipt frame to the dispatched candidate's own card.

 No text is reread: in a NEW frame the selected card must keep the candidate's
 pre-dispatch geometry and the fixed detail star must have turned from the
 recorded white to gold. Product/condition/price/wear remain those read from
 the candidate's own selection frame, and the caller records that basis.
 ``basis`` (that selection packet) decides only whether the whole visible list
 is unchanged, so its card geometry may still be used for the next input.
 """
 receipt=result.get('collection_pixel_receipt',{})
 if (result.get('capture_passed') is not True or result.get('error') or not isinstance(receipt,dict)
  or receipt.get('schema')!='collection-pixel-receipt-v1' or receipt.get('ready') is not True
  or receipt.get('ocr_performed') is not False or result.get('focus_activation_requests')!=0
  or result.get('focus_restore_requests')!=0 or result.get('target_foreground_retained') is not True):
  raise ValueError('COLLECTION_PIXEL_RECEIPT_NOT_READY')
 reference=receipt_reference(before)
 if receipt.get('reference')!=reference:raise ValueError('COLLECTION_RECEIPT_REFERENCE_BINDING')
 frames=result.get('frames');projection=result.get('collection_observation',{})
 layout=result.get('collection_layout',{});selected=result.get('collection_selected_card',{})
 if not isinstance(frames,list) or not frames or not isinstance(frames[-1],dict):raise ValueError('COLLECTION_RECEIPT_PROOF_FRAME')
 frame=frames[-1];frame_id=receipt.get('frame_id');digest=receipt.get('frame_sha256')
 if (not isinstance(frame_id,str) or not frame_id or not _hash(digest) or frame.get('sha256')!=digest
  or not all(isinstance(part,dict) and part.get('frame_id')==frame_id and part.get('frame_sha256')==digest
             and part.get('same_frame') is True for part in (projection,selected,layout))):
  raise ValueError('COLLECTION_RECEIPT_PROOF_FRAME')
 if frame_id==before.get('source_frame_id') or digest==before.get('source_frame_sha256'):
  raise ValueError('COLLECTION_REOBSERVATION_REQUIRED')
 original=before.get('favorite_before',{})
 if not (isinstance(original,dict) and original.get('favorite_warm_fraction')==0
  and _finite_number(original.get('favorite_bright_fraction')) and original['favorite_bright_fraction']>=.04):
  raise ValueError('COLLECTION_RECEIPT_TRANSITION_UNPROVEN')
 warm,bright=selected.get('favorite_warm_fraction'),selected.get('favorite_bright_fraction')
 if not (_finite_number(warm) and _finite_number(bright) and .05<=warm<=1 and 0<=bright<=.02):
  raise ValueError('COLLECTION_RECEIPT_STAR_NOT_GOLD')
 if layout.get('complete') is True:
  current=observe_layout(result,require_page=False)
  cards=[card for card in current['cards'] if card['selected']]
  if len(cards)!=1 or not cards[0]['selectable']:raise ValueError('COLLECTION_DYNAMIC_SELECTION_UNPROVEN')
  card=cards[0]
  if (selected.get('selected') is not True or selected.get('card_id')!=card['id']
   or selected.get('bounds')!=card['bounds'] or selected.get('fields_bounds')!=card['fields_bounds']):
   raise ValueError('COLLECTION_DYNAMIC_SELECTED_BINDING')
  if not same_card_geometry(before,card):raise ValueError('COLLECTION_RECEIPT_GEOMETRY_CONFLICT')
  unchanged=False
  if basis is not None:
   try:unchanged=layouts_equivalent(observe_layout(basis),current)
   except (ValueError,KeyError,TypeError):unchanged=False
  scope,layout_id='visible_layout',current['layout_id']
 else:
  card,identity=validate_receipt_geometry(result,before)
  unchanged,scope,layout_id=False,'receipt_only',identity['layout_id']
 return dict(card=deepcopy(card),frame_id=frame_id,frame_sha256=digest,favorite_warm_fraction=warm,
  favorite_bright_fraction=bright,scope=scope,layout_id=layout_id,layout_unchanged=unchanged,
  source_mono_ms=receipt.get('source_mono_ms'),text_reread=False)

def pixel_receipt_record(before,validated):
 """Journal receipt for a pixel-confirmed favorite; identity is the candidate's."""
 card=validated['card']
 return dict(schema='collection-pixel-receipt-record-v1',kind='pixel_same_card_white_to_gold',
  product=before['product'],condition=before['condition'],price=before['price'],wear=before['wear'],
  row_index=before['row_index'],identity_basis='pre_dispatch_selection_frame_candidate',
  identity_frame_id=before['source_frame_id'],identity_frame_sha256=before['source_frame_sha256'],
  source_frame_id=validated['frame_id'],source_frame_sha256=validated['frame_sha256'],
  card_id=card['id'],card_bounds=card['bounds'],fields_bounds=card['fields_bounds'],
  favorite_after=dict(favorite_warm_fraction=validated['favorite_warm_fraction'],
                      favorite_bright_fraction=validated['favorite_bright_fraction']),
  scope=validated['scope'],layout_id=validated['layout_id'],layout_unchanged=validated['layout_unchanged'],
  text_reread=False,purchase_phase_enabled=False)

def _bound_fields(result,card,layout,*,receipt_only=False):
 selected=result.get('collection_selected_card',{})
 if (selected.get('selected') is not True or selected.get('same_frame') is not True
  or selected.get('card_id')!=card['id'] or selected.get('frame_id')!=layout['frame_id']
  or selected.get('frame_sha256')!=layout['frame_sha256'] or selected.get('bounds')!=card['bounds']
  or selected.get('fields_bounds')!=card['fields_bounds']):raise ValueError('COLLECTION_DYNAMIC_SELECTED_BINDING')
 if 'condition_bounds' not in card or 'price_bounds' not in card:raise ValueError('COLLECTION_DYNAMIC_FIELD_ROI')
 rows=[r for r in result['collection_observation'].get('regions',[]) if r.get('kind')=='card_fields' and r.get('card_id')==card['id']]
 if len(rows)!=1 or rows[0].get('ok') is not True or rows[0].get('truncated') is not False or rows[0].get('bounds')!=card['fields_bounds']:
  raise ValueError('COLLECTION_DYNAMIC_FIELD_BINDING')
 if receipt_only:
  if rows[0].get('scope')!='receipt_only':raise ValueError('COLLECTION_RECEIPT_PROOF_SCOPE')
 elif rows[0].get('scope')=='receipt_only' or selected.get('scope')=='receipt_only':
  raise ValueError('COLLECTION_RECEIPT_PROOF_SCOPE')
 fields=rows[0].get('words')
 if not isinstance(fields,list) or not 1<=len(fields)<=256 or any(not isinstance(w,dict) or not isinstance(w.get('text'),str) or not _inside(w,card['fields_bounds']) for w in fields):
  raise ValueError('COLLECTION_DYNAMIC_FIELD_WORDS')
 return sorted(fields,key=lambda w:w['x']),card,layout

def _dynamic_fields(result,*,require_page=True):
 layout=observe_layout(result,require_page=require_page)
 cards=[card for card in layout['cards'] if card['selected']]
 if len(cards)!=1 or not cards[0]['selectable']:raise ValueError('COLLECTION_DYNAMIC_SELECTION_UNPROVEN')
 return _bound_fields(result,cards[0],layout)

def _condition_reading(words):
 """Case-normalize one observed ASCII grade after a literal Chinese prefix.

 No NFKC/casefold is applied before validation: full-width, long-s, Cyrillic,
 digit substitutions, missing prefixes and extra letters remain unknown.
 """
 raw=''.join(word['text'] for word in words)
 parsed=re.fullmatch(r'成色([sSaAbBcC])',raw)
 if not parsed:raise ValueError('COLLECTION_CARD_CONDITION')
 grade=parsed[1]
 canonical_grade={'s':'S','a':'A','b':'B','c':'C'}.get(grade,grade)
 condition='成色'+canonical_grade
 return condition,dict(method='literal_prefix_ascii_grade_case',raw_text=raw,
  raw_words=deepcopy(words),raw_grade=grade,canonical_grade=canonical_grade,
  canonical_text=condition,case_changed=grade!=canonical_grade)

def _wear_text(words):
 return ''.join(unicodedata.normalize('NFKC',w['text']) for w in words).replace('〔','(').replace('〕',')')

def _wear_prefix_reading(primary,english):
 """Use measured Chinese grade/bracket plus an observed English decimal tail.

 An English nonnumeric prefix is discardable only inside the Chinese prefix
 union (+/-2px). No word containing a digit can be discarded or split; no C
 is translated into a bracket, and no decimal point is invented.
 """
 for start in range(len(english)):
  tail=english[start:];tail_text=_wear_text(tail)
  if not re.fullmatch(r'[0-9]+\.[0-9]{1,9}\)',tail_text):continue
  excluded=english[:start]
  if any(any(char.isdigit() for char in _wear_text([word])) for word in excluded):
   raise ValueError('COLLECTION_WEAR_PREFIX_DIGIT')
  if any(not _inside(w,[2200,0,170,16384],tolerance=0) for w in primary+english):return None
  prefix=[w for w in primary if w['x']+w['width']<=tail[0]['x']+1]
  prefix_text=_wear_text(prefix)
  if not re.fullmatch(r'[SABC]\(',prefix_text):return None
  left=min(w['x'] for w in prefix);top=min(w['y'] for w in prefix)
  right=max(w['x']+w['width'] for w in prefix);bottom=max(w['y']+w['height'] for w in prefix)
  union=[left,top,right-left,bottom-top]
  if any(not _inside(word,union,tolerance=2) for word in excluded):return None
  excluded_text=_wear_text(excluded).strip().upper()
  # A visibly contradictory grade at the beginning is not mere punctuation.
  if excluded_text and excluded_text[0] in 'SABC' and excluded_text[0]!=prefix_text[0]:
   raise ValueError('COLLECTION_WEAR_CONFLICT')
  gap=tail[0]['x']-(prefix[-1]['x']+prefix[-1]['width'])
  center=prefix[-1]['y']+prefix[-1]['height']/2
  if not 0<=gap<=14 or any(abs(w['y']+w['height']/2-center)>6 for w in tail):return None
  if any(not -2<=b['x']-(a['x']+a['width'])<=14 for a,b in zip(tail,tail[1:])):return None
  # If Chinese OCR observed digits but lost the dot, those digits must still
  # agree. A truncated or conflicting English numeric tail is not a repair.
  primary_digits=''.join(char for char in _wear_text(primary) if char.isdigit())
  english_digits=''.join(char for char in tail_text if char.isdigit())
  if primary_digits and primary_digits!=english_digits:raise ValueError('COLLECTION_WEAR_CONFLICT')
  detail=prefix_text+tail_text
  return detail,dict(method='chinese_prefix_english_decimal',prefix_region='selected_detail',
   decimal_region='selected_detail_precise',prefix_words=deepcopy(prefix),decimal_words=deepcopy(tail),
   excluded_english_prefix=deepcopy(excluded),prefix_union=union,prefix_union_tolerance_px=2,
   prefix_gap_px=gap,baseline_tolerance_px=6,digit_sequence_agreed=bool(primary_digits),text=detail)
 return None

def match_selected_first_card(result,rule,*,receipt_before=None,receipt_only=False):
 if result.get('startup_page',{}).get('page')!='skin_listings' or result['startup_page']['overlay']!='none':raise ValueError('COLLECTION_PAGE')
 return _match_selected_fields(result,rule,receipt_before=receipt_before,receipt_only=receipt_only)

def match_watchlist_selected(result,rule):
 """Read-only reuse of the original price/wear/condition matcher, not a lease.

 No fabricated skin_listings page and no first-card fallback. The native
 purchase probe must bind the same full-OCR frame to a measured selection.
 """
 if result.get('startup_page',{}).get('page')!='watchlist_listings' or result['startup_page'].get('overlay')!='none':raise ValueError('PURCHASE_WATCHLIST_PAGE')
 p=result.get('purchase_observation',{});projection=result.get('collection_observation',{})
 if (p.get('valid') is not True or p.get('actions_enabled') is not False
  or p.get('purchase_authorized') is not False or p.get('same_frame') is not True
  or any(p.get(k)!=projection.get(k) for k in ('frame_id','frame_sha256'))
  or 'collection_layout' not in result):raise ValueError('PURCHASE_OBSERVATION_REQUIRED')
 output=_match_selected_fields(result,rule,watchlist=True)
 output.update(scope='purchase_readonly_candidate',actions_enabled=False,identity_kind='visual_association_not_server_listing_id')
 return output

def _match_selected_fields(result,rule,*,receipt_before=None,receipt_only=False,watchlist=False):
 if not rule.get('enabled') or not rule.get('dictionary_resolved'):raise ValueError('COLLECTION_RULE_NOT_RESOLVED')
 if not result.get('collection_selected_card',{}).get('selected'):raise ValueError('COLLECTION_SELECTED_CARD_UNPROVEN')
 if exact_label_target(result,'product_title',rule['product_name']) is None:raise ValueError('COLLECTION_PRODUCT_MISMATCH')
 dynamic='collection_layout' in result
 card=layout=None
 if dynamic:
  if receipt_only:
   card,layout=validate_receipt_geometry(result,receipt_before)
   fields,card,layout=_bound_fields(result,card,layout,receipt_only=True)
  else:fields,card,layout=_dynamic_fields(result,require_page=not watchlist)
  condition_words=[w for w in fields if _inside(w,card['condition_bounds'])]
  # Dynamic price OCR is English 3x; its mapped glyph boxes can exceed the
  # legacy Chinese 2x 22px limit. Bound them to the observed same-frame strip,
  # not the legacy grid. Oversize or detached tokens still remain unassociated.
  price_height_limit=card['price_bounds'][3]*0.7
  price_words=[w for w in fields if _inside(w,card['price_bounds']) and w['height']<=price_height_limit]
  if any(w not in condition_words and w not in price_words and unicodedata.normalize('NFKC',w['text']).strip() for w in fields):
   raise ValueError('COLLECTION_DYNAMIC_UNASSOCIATED_FIELD')
  price_right=card['price_bounds'][0]+card['price_bounds'][2]
  right_min,right_max=price_right-15,price_right+1
 else:
  if receipt_only:raise ValueError('COLLECTION_RECEIPT_PROOF_UNPROVEN')
  fields=_region(result,'first_card_fields')
  index=result['collection_selected_card']['index']
  if type(index) is not int or not 0<=index<=5:raise ValueError('COLLECTION_CARD_INDEX')
  dx=(index%2)*877
  condition_words=[w for w in fields if 120+dx<=w['x']<200+dx]
  price_words=[w for w in fields if w['x']>=200+dx and w['height']<=22]
  right_min,right_max=972+dx,981+dx
 condition,condition_evidence=_condition_reading(condition_words)
 readings=[];wear_readings=[]
 primary_words=[w for w in _region(result,'selected_detail') if 2200<=w['x']<2370]
 kinds=['selected_detail']
 if any(r.get('kind')=='selected_detail_precise' for r in result['collection_observation']['regions']):kinds.append('selected_detail_precise')
 for kind in kinds:
  local=[w for w in _region(result,kind) if 2200<=w['x']<2370]
  detail=_wear_text(local)
  source=dict(method='direct',region=kind,words=deepcopy(local),text=detail)
  if kind=='selected_detail_precise' and not re.fullmatch(r'([SABC])\(([0-9]+\.[0-9]{1,9})\)',detail):
   fused=_wear_prefix_reading(primary_words,local)
   if fused is not None:detail,source=fused
  parsed=re.fullmatch(r'([SABC])\(([0-9]+\.[0-9]{1,9})\)',detail)
  if parsed:readings.append(parsed);wear_readings.append(source)
 if len({(m[1],m[2]) for m in readings})>1:raise ValueError('COLLECTION_WEAR_CONFLICT')
 match=readings[0] if readings else None
 if not match or '成色'+match[1]!=condition:raise ValueError('COLLECTION_WEAR_ASSOCIATION')
 if rule['condition_label'] not in (condition,'仅磨损'):raise ValueError('COLLECTION_CONDITION_MISMATCH')
 # Check available identity before classifying a missing price as transient.
 # The missing numeric value is never borrowed from the pending receipt.
 if receipt_before is not None:
  identity=dict(product=rule['product_name'],condition=condition,wear=str(Decimal(match[2])),row_index=rule['row_index'])
  if any(receipt_before.get(k)!=v for k,v in identity.items()):raise ValueError('COLLECTION_RECEIPT_IDENTITY_CONFLICT')
  if receipt_before.get('geometry_mode')=='observed_dynamic':
   if not dynamic or not same_card_geometry(receipt_before,card):raise ValueError('COLLECTION_RECEIPT_GEOMETRY_CONFLICT')
 candidates=[dict(w,text=unicodedata.normalize('NFKC',w['text'])) for w in price_words]
 if not candidates:raise ValueError('COLLECTION_PRICE_MISSING')
 if not right_min<=candidates[-1]['x']+candidates[-1]['width']<=right_max:raise ValueError('COLLECTION_PRICE_ALIGNMENT')
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
 try:
  price=Decimal(price_text.replace(',',''));wear=Decimal(match[2])
  eligible=Decimal(rule['price_min'])<=price<=Decimal(rule['price_max']) and wear<=Decimal(rule['max_wear'])
 except (InvalidOperation,KeyError):raise ValueError('COLLECTION_RULE_NUMBER')
 output=dict(eligible=eligible,product=rule['product_name'],condition=condition,price=str(price),wear=str(wear),
  row_index=rule['row_index'],source_frame_id=result['collection_observation']['frame_id'],
  source_frame_sha256=result['collection_observation']['frame_sha256'],purchase_phase_enabled=False)
 output['wear_source']=wear_readings[0]['method']
 output['condition_evidence']=dict(condition_evidence,
  region='card_fields' if dynamic else 'first_card_fields',
  frame_id=result['collection_observation']['frame_id'],
  frame_sha256=result['collection_observation']['frame_sha256'],
  independent_detail_condition='成色'+match[1])
 output['wear_evidence']=dict(frame_id=result['collection_observation']['frame_id'],
  frame_sha256=result['collection_observation']['frame_sha256'],readings=wear_readings)
 if dynamic:output.update(card_id=card['id'],card_bounds=card['bounds'],fields_bounds=card['fields_bounds'],source_layout_id=layout['layout_id'],geometry_mode='observed_dynamic')
 if receipt_only:output.update(geometry_mode='receipt_only_observed_dynamic',scope='receipt_only',
  unique_selection_proven=False,actions_enabled=False,viewport_complete=False)
 return output

match_selected_card=match_selected_first_card
