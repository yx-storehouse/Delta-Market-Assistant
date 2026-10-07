"""Advance resolved rows with bounded focus batches; stop on unknowns, never retry mutations."""
import argparse
from decimal import Decimal
import json
from pathlib import Path
import subprocess
import sys
import time
from build_collection_plan import plan, capture, click, SNAPSHOT, ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rows',type=int,nargs='+',required=True)
    parser.add_argument('--continue-card',type=int)
    parser.add_argument('--run',required=True)
    args=parser.parse_args()
    tx=ROOT/'artifacts/m2_savedvalue_collection'
    directory=(tx/args.run).resolve()
    assert directory.is_relative_to(tx) and not directory.exists()
    directory.mkdir()
    rows=json.loads((ROOT/SNAPSHOT).read_text(encoding='utf-8'))['rows']
    rules={row['row_index']:row for row in rows}
    assert all(rules[row]['enabled'] and rules[row]['dictionary_resolved'] for row in args.rows)
    journal=[json.loads(p.read_text(encoding='utf-8')) for p in (tx/'journal').glob('*.json')]
    assert all(j['status'] in ('confirmed','confirmed_reconciliation') for j in journal),'COLLECTION_PENDING_RECONCILIATION'
    summary=dict(passed=False,rows=[],batches=[],error=None,image_file_writes=0)
    last_preview=None; counter=0;deadline=time.monotonic()+900

    def batch(data):
        nonlocal counter,last_preview
        assert time.monotonic()<deadline,'SERIES_DEADLINE'
        counter+=1;plan_path=directory/f'plan_{counter}.json';record=directory/f'live_{counter}.json'
        for step in data['steps']:
            if step['kind']=='collect_selected':step['end_segment_on_price_above']=True
        plan_path.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
        command=[sys.executable,'-X','utf8',str(ROOT/'tests/manual/run_foreground_batch.py'),
            '--plan',str(plan_path),'--record',str(record)]
        process=subprocess.run(command,capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW,timeout=50)
        result=json.loads(process.stdout)
        if result.get('preview_jpeg'):last_preview=result['preview_jpeg']
        report=result['metadata']
        summary['batches'].append(dict(record=str(record),exit_status=process.returncode,passed=report['passed'],error=report['error']))
        if process.returncode or not report['passed']:raise RuntimeError(report['error'])
        return report

    def above(report,rule):
        candidates=[s.get('candidate',s.get('collection_attempt')) for s in report['steps']]
        candidates=[c for c in candidates if c is not None]
        assert candidates,'COLLECTION_CANDIDATE_NOT_OBSERVED'
        return next((c for c in candidates if Decimal(c['price'])>Decimal(rule['price_max'])),None)

    previous_rule=None
    try:
        for index,row in enumerate(args.rows):
            rule=rules[row]
            boundary=None
            if previous_rule is not None and rule['product_name']!=previous_rule['product_name']:
                if rule['season_label']!=previous_rule['season_label']:raise RuntimeError('COLLECTION_NEXT_SEASON_REQUIRES_NAVIGATION')
                batch(dict(name='Return from completed product to catalogue',timeout_seconds=15,
                    steps=[capture(expected_page='skin_listings'),click([168,1403]),capture(expected_page='skin_home',ui_regions=True,preview=True)]))
                report=batch(plan('open',row,[]));boundary=above(report,rule);next_card=1
            elif index==0 and args.continue_card is not None:
                next_card=args.continue_card
            else:
                report=batch(plan('condition',row,[]));boundary=above(report,rule);next_card=1
            while boundary is None and next_card<=5:
                indices=list(range(next_card,min(6,next_card+2)))
                report=batch(plan('cards',row,indices));boundary=above(report,rule);next_card=indices[-1]+1
            if boundary is None:raise RuntimeError('COLLECTION_UNSCANNED_SCROLL_REGION')
            summary['rows'].append(dict(row_index=row,status='original_price_stop_boundary',boundary=boundary,
                unobserved_tail_exhaustive=False))
            previous_rule=rule
        summary['passed']=True
    except Exception as error:
        summary['error']=str(error)
    finally:
        (directory/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(metadata=summary,preview_jpeg=last_preview),ensure_ascii=False))
    return 0 if summary['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
