"""Pure watchlist planning. Emits observations/review, never executable input.

The existing C++ ReplayReducer and IEventStore remain the attempt/ledger
owners. This module does not introduce a second purchase ledger or interpret
the collection journal as a queue of orders.
"""
import hashlib
import json
from dataclasses import dataclass
from decimal import Decimal,InvalidOperation


def integer(value,low=0,high=2**53-1):
    if type(value) is not int or not low<=value<=high:raise ValueError('PURCHASE_INTEGER')
    return value


def number(value):
    if not isinstance(value,str) or not value or len(value)>64:raise ValueError('PURCHASE_NUMBER')
    try:n=Decimal(value)
    except InvalidOperation as error:raise ValueError('PURCHASE_NUMBER') from error
    if not n.is_finite() or n<0:raise ValueError('PURCHASE_NUMBER')
    return n


def association(candidate):
    from purchase_observation import normalize
    required=('product','condition','price','wear')
    if any(not isinstance(candidate.get(k),str) or not candidate[k] for k in required):raise ValueError('PURCHASE_ASSOCIATION')
    value=[normalize(candidate['product']),normalize(candidate['condition']),str(number(candidate['price']).normalize()),str(number(candidate['wear']).normalize())]
    return hashlib.sha256(json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()).hexdigest()


@dataclass(frozen=True)
class Scope:
    session_id:str
    clock_domain_id:str
    config_sha256:str
    viewport_generation:int
    cancel_epoch:int

    def __post_init__(self):
        import re
        if not self.session_id or not self.clock_domain_id or not re.fullmatch('[0-9a-f]{64}',self.config_sha256):
            raise ValueError('PURCHASE_SCOPE')
        integer(self.viewport_generation);integer(self.cancel_epoch)


@dataclass(frozen=True)
class DeadlineWindow:
    earliest_ms:int
    latest_ms:int
    source_ms:int
    source_frame_id:str
    association_ref:str
    scope:Scope


def estimate_deadline(seconds,source_ms,uncertainty_ms,frame_id,association_ref,scope):
    integer(seconds,0,86400);integer(source_ms);integer(uncertainty_ms,0,5000)
    if not frame_id or not association_ref:raise ValueError('PURCHASE_FRAME_REQUIRED')
    # Neither floor nor ceil rounding of the game's integer display is proven.
    # Keep a +/- one-second window; expiry only schedules another observation.
    return DeadlineWindow(max(0,source_ms-uncertainty_ms+max(0,seconds-1)*1000),
        source_ms+uncertainty_ms+(seconds+1)*1000,source_ms,frame_id,association_ref,scope)


def refine_deadline(previous,current):
    if current.scope!=previous.scope or current.association_ref!=previous.association_ref:
        raise ValueError('PURCHASE_DEADLINE_CONTEXT_CHANGED')
    if current.source_ms<=previous.source_ms or current.source_frame_id==previous.source_frame_id:
        raise ValueError('PURCHASE_NEW_FRAME_REQUIRED')
    low=max(previous.earliest_ms,current.earliest_ms);high=min(previous.latest_ms,current.latest_ms)
    if low>high:raise ValueError('PURCHASE_COUNTDOWN_DISCONTINUITY')
    return DeadlineWindow(low,high,current.source_ms,current.source_frame_id,current.association_ref,current.scope)


class ReadonlyPurchasePlanner:
    """One selected listing at a time, matching BBZPS evidence without sorting
    unobserved offers. A timer callback can only request a new observation.
    """
    def __init__(self,scope):
        if not isinstance(scope,Scope):raise ValueError('PURCHASE_SCOPE')
        self.scope=scope;self.window=None;self.last_frame=None;self.last_source=-1
        self.identity=None;self.state='Watchlist';self.stopped=False

    def stop(self):
        self.stopped=True;self.window=None;self.state='Stopped'
        return self._result('Stopped','USER_STOP',None)

    def _result(self,state,reason,wake):
        self.state=state
        return dict(schema='purchase-readonly-plan-v1',state=state,reason=reason,wake_mono_ms=wake,
            association_ref=self.identity,actions_enabled=False,purchase_authorized=False,
            purchase_commands=[],capture_while_waiting=False,requires_fresh_revalidation=True,
            delay_policy_applied=False,quota_policy_applied=False)

    def consume(self,observation,now_ms,scope):
        integer(now_ms)
        if self.stopped:return self._result('Stopped','USER_STOP',None)
        if scope!=self.scope or observation.get('config_sha256')!=self.scope.config_sha256:
            self.window=None;return self._result('NeedsReview','PURCHASE_CONTEXT_CHANGED',None)
        screen=observation['screen'];frame=screen['source_frame']
        source=integer(frame['source_mono_ms']);uncertainty=integer(frame['source_uncertainty_ms'],0,5000)
        fid=screen['frame_id']
        if source>now_ms+uncertainty or now_ms-source+uncertainty>5000:
            return self._result('NeedsReview','PURCHASE_STALE_OR_FUTURE_FRAME',None)
        if source<=self.last_source or fid==self.last_frame:
            return self._result('NeedsReview','PURCHASE_NEW_FRAME_REQUIRED',None)
        self.last_source,self.last_frame=source,fid
        if observation['decision']=='Empty':
            self.window=None;self.identity=None;return self._result('Empty','NO_WATCHLIST_ITEMS',None)
        if observation['decision']=='NoMatch':
            self.window=None;self.identity=None;return self._result('Skip','CURRENT_ITEM_OUTSIDE_CONFIGURED_RULES',None)
        if observation['decision']!='Match' or len(observation['candidates'])!=1:
            self.window=None;return self._result('NeedsReview',observation['decision'],None)
        candidate=observation['candidates'][0]
        if candidate.get('eligible') is not True or candidate.get('actions_enabled') is not False:
            raise ValueError('PURCHASE_READONLY_CANDIDATE')
        if candidate.get('source_frame_id')!=fid or candidate.get('source_frame_sha256')!=screen['frame_sha256']:
            raise ValueError('PURCHASE_CANDIDATE_FRAME')
        key=association(candidate)
        if self.identity is not None and self.identity!=key:
            self.window=None;self.identity=key
            return self._result('NeedsReview','PURCHASE_SELECTED_ITEM_CHANGED',None)
        self.identity=key
        if screen['signals']:
            self.window=None;return self._result('NeedsReview','PURCHASE_DIALOG_OR_RESULT_VISIBLE',None)
        if screen['countdown_unread'] or screen.get('countdown_ambiguous') or screen['button_state']=='conflict':
            self.window=None;return self._result('NeedsReview','PURCHASE_COUNTDOWN_UNREAD',None)
        seconds=screen['countdown_seconds']
        if seconds is not None:
            if screen['button_state']!='publicity':return self._result('NeedsReview','PURCHASE_TIMER_BUTTON_CONFLICT',None)
            current=estimate_deadline(seconds,source,uncertainty,fid,key,self.scope)
            try:self.window=refine_deadline(self.window,current) if self.window else current
            except ValueError as error:
                self.window=None;return self._result('NeedsReview',str(error),None)
            # Long waits suspend capture. Wake before the uncertainty window;
            # near-expiry reads remain bounded by the caller's demand budget.
            wake=max(now_ms+100,self.window.earliest_ms-1000)
            return self._result('AwaitDeadline','PUBLICITY_COUNTDOWN',wake)
        if screen['button_state']=='buy_visible':
            self.window=None
            return self._result('AwaitUserPurchaseTest','FRESH_MATCH_READY_FOR_REVIEW',None)
        self.window=None;return self._result('NeedsReview','PURCHASE_BUTTON_UNPROVEN',None)

    def wake(self,now_ms,scope):
        integer(now_ms)
        if self.stopped:return self._result('Stopped','USER_STOP',None)
        if scope!=self.scope or self.window is None:return self._result('NeedsReview','PURCHASE_NO_CURRENT_DEADLINE',None)
        if now_ms<self.window.earliest_ms-1000:
            return self._result('AwaitDeadline','PUBLICITY_COUNTDOWN',self.window.earliest_ms-1000)
        return self._result('Revalidate','FRESH_FRAME_REQUIRED_AFTER_WAKE',None)


def receipt_projection(screen,*,attempt_id=None,association_confirmed=False):
    """Projection for the existing runtime's Receipt/Unknown event adapter.

    A screen text alone never proves which attempt it belongs to. The live
    adapter is not bound here, and no persistent ledger is changed.
    """
    kinds={x['kind'] for x in screen['signals']}
    success='success_text' in kinds
    failed=bool(kinds&{'sold_or_removed','insufficient_balance'})
    # Queue/publicity are retry-policy observations, not terminal order facts.
    if not attempt_id or association_confirmed is not True or success==failed or (success and kinds&{'queue_full','still_publicity'}):
        outcome='Unknown'
    else:outcome='Success' if success else 'Failed'
    return dict(attempt_id=attempt_id,outcome=outcome,reservation_mutated=False,
        reservation_effect_on_commit={'Unknown':'keep','Success':'consume','Failed':'release'}[outcome],
        receipt_identity=screen['frame_id'],observed_kinds=sorted(kinds),
        authoritative_ledger_owner='relink::ledger::IEventStore',ledger_committed=False,
        purchase_commands=[],retry_purchase=False)
