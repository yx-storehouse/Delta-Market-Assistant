"""Synthetic purchase rehearsal plus old recorded field evidence; no OS input."""
import copy
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

from collection_candidate import match_selected_first_card,match_watchlist_selected
from purchase_observation import countdown_seconds,read_screen,match_current_watchlist,text_lines
from purchase_plan import Scope,ReadonlyPurchasePlanner,estimate_deadline,refine_deadline,receipt_projection,association
from test_collection_dynamic import dynamic_packet,rule


def packet(digest='a',source=10000,timer='10秒后解锁购买',button='公示中'):
    p=dynamic_packet(digest,x=120,top=303,gold=True)
    p['page_match_passed']=True
    p['startup_page'].update(page='watchlist_listings',overlay='none')
    p['collection_observation']['page']='watchlist_listings'
    p['frames'][-1].update(source_mono_ms=source,source_uncertainty_ms=1)
    projection=p['collection_observation'];fid=projection['frame_id'];sha=projection['frame_sha256']
    specs={'watch_header':[110,64,1760,134],'listing_text':[110,303,1760,930],
        'selected_detail':[1900,232,550,890],'purchase_footer':[1900,1130,550,175],'dialog_text':[360,210,1500,1030]}
    regions=[]
    for kind,bounds in specs.items():
        words=[]
        if kind=='purchase_footer':
            if timer:words.append(dict(text=timer,x=2000,y=1170,width=320,height=24))
            if button:words.append(dict(text=button,x=2100,y=1230,width=96,height=28))
        regions.append(dict(kind=kind,bounds=bounds,words=words,ok=True,truncated=False,same_frame=True,frame_id=fid,frame_sha256=sha))
    p['purchase_observation']=dict(schema='purchase-observation-v1',valid=True,same_frame=True,
        frame_id=fid,frame_sha256=sha,viewport=[2560,1440],page='watchlist_listings',overlay='none',
        actions_enabled=False,purchase_authorized=False,geometry_role='text_observation_only_not_click_targets',regions=regions)
    return p


def snapshot():return dict(schema='collection-run-snapshot-v1',ready=True,config_sha256='e'*64,rows=[rule()])
def scope():return Scope('session','monotonic','e'*64,0,0)


class CountdownTests(unittest.TestCase):
    def test_literal_forms(self):
        for raw,seconds in [('0秒后解锁购买',0),('10秒后解锁购买',10),('2分3秒后解锁购买',123),
                ('1小时2分3秒后解锁购买',3723),('１分２秒后解锁购买',62)]:
            with self.subTest(raw=raw):self.assertEqual(countdown_seconds(raw),seconds)

    def test_ocr_repairs_and_invalid_ranges_are_not_inferred(self):
        for raw in ('日2分3秒后解锁购买','2分O秒后解锁购买','2分3秒','-1秒后解锁购买',
                '1分60秒后解锁购买','1小时60分0秒后解锁购买','99小时0秒后解锁购买','余10秒后解锁购买','',None):
            with self.subTest(raw=raw),self.assertRaises(ValueError):countdown_seconds(raw)

    def test_deadline_is_an_interval_never_send_time(self):
        w=estimate_deadline(10,10000,2,'f','item',scope())
        self.assertEqual((w.earliest_ms,w.latest_ms),(18998,21002))
        n=estimate_deadline(9,11000,2,'g','item',scope())
        self.assertEqual(refine_deadline(w,n).earliest_ms,18998)

    def test_deadline_discontinuity_duplicate_and_scope_change_rejected(self):
        w=estimate_deadline(10,10000,1,'f','item',scope())
        for n in (w,estimate_deadline(40,11000,1,'g','item',scope()),
                estimate_deadline(9,11000,1,'g','other',scope()),
                estimate_deadline(9,11000,1,'g','item',Scope('new','monotonic','e'*64,0,0))):
            with self.assertRaises(ValueError):refine_deadline(w,n)

    def test_bools_nonfinite_and_negative_time_rejected(self):
        for bad in (True,-1,float('nan'),1.5):
            with self.assertRaises(ValueError):estimate_deadline(1,bad,1,'f','item',scope())


def with_toast(p, *texts):
    """Add the native 'toast' region (purchase_observation.h) with these lines."""
    projection = p['purchase_observation']
    words = [dict(text=text, x=1170, y=207 + 10 * i, width=226, height=28) for i, text in enumerate(texts)]
    projection['regions'].append(dict(kind='toast', bounds=[896, 150, 768, 102], words=words, ok=True, truncated=False,
                                      same_frame=True, frame_id=projection['frame_id'],
                                      frame_sha256=projection['frame_sha256']))
    return p


class LineGroupingTests(unittest.TestCase):
    def test_a_word_near_two_lines_joins_the_nearer(self):
        # Live buy03_01: a 2 px high dash in a card title sat close to two lines.
        words=[dict(text='AS',x=100,y=300,width=20,height=40),      # line centre 320
               dict(text='一',x=160,y=318,width=8,height=2),        # its own line, centre 319
               dict(text='一',x=200,y=318.5,width=8,height=2)]      # centre 319.5: close to both
        lines=text_lines(words,[0,0,400,400])   # raised PURCHASE_LINE_AMBIGUOUS before
        self.assertEqual(sum(len(l['words']) for l in lines),3)


class ToastTests(unittest.TestCase):
    def test_the_top_toast_is_read_with_its_kind(self):
        for text, kind in (('订单尚未开放购买', 'not_open_yet'), ('抢购队列已满，无法购买', 'queue_full'),
                           ('您已加入抢购池，将在抢购玩家名单收集完成后进行随机', 'lottery_pool'),
                           ('抢购请求玩家过多，请稍后重试', 'too_many_buyers')):
            with self.subTest(text=text):
                screen = read_screen(with_toast(packet(), text))
                self.assertEqual(screen['toast'], [dict(text=text.replace('，', ','), kinds=[kind])])   # NFKC
                self.assertEqual([(s['kind'], s['region']) for s in screen['signals']], [(kind, 'toast')])

    def test_packets_without_a_toast_region_still_read(self):
        screen = read_screen(packet())
        self.assertEqual((screen['toast'], screen['signals']), ([], []))
        bad = packet()
        bad['purchase_observation']['regions'].append(dict(bad['purchase_observation']['regions'][0], kind='banner'))
        with self.assertRaisesRegex(ValueError, 'PURCHASE_REGIONS'):
            read_screen(bad)

    def test_a_pressed_early_outcome(self):
        from run_purchase_timed_buy import OUTCOME_TEXT, outcome_of
        screen = read_screen(with_toast(packet(), '订单尚未开放购买'))
        self.assertEqual(outcome_of([screen]), 'not_open_yet')
        self.assertIn('按早了', OUTCOME_TEXT['not_open_yet'])


class ObservationTests(unittest.TestCase):
    def test_original_collection_rejects_watchlist_and_readonly_reuses_fields(self):
        p=packet();original=copy.deepcopy(p)
        with self.assertRaisesRegex(ValueError,'COLLECTION_PAGE'):match_selected_first_card(p,rule())
        c=match_watchlist_selected(p,rule())
        self.assertEqual(c['price'],'230');self.assertEqual(c['wear'],'0.187079')
        self.assertTrue(c['eligible']);self.assertFalse(c['actions_enabled'])
        self.assertEqual(p,original)

    def test_no_legacy_first_card_fallback(self):
        p=packet();p.pop('collection_layout')
        with self.assertRaisesRegex(ValueError,'OBSERVATION_REQUIRED'):match_watchlist_selected(p,rule())

    def test_frame_page_and_region_tampering_rejected(self):
        for k in ('frame','page','region','actions','words','bounds'):
            p=packet();v=p['purchase_observation']
            if k=='frame':v['frame_sha256']='b'*64
            elif k=='page':v['page']='skin_listings'
            elif k=='region':v['regions'][0]['frame_id']='stale'
            elif k=='actions':v['actions_enabled']=True
            elif k=='words':v['regions'][3]['words'][0]['x']=0
            else:v['regions'][0]['bounds'][2]=10000
            with self.subTest(k=k),self.assertRaises(ValueError):read_screen(p)

    def test_same_frame_countdown_button_and_purity(self):
        p=packet();old=copy.deepcopy(p);s=read_screen(p)
        self.assertEqual(s['countdown_seconds'],10);self.assertEqual(s['button_state'],'publicity')
        self.assertFalse(s['screen_result_is_order_receipt']);self.assertEqual(p,old)

    def test_two_countdowns_are_ambiguous_not_minimum(self):
        p=packet();r=p['purchase_observation']['regions'][3]
        r['words'].append(dict(text='9秒后解锁购买',x=2000,y=1200,width=300,height=24))
        self.assertIsNone(read_screen(p)['countdown_seconds'])

    def test_low_confidence_not_promoted_to_ready(self):
        p=packet();p['purchase_observation']['regions'][3]['words'][0]['score']=.7
        self.assertIsNone(read_screen(p)['countdown_seconds'])

    def test_split_same_line_joins_but_separate_lines_do_not(self):
        words=[dict(text='10',x=10,y=10,width=25,height=20),dict(text='秒后解锁购买',x=40,y=10,width=110,height=20)]
        self.assertEqual(text_lines(words,[0,0,200,100])[0]['text'],'10秒后解锁购买')
        words[1]['y']=50;self.assertEqual(len(text_lines(words,[0,0,200,100])),2)

    def test_unknown_product_is_not_bought_just_because_favorited(self):
        s=snapshot();s['rows'][0]['product_name']='unconfigured item'
        d=match_current_watchlist(packet(),s)
        self.assertEqual(d['decision'],'NoMatch');self.assertFalse(d['actions_enabled'])

    def test_original_price_wear_and_condition_thresholds_apply(self):
        for field,value in (('price_max','229'),('max_wear','0.1'),('condition_label','成色A')):
            s=snapshot();s['rows'][0][field]=value
            d=match_current_watchlist(packet(),s)
            self.assertNotEqual(d['decision'],'Match')

    def test_overlapping_rules_require_review_no_arbitrary_first_match(self):
        s=snapshot();s['rows'].append(dict(s['rows'][0],row_index=1))
        self.assertEqual(match_current_watchlist(packet(),s)['decision'],'NeedsReview')

    def test_recorded_watchlist_without_geometry_is_not_upgraded(self):
        root=Path(__file__).resolve().parents[2]
        path=root/'artifacts/hotkey_runs/20261009-092639/segment_01/session.steps/000003.json'
        if not path.is_file():self.skipTest('local historical watchlist capture absent')
        p=json.loads(path.read_text('utf8'))['result']
        self.assertEqual(p['startup_page']['page'],'watchlist_listings')
        with self.assertRaisesRegex(ValueError,'OBSERVATION_REQUIRED'):
            match_watchlist_selected(p,rule())

    def test_bbzps_literal_countdowns_are_static_test_data_not_time_unit_proof(self):
        root=Path(__file__).resolve().parents[2]
        path=root/'artifacts/purchase_readonly/bbzps_purchase_evidence.json'
        if not path.is_file():self.skipTest('local static evidence absent')
        data=json.loads(path.read_text('utf8'));good=bad=0
        for item in data['ocr_literals']:
            if '后解锁购买' not in item['text']:continue
            if item['text'].startswith(('日','品')):
                with self.assertRaises(ValueError):countdown_seconds(item['text'])
                bad+=1
            else:
                self.assertGreaterEqual(countdown_seconds(item['text']),0);good+=1
        self.assertGreater(good,0);self.assertGreater(bad,0)


class PlannerTests(unittest.TestCase):
    def read(self,p):return match_current_watchlist(p,snapshot())

    def test_countdown_wake_fresh_revalidation_and_ready_stop(self):
        plan=ReadonlyPurchasePlanner(scope())
        first=plan.consume(self.read(packet()),10001,scope())
        self.assertEqual(first['state'],'AwaitDeadline');self.assertFalse(first['capture_while_waiting'])
        self.assertEqual(plan.wake(19000,scope())['state'],'Revalidate')
        ready=plan.consume(self.read(packet('b',21000,None,'购买')),21001,scope())
        self.assertEqual(ready['state'],'AwaitUserPurchaseTest');self.assertEqual(ready['purchase_commands'],[])
        self.assertFalse(ready['purchase_authorized'])

    def test_zero_timer_never_means_purchase_ready(self):
        p=ReadonlyPurchasePlanner(scope()).consume(self.read(packet(timer='0秒后解锁购买')),10001,scope())
        self.assertEqual(p['state'],'AwaitDeadline')

    def test_stop_cancels_old_wake_and_future_observations(self):
        p=ReadonlyPurchasePlanner(scope());p.consume(self.read(packet()),10001,scope());p.stop()
        self.assertEqual(p.wake(20000,scope())['state'],'Stopped')
        self.assertEqual(p.consume(self.read(packet('b',21000,None,'购买')),21001,scope())['state'],'Stopped')

    def test_stale_duplicate_and_new_scope_never_revalidate(self):
        for mutation in ('stale','duplicate','scope'):
            p=ReadonlyPurchasePlanner(scope());d=self.read(packet());p.consume(d,10001,scope())
            result=p.consume(d,20000 if mutation=='stale' else 10001,
                Scope('session','monotonic','f'*64,0,0) if mutation=='scope' else scope())
            self.assertEqual(result['state'],'NeedsReview');self.assertFalse(result['purchase_authorized'])

    def test_mismatched_snapshot_and_ambiguous_timer_do_not_reach_ready(self):
        d=self.read(packet());d['config_sha256']='f'*64
        self.assertEqual(ReadonlyPurchasePlanner(scope()).consume(d,10001,scope())['reason'],'PURCHASE_CONTEXT_CHANGED')
        d=self.read(packet(timer=None,button='购买'));d['screen']['countdown_ambiguous']=True
        self.assertEqual(ReadonlyPurchasePlanner(scope()).consume(d,10001,scope())['state'],'NeedsReview')

    def test_no_match_is_skip_not_a_purchase_attempt(self):
        s=snapshot();s['rows'][0]['price_max']='229'
        d=match_current_watchlist(packet(),s)
        self.assertEqual(ReadonlyPurchasePlanner(scope()).consume(d,10001,scope())['state'],'Skip')

    def test_item_change_invalidates_old_deadline(self):
        p=ReadonlyPurchasePlanner(scope());p.consume(self.read(packet()),10001,scope())
        d=self.read(packet('b',11000));d['candidates'][0]['wear']='0.2'
        self.assertEqual(p.consume(d,11001,scope())['reason'],'PURCHASE_SELECTED_ITEM_CHANGED')
        self.assertIsNone(p.window)

    def test_failure_success_unknown_receipt_projection_never_writes_ledger(self):
        for kinds,want in ((['success_text'],'Success'),(['sold_or_removed'],'Failed'),
                (['success_text','sold_or_removed'],'Unknown'),(['queue_full'],'Unknown'),
                (['still_publicity'],'Unknown'),([],'Unknown')):
            s=dict(frame_id='f',signals=[dict(kind=k) for k in kinds])
            self.assertEqual(receipt_projection(s)['outcome'],'Unknown')
            r=receipt_projection(s,attempt_id='synthetic:1',association_confirmed=True)
            self.assertEqual(r['outcome'],want);self.assertFalse(r['ledger_committed']);self.assertFalse(r['retry_purchase'])
            self.assertFalse(r['reservation_mutated'])
            self.assertEqual(r['reservation_effect_on_commit'],{'Unknown':'keep','Success':'consume','Failed':'release'}[want])

    def test_association_uses_exact_decimal_not_binary_float(self):
        c=dict(product='AS Val',condition='成色S',price='230',wear='0.10')
        self.assertEqual(association(c),association(dict(c,product='ASVal',wear='0.100')))
        self.assertNotEqual(association(c),association(dict(c,wear='0.100001')))


class ProbeIsolationTests(unittest.TestCase):
    def test_probe_allowlist_excludes_every_side_effect_in_watchlist(self):
        from run_purchase_probe import allowed_step
        for kind in ('click','key','select_visible_card','collect_selected','scroll_visible_list','purchase','confirm'):
            self.assertFalse(allowed_step(dict(kind=kind,point=[2339,1250],key='enter',expected_before='watchlist_listings')))
        self.assertTrue(allowed_step(dict(kind='click',point=[2310,274],expected_before='skin_home')))
        self.assertTrue(allowed_step(dict(kind='capture',purchase_observation=True)))
        self.assertFalse(allowed_step(dict(kind='capture',expect_collection_added=True)))

    def test_probe_samples_classify_pages_like_its_navigation_reads(self):
        from run_purchase_probe import sample_step, allowed_step
        for page in ('empty_watchlist','watchlist_listings'):
            step=sample_step(page)
            self.assertTrue(step['ui_regions'] and step['purchase_observation'] and step['expected_page']==page)
            self.assertEqual(step['collection_layout'],page=='watchlist_listings')
            self.assertTrue(allowed_step(step))

    def test_probe_returns_to_the_window_it_started_from_by_default(self):
        from run_purchase_probe import return_policy
        self.assertEqual(return_policy('entry'),'entry_foreground')
        self.assertEqual(return_policy('ide'),'ide')
        with self.assertRaisesRegex(ValueError,'PURCHASE_PROBE_RETURN_POLICY'):return_policy('game')

    def test_navigation_reads_a_frame_mid_transition_again(self):
        from run_purchase_probe import UNKNOWN_REREADS,navigate
        class Session:
            def __init__(self,pages):self.pages,self.steps=list(pages),[]
            def perform(self,step):
                self.steps.append(step)
                self.previous=dict(startup_page=dict(page=self.pages.pop(0),overlay='none'))
        session=Session(['unknown','unknown','empty_watchlist'])
        self.assertEqual(navigate(session,pause=lambda s:None),'empty_watchlist')
        self.assertEqual([s['kind'] for s in session.steps],['capture']*3)
        session=Session(['unknown']*(UNKNOWN_REREADS+1))
        with self.assertRaisesRegex(ValueError,'PURCHASE_PROBE_PAGE:unknown'):navigate(session,pause=lambda s:None)
        self.assertEqual(len(session.steps),UNKNOWN_REREADS+1)

    def test_a_listing_filter_left_open_by_a_collection_is_closed_once(self):
        # Live buy_cycle02 2026-10-10: a stopped collection left the 成色 panel
        # open; the purchase refused every attempt after it.
        from run_purchase_probe import LISTING_FILTER_CONFIRM,allowed_step,navigate
        class Session:
            def __init__(self,screens):self.screens,self.steps=list(screens),[]
            def perform(self,step):
                self.steps.append(step)
                if step['kind']=='capture':
                    page,overlay=self.screens.pop(0)
                    self.previous=dict(startup_page=dict(page=page,overlay=overlay))
        session=Session([('skin_listings','listing_filter'),('skin_listings','none'),('skin_home','none'),
                         ('watchlist_listings','none')])
        self.assertEqual(navigate(session,pause=lambda s:None),'watchlist_listings')
        click=session.steps[1]
        self.assertEqual((click['kind'],click['point'],click['expected_overlay']),('click',LISTING_FILTER_CONFIRM,'listing_filter'))
        self.assertTrue(allowed_step(click))
        self.assertEqual(session.steps[3]['kind'],'key')
        # Still open after the confirm, or any other panel: refused, nothing else clicked.
        for screens in ([('skin_listings','listing_filter'),('skin_listings','listing_filter')],
                        [('skin_listings','purchase_dialog')],[('watchlist_listings','listing_filter')]):
            session=Session(screens)
            with self.subTest(screens=screens),self.assertRaisesRegex(ValueError,'OVERLAY_REQUIRES_REVIEW'):
                navigate(session,pause=lambda s:None)
            self.assertLessEqual(sum(1 for s in session.steps if s['kind']=='click'),1)
        for bad in (dict(kind='click',point=[2340,1355],expected_before='skin_listings',expected_overlay='purchase_dialog'),
                    dict(kind='click',point=[2339,1250],expected_before='skin_listings',expected_overlay='listing_filter'),
                    dict(kind='click',point=[2310,274],expected_before='skin_home',expected_overlay='listing_filter')):
            self.assertFalse(allowed_step(bad))

    def test_the_catalog_filter_left_open_by_a_collection_is_escaped(self):
        # Review wf_993b38d0-6b8: a collection stopped inside the 典藏 filter dialog.
        from run_purchase_probe import allowed_step,navigate
        class Session:
            def __init__(self,screens):self.screens,self.steps=list(screens),[]
            def perform(self,step):
                self.steps.append(step)
                if step['kind']=='capture':
                    page,overlay=self.screens.pop(0)
                    self.previous=dict(startup_page=dict(page=page,overlay=overlay))
        for screens,escapes in (([('catalog_filter','none'),('skin_home','none'),('watchlist_listings','none')],1),
                                ([('catalog_filter','none'),('catalog_filter','none'),('skin_home','none'),
                                  ('watchlist_listings','none')],2)):
            session=Session(screens)
            with self.subTest(escapes=escapes):
                self.assertEqual(navigate(session,pause=lambda s:None),'watchlist_listings')
                keys=[s for s in session.steps if s['kind']=='key']
                self.assertEqual([k['expected_before'] for k in keys],['catalog_filter']*escapes)
                self.assertTrue(all(allowed_step(k) for k in keys))
        session=Session([('catalog_filter','none')]*3)
        with self.assertRaisesRegex(ValueError,'PURCHASE_PROBE_PAGE:catalog_filter'):navigate(session,pause=lambda s:None)
        self.assertEqual(sum(1 for s in session.steps if s['kind']=='key'),2)
        self.assertFalse(allowed_step(dict(kind='key',key='enter',expected_before='catalog_filter')))

    def test_probe_import_never_loads_input_backend(self):
        path=Path(__file__).resolve().parent
        code="import sys;sys.path.insert(0,sys.argv[1]);import run_purchase_probe;assert 'navigate_lobby_to_warehouse' not in sys.modules;assert 'collection_live_session' not in sys.modules;print('INERT_IMPORT=PASS')"
        r=subprocess.run([sys.executable,'-B','-X','utf8','-c',code,str(path)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr);self.assertIn('INERT_IMPORT=PASS',r.stdout)


if __name__=='__main__':unittest.main()
