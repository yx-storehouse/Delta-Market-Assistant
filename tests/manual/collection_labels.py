"""Exact bounded matching of same-frame UI field labels, no fuzzy characters."""
import math
import re
import unicodedata
import copy
def normalized(text):
 return re.sub(r'\s+','',unicodedata.normalize('NFKC',text))
def exact_label_target(result,region_name,label):
 primary=_exact_label_target(result,region_name,label)
 if region_name!='product_title':return primary
 regions=result.get('collection_observation',{}).get('regions',[])
 latin=[r for r in regions if r.get('kind')=='product_title_latin' and r.get('ok') and not r.get('truncated')]
 base=[r for r in regions if r.get('kind')=='product_title' and r.get('ok') and not r.get('truncated')]
 if len(latin)!=1 or len(base)!=1:return primary
 # A second language may recover the visibly present model digits omitted
 # by Chinese OCR. Only observed Latin tokens join the same-frame geometric
 # path; no guessed characters, substitutions, or catalogue-derived prefix.
 merged=copy.deepcopy(result)
 for r in merged['collection_observation']['regions']:
  if r['kind']=='product_title':
   r['words'] += [w for w in latin[0]['words'] if re.fullmatch(r'[A-Za-z0-9-]+',w.get('text',''))]
 return _exact_label_target(merged,region_name,label)

def _exact_label_target(result,region_name,label):
 projection=result.get('collection_observation',{})
 if projection.get('same_frame') is not True or not result.get('frames'):
  return None
 if projection.get('frame_sha256')!=result['frames'][-1].get('sha256'):return None
 regions=[x for x in projection.get('regions',[]) if x.get('kind')==region_name]
 if len(regions)!=1 or not regions[0].get('ok') or regions[0].get('truncated'):return None
 tokens=[]
 for w in regions[0].get('words',[]):
  if not isinstance(w.get('text'),str) or w.get('score',1)<.7:return None
  if any(type(w.get(k)) not in (int,float) or not math.isfinite(w[k]) for k in ['x','y','width','height']):return None
  if w['width']<=0 or w['height']<=0:return None
  text=normalized(w['text'])
  if text:tokens.append(dict(w,text=text,match_height=w['height']))
 if len(tokens)>256:return None
 # Windows OCR reads the UI's short separator stroke as Chinese 一, or the
 # larger title separator as ·. Normalize only a narrow, low-height stroke
 # bracketed by real text on the same line. A full-width Chinese 一 remains
 # a Chinese character; arbitrary product spelling is never repaired.
 for t in tokens:
  if t['text'] not in ('-','一','·','—','–','−'):continue
  neighbours=[s for s in tokens if s is not t and s['height']>=8
    and abs((s['y']+s['height']/2)-(t['y']+t['height']/2))<=.4*s['height']
    and abs((s['x']+s['width']/2)-(t['x']+t['width']/2))<=3*s['height']]
  left=[s for s in neighbours if s['x']+s['width']<=t['x']+1]
  right=[s for s in neighbours if s['x']>=t['x']+t['width']-1]
  if not left or not right:continue
  font=min(min(left,key=lambda s:abs(s['x']+s['width']-t['x']))['height'],min(right,key=lambda s:abs(s['x']-t['x']-t['width']))['height'])
  if t['height']<=.35*font and t['width']<=.75*font:
   t['text']='-';t['match_height']=font
 desired=normalized(label);matches=[];budget=[8192]
 def follow(path,text):
  if text==desired:
   a=min(w['x'] for w in path);b=min(w['y'] for w in path)
   c=max(w['x']+w['width'] for w in path);d=max(w['y']+w['height'] for w in path)
   matches.append((round((a+c)/2),round((b+d)/2)));return
  if len(path)>=16:return
  last=path[-1]
  for t in tokens:
   budget[0]-=1
   if budget[0]<0:return
   if t['x']<=last['x']:continue
   font=min(t['match_height'],last['match_height']);gap=t['x']-(last['x']+last['width'])
   if gap<-.3*font or gap>.9*font or abs(t['y']+t['height']/2-last['y']-last['height']/2)>.5*font:continue
   if desired.startswith(text+t['text']):follow(path+[t],text+t['text'])
 for t in tokens:
  if desired.startswith(t['text']):follow([t],t['text'])
 if budget[0]<0:return None
 # Different tokenization of the same bounds is one target; distinct rows
 # or duplicated exact controls stay ambiguous instead of taking the first.
 distinct=[]
 for point in matches:
  if not any(abs(point[0]-p[0])<=3 and abs(point[1]-p[1])<=3 for p in distinct):distinct.append(point)
 return distinct[0] if len(distinct)==1 else None
