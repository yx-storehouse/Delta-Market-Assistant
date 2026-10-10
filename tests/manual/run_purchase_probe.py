"""Explicit bounded watchlist observation batch; no purchase input is defined.

Importing this module is inert. Run only after coordinating game foreground.
It navigates to My Favorites, reads the current selected offer, then restores
IDE once. It does not select other offers, toggle stars or click any modal.
"""
import argparse
import json
import math
from pathlib import Path
import sys
import time

from collection_paths import project_root
from collection_run_config import snapshot_from_files
from purchase_observation import match_current_watchlist
from purchase_plan import Scope,ReadonlyPurchasePlanner


# A read without an expected page that lands on a frame mid-transition is
# read again (live timed04: the first read right after the game came to the
# front was 'unknown' with only two anchors).
UNKNOWN_REREADS=4
UNKNOWN_REREAD_PAUSE_S=.25
# The listing filter's confirm control (collection startup closes a leftover
# panel the same way): live buy_cycle02 2026-10-10, a collection stopped with
# the panel open and every purchase attempt after it refused the page.
LISTING_FILTER_CONFIRM=[2340,1355]


def navigate(session,pause=None):
    pause=pause or time.sleep
    seen={}
    def capture(expected=None,filter_ok=False):
        for attempt in range(1+(0 if expected else UNKNOWN_REREADS)):
            if attempt:pause(UNKNOWN_REREAD_PAUSE_S)
            step=dict(kind='capture',collection_observation=True,ui_regions=True)
            if expected:step['expected_page']=expected
            session.perform(step)
            page=session.previous.get('startup_page',{})
            seen['overlay']=page.get('overlay')
            leftover_filter=filter_ok and page.get('overlay')=='listing_filter' and page.get('page')=='skin_listings'
            if page.get('overlay')!='none' and not leftover_filter:raise ValueError('PURCHASE_PROBE_OVERLAY_REQUIRES_REVIEW')
            if page['page']!='unknown':break
        return page['page']
    page=capture(filter_ok=True)
    if page=='skin_listings' and seen['overlay']=='listing_filter':
        session.perform(dict(kind='click',point=LISTING_FILTER_CONFIRM,viewport=[2560,1440],
                             expected_before='skin_listings',expected_overlay='listing_filter'))
        page=capture('skin_listings')
    # The 典藏 filter dialog a stopped collection left open (review wf_993b38d0-6b8):
    # Esc without 确认, once more if the first only closed the season dropdown
    # (as CollectionTrial._close_filter); every collection rule sets its filter again.
    for press in range(2):
        if page!='catalog_filter':break
        session.perform(dict(kind='key',key='escape',expected_before='catalog_filter'))
        page=capture()
    if page=='lobby':
        session.perform(dict(kind='click',point=[678,1402],viewport=[2560,1440],expected_before='lobby'))
        page=capture('mandel')
    if page=='mandel':
        session.perform(dict(kind='click',point=[430,103],viewport=[2560,1440],expected_before='mandel'))
        page=capture('skin_home')
    if page=='skin_listings':
        session.perform(dict(kind='key',key='escape',expected_before='skin_listings'))
        page=capture('skin_home')
    if page=='skin_home':
        session.perform(dict(kind='click',point=[2310,274],viewport=[2560,1440],expected_before='skin_home'))
        page=capture()
    if page not in ('watchlist_listings','empty_watchlist'):
        raise ValueError('PURCHASE_PROBE_PAGE:'+str(page))
    return page


def sample_step(page):
    """One read of the watchlist page. ui_regions is required as in the
    navigation reads: the empty watchlist is classified from its on-page
    message (live 2026-10-09 live02: without it the page read as unknown)."""
    listed=page=='watchlist_listings'
    return dict(kind='capture',collection_observation=True,ui_regions=True,collection_layout=listed,
                collection_numeric_price=listed,purchase_observation=True,expected_page=page)


def allowed_step(step):
    if step.get('kind')=='capture':
        return not any(step.get(k) for k in ('expect_collection_added','receipt_if_pending','card_id','visible_index'))
    if step.get('kind')=='key':
        return step.get('key')=='escape' and step.get('expected_before') in ('skin_listings','catalog_filter')
    if step.get('expected_overlay','none')!='none':
        return (step.get('kind')=='click' and step.get('expected_overlay')=='listing_filter'
                and step.get('expected_before')=='skin_listings' and step.get('point')==LISTING_FILTER_CONFIRM)
    return step.get('kind')=='click' and (step.get('expected_before'),step.get('point')) in (
        ('lobby',[678,1402]),('mandel',[430,103]),('skin_home',[2310,274]))


RETURN_POLICIES=dict(entry='entry_foreground',ide='ide')


def return_policy(name):
    """Where the lease ends: 'entry' returns to the window that was in front
    when the probe started (the IDE or chat it was run from; nothing when the
    game itself was in front), 'ide' to the single Mirasim window."""
    if name not in RETURN_POLICIES:raise ValueError('PURCHASE_PROBE_RETURN_POLICY')
    return RETURN_POLICIES[name]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--config',type=Path,default=Path.home()/'AppData/Local/RelinkStudio/RelinkStudio/config.json')
    p.add_argument('--samples',type=int,choices=range(1,6),default=3)
    p.add_argument('--return-to',choices=sorted(RETURN_POLICIES),default='entry')
    args=p.parse_args();root=project_root(__file__)
    output=args.output.resolve()
    if not output.is_relative_to(root/'artifacts'):raise ValueError('PURCHASE_PROBE_OUTPUT_DIRECTORY')
    snapshot=snapshot_from_files(args.config,root/'dist/RelinkStudio/catalog/skins.json')
    output.mkdir(parents=True,exist_ok=False)
    # Imports instantiate no live backend until this explicit entry point.
    from collection_live_session import ForegroundSession
    from run_collection_observed import MemoryReviewBackend
    class ProbeSession(ForegroundSession):
        def perform(self,step,remaining=None):
            if not allowed_step(step):raise ValueError('PURCHASE_PROBE_STEP_NOT_ALLOWED')
            return super().perform(step,remaining)
    backend=MemoryReviewBackend(root,local_title=True,local_price=True,fast_capture=True,
        local_hotpath=False,ready_stream=False,fast_actions=False,parallel_local_price=True,
        return_policy=return_policy(args.return_to))
    session=ProbeSession(output/'session.json',backend=backend,timeout_seconds=60,max_steps=24,
        numeric_price=True,fast_settle=False)
    observations=[];result=dict(mode='watchlist_observation_only',purchase_actions=0,collection_actions=0)
    scope=Scope(output.name,'native_frame_monotonic',snapshot['config_sha256'],0,0)
    planner=ReadonlyPurchasePlanner(scope)
    try:
        with session:
            page=navigate(session)
            for index in range(args.samples):
                session.perform(sample_step(page))
                observed=match_current_watchlist(session.previous,snapshot)
                # Convert an elapsed age bound into the native frame's clock
                # domain; never subtract Python/native absolute timestamps.
                f=observed['screen']['source_frame']
                age=session.previous_age_upper_ms
                if not isinstance(age,(float,int)) or not math.isfinite(age) or age<0:
                    raise ValueError('PURCHASE_PROBE_FRAME_AGE')
                plan=planner.consume(observed,f['source_mono_ms']+math.ceil(age),scope)
                observations.append(dict(observation=observed,plan=plan))
                if page=='empty_watchlist':break
        result['status']='passed'
    except Exception as error:
        result.update(status='blocked',error=str(error))
    result.update(ide_restored=session.report.get('ide_restored'),cursor_restore_error=session.report.get('cursor_restore_error'),
        observations=observations,config_sha256=snapshot['config_sha256'],image_file_writes=session.report.get('image_file_writes'),
        pending_collection=session.pending,enter_calls=session.report['enter_calls'],leave_calls=session.report['leave_calls'])
    path=output/'result.json';path.write_text(json.dumps(result,ensure_ascii=False,indent=2,allow_nan=False)+'\n','utf8')
    assert json.loads(path.read_text('utf8'))==result
    print(json.dumps({k:v for k,v in result.items() if k!='observations'},ensure_ascii=False))
    return 0 if result['status']=='passed' and result['ide_restored'] else 1


if __name__=='__main__':raise SystemExit(main())
