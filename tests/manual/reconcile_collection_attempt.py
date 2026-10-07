"""Reconcile one dispatched attempt with a later same-item gold-star observation.

Does not send input, capture pixels, or replace the original failed batch.
"""
import argparse
import json
from pathlib import Path
from collection_candidate import match_selected_first_card
from collection_journal import CollectionJournal, candidate_key

ROOT=Path(__file__).resolve().parents[2]
TX=ROOT/'artifacts/m2_savedvalue_collection'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--observation',required=True);p.add_argument('--row',type=int,required=True);p.add_argument('--output',required=True)
    args=p.parse_args()
    source=(TX/args.observation).resolve();out=(TX/args.output).resolve()
    assert source.is_relative_to(TX) and out.is_relative_to(TX) and not out.exists()
    report=json.loads(source.read_text(encoding='utf-8'));assert report['passed'] and report['manual_clicks']==0
    packet=report['steps'][-1]['result']
    rule=next(r for r in json.loads((TX/'input/task_snapshot.json').read_text(encoding='utf-8'))['rows'] if r['row_index']==args.row)
    candidate=match_selected_first_card(packet,rule);key=candidate_key(candidate)
    journal=CollectionJournal(TX/'journal')
    pending=json.loads((journal.directory/(key+'.json')).read_text(encoding='utf-8'))
    assert pending['status'] in ('prepared','dispatched','input_uncertain')
    assert all(pending['candidate'][k]==candidate[k] for k in ('product','condition','price','wear','row_index'))
    star=packet['collection_selected_card'];assert star['favorite_warm_fraction']>=.05 and star['favorite_bright_fraction']<=.02
    record=dict(status='confirmed_reconciliation',candidate=candidate,observation=str(source),gold_indicator=star,
        original_failed_batch_unchanged=True,input_actions=0,journal_key=key)
    with out.open('x',encoding='utf-8') as file:json.dump(record,file,ensure_ascii=False,indent=2)
    journal.update(key,'confirmed_reconciliation',reconciliation=record)
    print('RECONCILIATION=PASS; input_actions=0; original_failed_batch_unchanged=true')

if __name__=='__main__':main()
