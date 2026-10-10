"""Explicit read-only listing-wheel calibration, with no star or purchase action.

Navigate with the existing observer. Read top-window identities, expose the
next row with two single notches, read its two identities, return to the top,
then measure one actual -480 input against the same row. Images stay in memory.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys
import time

from collection_candidate import match_selected_card
from collection_labels import exact_label_target
from collection_scroll import observe_layout, same_card_geometry
from collection_live_session import ForegroundSession
from run_collection_observed import MemoryReviewBackend
from run_collection_trial import CollectionTrial

ROOT=Path(__file__).resolve().parents[2]

def write(path,value):
    Path(path).write_text(json.dumps(value,ensure_ascii=False,indent=2,allow_nan=False)+'\n','utf-8')

def frame_info(packet):
    layout=observe_layout(packet)
    return {k:copy.deepcopy(layout[k]) for k in ('frame_id','frame_sha256','viewport','listing_viewport','scrollbar','cards')}

def identity(candidate):
    return {key:candidate[key] for key in ('product','condition','price','wear')}

def select_read(trial,rule,index):
    current=trial.geometry()
    full=[c for c in current['cards'] if c['selectable']]
    card=copy.deepcopy(full[index])
    trial.select_card(rule,card)
    value=match_selected_card(trial.observed,rule)
    return dict(card=card,candidate=value,disposition='read_only_observed')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--row',type=int,default=7)
    parser.add_argument('--delta',type=int,choices=(-480,-600),default=-480)
    args=parser.parse_args()
    output=args.output.resolve()
    assert output.is_relative_to(ROOT/'artifacts')
    output.mkdir(exist_ok=False)
    snapshot_path=ROOT/'artifacts/collection_scroll_speed/input/task_snapshot.json'
    snapshot=json.loads(snapshot_path.read_text('utf-8'))
    rule=next(r for r in snapshot['rows'] if r['row_index']==args.row)
    journal=ROOT/'artifacts/m2_savedvalue_collection/journal'
    before_journal={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in journal.glob('*.json')}
    backend=MemoryReviewBackend(ROOT,local_title=True,local_price=True,fast_capture=True)
    session=ForegroundSession(output/'session.json',backend=backend,timeout_seconds=600,
                              max_steps=250,numeric_price=True,fast_settle=True)
    session.report['requested_runner_command']=[sys.executable,*sys.argv]
    events=[]
    trial=CollectionTrial(session,snapshot,snapshot_path,emit=events.append,
                          snapshot_sha256=hashlib.sha256(snapshot_path.read_bytes()).hexdigest())
    result=dict(schema='collection-scroll-probe-v1',status='running',phase='read_only_scroll_measurement',
                star_actions=0,purchase_actions=0,game_image_file_writes=0,row=rule,readings=[])
    start=time.perf_counter()
    try:
        with session:
            trial.startup();trial.open_product(rule);trial.condition(rule);trial.ensure_list_top(rule)
            initial=frame_info(trial.observed)
            n=len([c for c in initial['cards'] if c['selectable']])
            assert n==6,'MEASUREMENT_REQUIRES_TOP_THREE_ROWS'
            for i in range(n):result['readings'].append(select_read(trial,rule,i))
            result['initial']=frame_info(trial.observed)
            # This independent reference exposure is one two-notch navigation
            # input. It is not the -480 batch being calibrated, and avoids
            # processing the intermediate one-notch window as completed work.
            session.perform(dict(kind='scroll',delta=-240,point=[990,751],viewport=[2560,1440],
                expected_before='skin_listings',require_label={'region':'product_title','label':rule['product_name']}))
            trial.capture('skin_listings',collection_layout=True)
            result['reference_geometry']=frame_info(trial.observed)
            full=[c for c in trial.geometry()['cards'] if c['selectable']]
            # At two observed notches, the formerly clipped fourth row is now
            # the last complete row. Bind its observed edge displacement to
            # both prior top-frame boundaries, before trusting any identity.
            refs=[]
            for i in range(len(full)-2,len(full)):
                entry=select_read(trial,rule,i)
                refs.append(entry)
            result['reference_readings']=refs
            session.perform(dict(kind='scroll',delta=240,point=[990,751],viewport=[2560,1440],
                expected_before='skin_listings',require_label={'region':'product_title','label':rule['product_name']}))
            trial.capture('skin_listings',collection_layout=True);trial.ensure_list_top(rule)
            result['before']=frame_info(trial.observed)
            top=trial.geometry();assert top['scrollbar']['thumb_bounds'][1]-top['scrollbar']['track_bounds'][1]<=2
            before_full=[c for c in top['cards'] if c['selectable']]
            assert len(before_full)==6
            boundaries=sorted([c for c in top['cards'] if not c['selectable'] and c['edges']['top']
                               and not c['edges']['bottom']],key=lambda c:c['bounds'][0])
            assert len(boundaries)==2
            # This is a diagnostic input, NOT an invented calibrated lease.
            # The next operation is always a new capture; no collection loop
            # is called, and no coverage completion is claimed by this input.
            layout_before=copy.deepcopy(top)
            point=[top['listing_viewport'][0]+top['listing_viewport'][2]//2,
                   top['listing_viewport'][1]+top['listing_viewport'][3]//2]
            def guard():
                assert session.pending is None and session.pending_geometry is None
                assert backend.foreground() and list(backend.viewport())==top['viewport']
                session._fresh()
                assert observe_layout(session.previous)==layout_before
                assert exact_label_target(session.previous,'product_title',rule['product_name']) is not None
                assert session.previous['startup_page']['overlay']=='none'
                assert session.remaining_seconds()>15
            guard()
            action=dict(kind='diagnostic_calibration_scroll',delta=args.delta,point=point,
                measurement_only=True,collection_completion_claimed=False,
                source_frame_id=top['frame_id'],source_frame_sha256=top['frame_sha256'],status='prepared')
            write(output/'batch_input.json',action)
            started=time.perf_counter()
            sent=backend.scroll(point,args.delta,before_dispatch=guard)
            action.update(sent=sent,status='dispatched' if sent==1 else 'input_uncertain',
                elapsed_ms=(time.perf_counter()-started)*1000,motion=copy.deepcopy(backend.last_motion))
            write(output/'batch_input.json',action)
            session.previous={};session.selected_lease=None
            assert sent==1,'SCROLL_PROBE_INPUT_UNCERTAIN'
            time.sleep(.6)
            trial.capture('skin_listings',collection_layout=True)
            result['after']=frame_info(trial.observed)
            result['batch_input']=action
            matches=[]
            for i in range(2):
                entry=select_read(trial,rule,i)
                ref=refs[i]
                assert identity(ref['candidate'])==identity(entry['candidate']),'SCROLL_PROBE_IDENTITY_CHANGED'
                assert all(identity(x['candidate'])!=identity(entry['candidate']) for x in result['readings']), 'SCROLL_PROBE_OLD_ITEM_REAPPEARED'
                matches.append(dict(column_index=i,before_boundary_top_y=boundaries[i]['bounds'][1],
                    after_card_top_y=entry['candidate']['card_bounds'][1],
                    before_candidate=identity(ref['candidate']),after_candidate=identity(entry['candidate']),
                    reference_observation_frame_id=ref['candidate']['source_frame_id'],
                    reference_observation_frame_sha256=ref['candidate']['source_frame_sha256'],
                    after_observation_frame_id=entry['candidate']['source_frame_id'],
                    after_observation_frame_sha256=entry['candidate']['source_frame_sha256']))
                result['readings'].append(entry)
            result['identity_matches']=matches
            result['status']='measured'
    except Exception as error:
        result.update(status='blocked',error=str(error))
    result.update(ide_restored=session.report.get('ide_restored'),elapsed_ms=(time.perf_counter()-start)*1000,
                  pending_geometry=session.pending_geometry,pending_collection=session.pending)
    after_journal={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in journal.glob('*.json')}
    result['journal_unchanged']=before_journal==after_journal
    assert result['journal_unchanged']
    write(output/'events.json',events);write(output/'result.json',result)
    print(json.dumps({k:result.get(k) for k in ('status','error','ide_restored','elapsed_ms','journal_unchanged','identity_matches')},ensure_ascii=False))
    print(json.dumps(dict(packet='scroll-probe-memory-image-v1',preview_jpeg_base64=session.preview_jpeg(),
                         image_file_writes=0),ensure_ascii=False))
    return 0 if result['status']=='measured' and result['ide_restored'] else 1

if __name__=='__main__':raise SystemExit(main())
