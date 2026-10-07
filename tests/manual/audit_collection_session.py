"""Audit persisted collection evidence without capturing or sending any input."""
from collections import Counter
import hashlib
import json
from pathlib import Path
from collection_journal import candidate_key

ROOT=Path(__file__).resolve().parents[2]
TX=ROOT/'artifacts/m2_savedvalue_collection'

def main():
    source=TX/'input/source.savedValue'
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    snapshot=json.loads((TX/'input/task_snapshot.json').read_text(encoding='utf-8'))
    assert digest==snapshot['source_sha256']=='cff452bcd4b77a17fb3cc9a23f55e0131304098f7ac0057205f619bbd4406253'
    assert len(snapshot['rows'])==50 and sum(r['enabled'] for r in snapshot['rows'])==21
    journal=[]
    for path in sorted((TX/'journal').glob('*.json')):
        item=json.loads(path.read_text(encoding='utf-8'));candidate=item['candidate']
        assert item['key']==candidate_key(candidate)==path.stem
        assert candidate['source_sha256']==digest and candidate['eligible']
        assert item['status'] in ('confirmed','confirmed_reconciliation'),'pending collection requires reconciliation'
        if item['status']=='confirmed':
            assert item['sent']==2
            receipt=item['receipt']
            assert candidate['source_frame_sha256']!=receipt['source_frame_sha256']
            assert all(candidate[k]==receipt[k] for k in ('product','condition','price','wear','row_index'))
            record=Path(item['record']);assert record.is_file()
            batch=json.loads(record.read_text(encoding='utf-8'))
            assert any(s.get('result',{}).get('collection_receipt_passed') is True
                and s['result']['collection_observation']['frame_sha256']==receipt['source_frame_sha256'] for s in batch['steps'])
            if item.get('receipt_kind')=='same_item_white_to_gold':
                assert candidate['favorite_before']['favorite_warm_fraction']==0
                assert candidate['favorite_before']['favorite_bright_fraction']>=.04
        else:
            reconciliation=item['reconciliation']
            assert reconciliation.get('input_actions',0)==0
            assert reconciliation['gold_indicator']['favorite_warm_fraction']>=.05
        journal.append(item)
    records=sorted(set(TX.glob('live_*.json'))|set(TX.glob('live_series_*/live_*.json')))
    evidence=[]
    for path in records:
        data=json.loads(path.read_text(encoding='utf-8'))
        assert data['image_file_writes']==0 and data['ide_restored'] is True
        assert data['enter_calls']==1 and data['leave_calls']==1
        evidence.append(dict(path=str(path.relative_to(ROOT)),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            passed=data['passed'],exit_status=data['exit_status'],error=data['error']))
    by_row=Counter(j['candidate']['row_index'] for j in journal)
    result=dict(source_sha256=digest,confirmed_collections=len(journal),statuses=dict(Counter(j['status'] for j in journal)),
        collections_by_source_row=dict(sorted(by_row.items())),pending_attempts=0,
        task_file_fully_completed=False,records=evidence,no_game_image_files_written=True,
        game_mutations_rolled_back=False,release_rollback_is_separate_from_game_state=True)
    (TX/'session_audit.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print('COLLECTION_LIVE_AUDIT=PASS; confirmed='+str(len(journal))+'; pending=0; historical_failures_preserved=true')

if __name__=='__main__':main()
