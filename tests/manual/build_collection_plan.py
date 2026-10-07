"""Create a finite calibrated collection batch from the resolved task snapshot."""
import argparse
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
SNAPSHOT='artifacts/m2_savedvalue_collection/input/task_snapshot.json'

def capture(**kwargs):
    return dict(kind='capture',collection_observation=True,**kwargs)

def click(point,**kwargs):
    return dict(kind='click',expected_before='skin_listings',point=point,viewport=[2560,1440],**kwargs)

def collect(row,index):
    return [dict(kind='collect_selected',expected_before='skin_listings',snapshot=SNAPSHOT,row_index=row,
        viewport=[2560,1440],skip_ineligible=True,skip_favorited=True),
        capture(expected_page='skin_listings',card_index=index,expect_collection_added=True,receipt_if_pending=True,preview=True)]

def plan(mode,row,indices):
    rows=json.loads((ROOT/SNAPSHOT).read_text(encoding='utf-8'))['rows']
    rule=next(r for r in rows if r['row_index']==row)
    assert rule['enabled'] and rule['dictionary_resolved']
    title=dict(region='product_title',label=rule['product_name'])
    steps=[capture(expected_page='skin_listings')]
    if mode in ('condition','open'):
        if mode=='open':
            steps=[capture(expected_page='skin_home',ui_regions=True),
                dict(kind='click_label',expected_before='skin_home',region='catalog_names',label=rule['product_name'],viewport=[2560,1440]),
                capture(expected_page='skin_home',ui_regions=True,preview=True),
                dict(kind='click',expected_before='skin_home',point=[2240,1180],require_label=title,viewport=[2560,1440]),
                capture(expected_page='skin_listings')]
        condition=rule['condition_label'][-1]
        points={'S':[2187,464],'A':[1861,539],'B':[2187,539],'C':[1861,613]}
        wanted={k:'checked' if k==condition else 'unchecked' for k in ['all','S','A','B','C']}
        steps += [click([300,263],require_label=title),capture(),
            click(points[condition],expected_overlay='listing_filter',skip_if_condition_state=[condition,'checked']),
            capture(expected_condition_filter=wanted),click([2340,1355],expected_overlay='listing_filter'),
            capture(expected_page='skin_listings',card_index=0,preview=True),*collect(row,0)]
    elif mode=='cards':
        assert 1<=len(indices)<=3 and all(0<=i<=5 for i in indices)
        for index in indices:
            steps += [click([500+(index%2)*877,420+(index//2)*275],require_label=title),
                capture(expected_page='skin_listings',card_index=index,preview=True),*collect(row,index)]
    return dict(name=f'Collection {mode}: source row {row}',timeout_seconds=30,steps=steps)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=['condition','cards','open']);p.add_argument('--row',type=int,required=True)
    p.add_argument('--indices',type=int,nargs='*',default=[]);p.add_argument('--output',required=True)
    args=p.parse_args();out=(ROOT/args.output).resolve()
    assert out.is_relative_to(ROOT/'tests/manual/plans') and not out.exists()
    out.write_text(json.dumps(plan(args.mode,args.row,args.indices),ensure_ascii=False,indent=2),encoding='utf-8')
