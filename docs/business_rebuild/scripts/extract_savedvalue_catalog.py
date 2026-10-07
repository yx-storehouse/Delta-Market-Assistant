"""Recover only public task dropdown data from an existing local dump.

Never starts/imports the target, and never exports unrelated memory strings.
The source QIML span is not a well-formed whole document: individual closed
combo_group fragments and the condition combo are parsed without repair.
"""
import hashlib
import json
import mmap
from pathlib import Path
import re
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[3]
def main():
 source=next(Path(r'C:\Users\Administrator\Desktop\ida').glob('*0925*.DMP'))
 offset,length=0x1071e04,30894
 with source.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
  raw=data[offset:offset+length]
 assert hashlib.sha256(raw).hexdigest()=='6b9a859568f078cd4088bc94dc60fb4ce5f9ccfbc18f3f63c693a967799085ba'
 text=raw.decode('utf-16le',errors='strict');products={}
 for match in re.finditer(r'<combo_group\b[^>]*>.*?</combo_group>',text,re.S):
  group=ET.fromstring(match.group());season=group.attrib['text'].split(':',1)[1].strip()
  for item in group.findall('combo_item'):
   identifier=item.attrib['val'];display=item.attrib['text'];name=display.split('|',1)[1].strip()
   assert identifier.isdecimal() and identifier not in products
   products[identifier]=dict(display_name=display,name=name,season_label=season,
    source_span_offset=offset+match.start()*2,source_group=group.attrib['text'])
 condition_fragment=re.search(r'<combo var="成色设置栏"[^>]*>.*?</combo>',text,re.S)
 assert condition_fragment
 conditions={str(i):n.attrib['text'] for i,n in enumerate(ET.fromstring(condition_fragment.group()).findall('combo_item'))}
 assert list(conditions.values())==['成色S','成色A','成色B','成色C','仅磨损']
 output=ROOT/'docs/business_rebuild/implementation/domain/relink_0925_catalog.json'
 data=dict(schema='relink-savedvalue-dictionary-v1',source_kind='existing_dump_public_dropdown_data',
  source_filename=source.name,source_size=source.stat().st_size,source_span_offset=offset,
  source_span_bytes=length,source_span_sha256=hashlib.sha256(raw).hexdigest(),
  specimen_executed=False,grade_inferred_from_color=False,products=products,conditions=conditions,
  collection_limit_zero_semantics='no_quantity_limit',collection_limit_evidence='默认0不限制数量(抢购数量限制 达量后不再收藏)')
 output.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 assert json.loads(output.read_text(encoding='utf-8'))==data
 print(f'SAVEDVALUE_DICTIONARY=PASS; products={len(products)}; conditions={len(conditions)}; specimen_executed=false')
if __name__=='__main__':main()
