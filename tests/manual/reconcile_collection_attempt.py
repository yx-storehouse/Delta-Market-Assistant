"""Reconcile one dispatched attempt with a later same-item gold-star observation.

Does not send input, capture pixels, or replace the original failed batch.
"""
import argparse
import json
from pathlib import Path
from collection_candidate import match_selected_first_card
from collection_journal import CollectionJournal, validate_record_identity

ROOT=Path(__file__).resolve().parents[2]
TX=ROOT/'artifacts/m2_savedvalue_collection'


def pending_for_candidate(records, candidate):
    """Resolve an actual unfinished attempt, never a completed identity record."""
    matches=[]
    for record in records:
        validate_record_identity(record)
        if record.get('status') not in ('prepared','dispatched','input_uncertain'):
            continue
        if all(record['candidate'].get(k)==candidate.get(k)
               for k in ('product','condition','price','wear','row_index')):
            matches.append(record)
    if len(matches)!=1:
        raise ValueError('COLLECTION_RECONCILIATION_PENDING_NOT_UNIQUE')
    original=matches[0]['candidate']
    if (original.get('source_frame_sha256')==candidate.get('source_frame_sha256')
            or original.get('source_frame_id') is not None
               and original.get('source_frame_id')==candidate.get('source_frame_id')):
        raise ValueError('COLLECTION_RECONCILIATION_FRESH_FRAME_REQUIRED')
    return matches[0]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--observation',required=True);p.add_argument('--row',type=int,required=True);p.add_argument('--output',required=True)
    args=p.parse_args()
    source=(TX/args.observation).resolve();out=(TX/args.output).resolve()
    assert source.is_relative_to(TX) and out.is_relative_to(TX) and not out.exists()
    report=json.loads(source.read_text(encoding='utf-8'));assert report['passed'] and report['manual_clicks']==0 and report.get('key_presses',0)==0
    packet=report['steps'][-1]['result']
    rule=next(r for r in json.loads((TX/'input/task_snapshot.json').read_text(encoding='utf-8'))['rows'] if r['row_index']==args.row)
    candidate=match_selected_first_card(packet,rule)
    journal=CollectionJournal(TX/'journal')
    records=[]
    for path in journal.directory.glob('*.json'):
        record=json.loads(path.read_text(encoding='utf-8'))
        assert validate_record_identity(record)==path.stem
        records.append(record)
    pending=pending_for_candidate(records,candidate);key=pending['key']
    star=packet['collection_selected_card'];assert star['favorite_warm_fraction']>=.05 and star['favorite_bright_fraction']<=.02
    record=dict(status='confirmed_reconciliation',candidate=candidate,observation=str(source),gold_indicator=star,
        original_failed_batch_unchanged=True,input_actions=0,journal_key=key)
    with out.open('x',encoding='utf-8') as file:json.dump(record,file,ensure_ascii=False,indent=2)
    journal.update(key,'confirmed_reconciliation',reconciliation=record)
    print('RECONCILIATION=PASS; input_actions=0; original_failed_batch_unchanged=true')

if __name__=='__main__':main()
