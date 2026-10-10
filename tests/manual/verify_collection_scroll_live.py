"""Exercise the real calibrated scroll plans without toggling any star.

All complete cards are independently selected/read before each wheel. We do
not count the probe as business collection or bypass a historical star conflict.
"""
import argparse,copy,hashlib,json,sys,time
from decimal import Decimal
from pathlib import Path
from collection_live_session import ForegroundSession
from collection_scroll import build_scroll_coverage,select_window_scroll_calibration,scroll_plan
from run_collection_observed import MemoryReviewBackend
from run_collection_trial import CollectionTrial,load_scroll_profile
from probe_collection_scroll import select_read,frame_info,identity,write

ROOT=Path(__file__).resolve().parents[2]
def choose_window_profile(packet,rule,coverage,profiles,*,window_index,first_delta=None):
    """Constrain only the first read-only test input to an existing safe profile.

    An independently measured first distance seeds a different row phase;
    subsequent windows always use the ordinary adaptive selection. This helper
    performs no input and never relaxes coverage, shape or distance bounds.
    """
    if type(window_index) is not int or window_index<0:
        raise ValueError('COLLECTION_SCROLL_TEST_WINDOW_INDEX')
    if first_delta is not None and (type(first_delta) is not int or first_delta not in (-480,-600)):
        raise ValueError('COLLECTION_SCROLL_TEST_FIRST_DELTA')
    candidates=profiles
    if window_index==0 and first_delta is not None:
        candidates=[profile for profile in profiles if profile.get('delta')==first_delta]
        if not candidates:
            raise ValueError('COLLECTION_SCROLL_TEST_FIRST_DELTA_UNMEASURED')
    return select_window_scroll_calibration(packet,rule,coverage,candidates)

def price_independent_observation_key(candidate):
    """A local composite observation, not a server listing identifier."""
    return candidate['product'],candidate['condition'],str(Decimal(candidate['wear']).normalize())

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--windows',type=int,default=3)
    p.add_argument('--first-delta',type=int,choices=(-480,-600),default=None,
                   help='Read-only first scroll: choose an existing measured safe profile; later scrolls stay adaptive.')
    args=p.parse_args();out=args.output.resolve()
    assert out.is_relative_to(ROOT/'artifacts') and 2<=args.windows<=8
    out.mkdir(exist_ok=False)
    snap=ROOT/'artifacts/collection_scroll_speed/input/task_snapshot.json'
    snapshot=json.loads(snap.read_text('utf-8'));rule=next(r for r in snapshot['rows'] if r['row_index']==7)
    bank_path=ROOT/'artifacts/collection_scroll_speed/calibration_bank.json'
    bank=json.loads(bank_path.read_text('utf-8'))
    profiles=[]
    for entry in bank['profiles']:
        path=ROOT/entry['path'];assert hashlib.sha256(path.read_bytes()).hexdigest()==entry['sha256']
        profiles.append(load_scroll_profile(path)[0])
    journal=ROOT/'artifacts/m2_savedvalue_collection/journal'
    hashes=lambda:{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in journal.glob('*.json')}
    before_journal=hashes()
    session=ForegroundSession(out/'session.json',backend=MemoryReviewBackend(ROOT,local_title=True,
        local_price=True,fast_capture=True),timeout_seconds=600,max_steps=400,numeric_price=True,fast_settle=True)
    session.report['requested_runner_command']=[sys.executable,*sys.argv]
    events=[]
    trial=CollectionTrial(session,snapshot,snap,emit=events.append,scroll_profile_bank=profiles)
    result=dict(schema='collection-scroll-live-validation-v1',status='running',
        mode='read_only_module_test',windows=[],star_actions=0,purchase_actions=0,image_file_writes=0,
        first_delta=args.first_delta,first_delta_requires_measured_safe_profile=True,
        later_windows_use_normal_adaptive_selection=True,
        observed_tuple_fields=['product','condition','price','wear'],
        price_independent_observation_fields=['product','condition','decimal_normalized_wear'],
        server_listing_id_observed=False,permanent_listing_identity_claimed=False)
    started=time.perf_counter();seen=set();seen_price_independent=set()
    try:
        with session:
            trial.startup();trial.open_product(rule);trial.condition(rule);trial.ensure_list_top(rule)
            for index in range(args.windows):
                n=len([c for c in trial.geometry()['cards'] if c['selectable']])
                readings=[]
                for i in range(n):readings.append(select_read(trial,rule,i))
                current={tuple(identity(v['candidate']).items()) for v in readings}
                current_price_independent={price_independent_observation_key(v['candidate']) for v in readings}
                duplicate=current & seen
                duplicate_price_independent=current_price_independent & seen_price_independent
                window=dict(index=index,frame=frame_info(trial.observed),readings=readings,
                    duplicate_prior_identity_count=len(duplicate),complete_cards=n,
                    duplicate_count_semantics='exact_observed_product_condition_price_wear_tuple_not_server_listing_id',
                    duplicate_prior_product_condition_wear_count=len(duplicate_price_independent),
                    duplicate_price_independent_observations=sorted(duplicate_price_independent))
                result['windows'].append(window);write(out/'result.json',result)
                assert not duplicate,'SCROLL_WINDOW_REPEATED_PRIOR_IDENTITY'
                seen.update(current)
                seen_price_independent.update(current_price_independent)
                if index+1==args.windows:break
                coverage=build_scroll_coverage(trial.observed,rule,readings,mode='read_only_probe')
                chosen=choose_window_profile(trial.observed,rule,coverage,profiles,
                    window_index=index,first_delta=args.first_delta)
                records=[json.loads(f.read_text('utf-8')) for f in journal.glob('*.json')]
                plan=scroll_plan(trial.observed,rule,records,delta=chosen['delta'],calibration=chosen,coverage=coverage)
                window['scroll_delta']=chosen['delta'];window['coverage']=coverage
                window['calibration_source']=copy.deepcopy(chosen['source'])
                window['profile_selection']=('explicit_first_measured_safe_profile'
                    if index==0 and args.first_delta is not None else 'normal_adaptive_selection')
                for step in plan['steps']:session.perform(step)
                rebound=trial.observed.get('collection_geometry_rebind',{})
                assert rebound.get('passed') and rebound.get('kind')=='scroll'
                window['readback']=rebound
                write(out/'result.json',result)
            result['status']='passed'
    except Exception as error:
        result.update(status='blocked',error=str(error),candidate_rejections=getattr(error,'candidate_rejections',None))
    result.update(ide_restored=session.report.get('ide_restored'),elapsed_ms=(time.perf_counter()-started)*1000,
        journal_unchanged=hashes()==before_journal,distinct_observed_identities=len(seen),
        distinct_observed_product_condition_wear=len(seen_price_independent),
        pending_collection=session.pending,pending_geometry=session.pending_geometry,
        task_file_fully_completed=False,collection_completion_claimed=False)
    assert result['journal_unchanged']
    write(out/'result.json',result);write(out/'events.json',events)
    print(json.dumps({k:result.get(k) for k in ('status','error','ide_restored','distinct_observed_identities','elapsed_ms','journal_unchanged','candidate_rejections')},ensure_ascii=False))
    print(json.dumps(dict(packet='scroll-window-memory-image-v1',preview_jpeg_base64=session.preview_jpeg(),image_file_writes=0)))
    return 0 if result['status']=='passed' and result['ide_restored'] else 1
if __name__=='__main__':raise SystemExit(main())
