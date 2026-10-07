"""Build a reference skin table from existing IDs and user-supplied menu images.

This does not change executable resources, task rules, or game state. Menu
color, textual variant, and sale-listing condition remain separate fields.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
from PIL import Image

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'docs/business_rebuild/skin_reference'
CATALOG=ROOT/'docs/business_rebuild/implementation/domain/relink_0925_catalog.json'
ATTACHMENTS=Path(r'C:\Users\Administrator\AppData\Local\Temp')
SOURCES={
 1:(7,'dfec096f'),2:(8,'bb8d8145'),3:(9,'3017e314'),4:(10,'fbe4ea51'),
 5:(11,'4a2b4f26'),6:(1,'6129f04b'),7:(2,'b84e7475'),8:(3,'b117ad99'),
 9:(4,'d8bcc0c8'),10:(5,'4faef66b'),11:(6,'bf57ee57')}
LABELS={'red':'红色','orange':'橙色','purple':'紫色','blue':'蓝色'}

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def row_color(image,box):
    counts=Counter()
    for r,g,b in image.crop(box).convert('RGB').getdata():
        if r>130 and g<110 and b<115 and r>g*1.35 and r>b*1.35:counts['red']+=1
        elif r>150 and 70<=g<=180 and b<70 and r>g*1.15:counts['orange']+=1
        elif r>130 and b>140 and g<90:counts['purple']+=1
        elif b>130 and r<130 and 20<=g<=140 and b>r*1.2 and b>g*1.2:counts['blue']+=1
    ranked=counts.most_common()
    assert ranked and ranked[0][1]>100 and (len(ranked)==1 or ranked[0][1]>ranked[1][1]*2),counts
    return ranked[0][0],dict(counts)

def build():
    catalog=json.loads(CATALOG.read_text(encoding='utf-8'))
    raw_hash=sha(CATALOG)
    evidence=OUT/'evidence';evidence.mkdir(parents=True,exist_ok=True)
    rows=[];sources=[]
    for season,(number,token) in SOURCES.items():
        original=ATTACHMENTS/f'mirasim-att-{token}.png'
        copied=evidence/f'S{season:02d}.png'
        if copied.exists():assert sha(copied)==sha(original)
        else:shutil.copy2(original,copied)
        im=Image.open(copied);im.load()
        products=[(key,value) for key,value in catalog['products'].items() if value['source_group'].startswith(f'S{season}:')]
        products.sort(key=lambda p:int(p[0]))
        assert len(products)==(15 if season in (4,5,8) else 13)
        for index,(key,product) in enumerate(products):
            # Each supplied crop has uniform menu rows. Image 7 additionally
            # contains the original app; use only its right-hand S1 submenu.
            if season==1:box=(466,527+36*index,774,527+36*(index+1))
            else:box=(0,round(im.height*index/len(products)),im.width,round(im.height*(index+1)/len(products)))
            color,pixels=row_color(im,box)
            parts=product['name'].split(' - ')
            variant=parts[-1] if parts[-1] in ('极品','优品') else None
            if variant:parts.pop()
            weapon=parts[0];skin=' - '.join(parts[1:]) or None
            if variant=='极品':assert color=='red'
            if variant=='优品':assert color=='orange'
            rows.append(dict(product_id=key,season_id=f'S{season}',season_label=product['season_label'],
                weapon=weapon,skin_series=skin,variant_label=variant,menu_color=color,
                game_quality_name=None,display_name=product['display_name'],
                source_image_number=number,source_menu_row=index+1,source_image_sha256=sha(copied),
                color_sample_box=list(box),color_pixel_counts=pixels))
        sources.append(dict(season=f'S{season}',image_number=number,file=f'evidence/S{season:02d}.png',sha256=sha(copied)))
    assert len(rows)==149 and len({r['product_id'] for r in rows})==149
    data=dict(schema='skin-reference-v1',created_date='2026-10-07',source_catalog_sha256=raw_hash,
        text_source='existing 0925 dropdown dictionary; IDs preserved',color_source='user supplied 1003 menu screenshots',
        automatic_game_quality_mapping=False,production_rules_changed=False,condition_is_product_quality=False,
        game_quality_name_status='not established by menu colors alone',sources=sources,rows=rows)
    (OUT/'skin_reference.json').write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    text=['# 皮肤基础表','',
        '已有目录 149 项；补录用户截图中的红、橙、紫、蓝菜单颜色。极品/优品后缀单列，不与 S/A/B/C 成色混用。',
        '名称与商品 ID 来自现有目录，颜色来自这次截图。游戏正式品质名称与筛选按钮的对应尚未确认；这张参考表尚未接入运行校验。',
        '同武器同皮肤的极品、优品保留不同商品 ID。原菜单没有给出系列名的项保留“未标注”，不拿赛季名称代填。','',
        '字段关系：皮肤目录保存固定属性；任务另存启用、成色、最大磨损、价格上下限、限量，并通过商品 ID 引用目录。','',
        '| 商品 ID | 赛季 | 武器 | 皮肤系列 | 极品/优品标记 | 菜单颜色 | 游戏品质名 | 来源图/行 |',
        '|---|---|---|---|---|---|---|---|']
    for r in rows:
        text.append('| '+' | '.join([r['product_id'],r['season_id'],r['weapon'],r['skin_series'] or '未标注',
            r['variant_label'] or '未标注',LABELS[r['menu_color']],'待确认',f"图{r['source_image_number']}/第{r['source_menu_row']}行"])+' |')
    (OUT/'皮肤基础表.md').write_text('\n'.join(text)+'\n',encoding='utf-8')
    assert sha(CATALOG)==raw_hash
    verify()

def verify():
    data=json.loads((OUT/'skin_reference.json').read_text(encoding='utf-8'))
    rows=data['rows'];assert len(rows)==149 and len({r['product_id'] for r in rows})==149
    assert sha(CATALOG)==data['source_catalog_sha256']
    for source in data['sources']:assert sha(OUT/source['file'])==source['sha256']
    assert len((OUT/'皮肤基础表.md').read_text(encoding='utf-8').splitlines())>=149
    assert all(r['game_quality_name'] is None for r in rows)
    enabled_ids=['10602','10603','10604','11102','11103','11104','10504','10505','10506']
    by_id={r['product_id']:r for r in rows}
    assert all(by_id[key]['menu_color']=='purple' for key in enabled_ids)
    assert Counter(r['menu_color'] for r in rows)==dict(red=14,orange=14,purple=33,blue=88)
    print('SKIN_REFERENCE=PASS; rows=149; current_enabled_products=9; enabled_colors=purple; production_rules_changed=false')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--verify-only',action='store_true');args=p.parse_args()
    verify() if args.verify_only else build()
