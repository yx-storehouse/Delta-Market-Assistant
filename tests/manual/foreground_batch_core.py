"""Pure bounded batch coordinator; OS operations are injected by the manual runner."""
import time
import math


def filter_expectation_matches(result, expected):
    """Strict diagnostic assertion, never infer missing/Unknown as unchecked."""
    if not isinstance(expected, dict) or not expected:
        return False
    state=result.get('catalog_filter_state',{})
    frames=result.get('frames',[])
    digest=state.get('frame_sha256')
    if (state.get('valid_page') is not True or state.get('complete') is not True or state.get('same_frame') is not True
        or not isinstance(state.get('frame_id'),str) or not state['frame_id']
        or not isinstance(digest,str) or len(digest)!=64 or any(c not in '0123456789abcdef' for c in digest)
        or not frames or digest!=frames[-1].get('sha256')):
        return False
    for key,value in expected.items():
        if key=='season':
            if not isinstance(value,str) or state.get('season_label')!=value:return False
        elif key not in ('owned','unowned','legendary','epic','rare','common') or value not in ('checked','unchecked'):
            return False
        elif state.get('checkboxes',{}).get(key,{}).get('state')!=value:
            return False
    return True


def filter_matches_under_pointer(result, expected, pointer_key):
    """filter_expectation_matches for a readback taken with the pointer still
    resting on the box it just clicked: that one box may read 'unknown' (its
    hover glow hides the tick) or its expected state; every other expected box
    must read exactly. The caller verifies the whole set strictly once the
    pointer has left the boxes (user 2026-10-09: no return trips)."""
    try:
        state = result['catalog_filter_state']
        frames = result.get('frames') or []
        digest = state.get('frame_sha256')
        if (state.get('valid_page') is not True or state.get('same_frame') is not True
                or not isinstance(state.get('frame_id'), str) or not state['frame_id']
                or not frames or digest != frames[-1].get('sha256')):
            return False
        boxes = state['checkboxes']
        others = [key for key in boxes if key != pointer_key]
        if any(boxes[key].get('state') not in ('checked', 'unchecked') for key in others):
            return False
        for key, value in expected.items():
            if key == 'season':
                if state.get('season_label') != value:
                    return False
            elif boxes[key].get('state') not in ((value, 'unknown') if key == pointer_key else (value,)):
                return False
        return True
    except (KeyError, TypeError, AttributeError):
        return False


def filter_readback_unsettled(result, expected):
    """A filter readback that may still be settling: every expected box reads
    either its expected state or 'unknown' (hover glow / tick animation, live
    2026-10-09 09:56: the just-ticked 已拥有 read E_FILTER_CHECKBOX_AMBIGUOUS
    under the resting pointer), at least one is unknown, none is opposite.
    Authorizes another read only, never another click."""
    try:
        state = result['catalog_filter_state']
        if state.get('valid_page') is not True or state.get('same_frame') is not True:
            return False
        actual = {key: state['checkboxes'][key]['state'] for key in expected if key != 'season'}
        if 'season' in expected and state.get('season_label') != expected['season']:
            return False
        return (all(actual[key] in (value, 'unknown') for key, value in expected.items() if key != 'season')
                and any(value == 'unknown' for value in actual.values()))
    except (KeyError, TypeError, AttributeError):
        return False


def stabilize_capture(capture, is_foreground, *, attempts=3, deadline, now=time.monotonic, wait=time.sleep,
                      retry_observation=None, retry_delay=None):
    """Bound new observations; optional caller predicate never repeats inputs."""
    # 4 is used only by the post-receipt layout recheck (its own budget).
    if type(attempts) is not int or not 1 <= attempts <= 4:
        raise ValueError('Capture attempts must be 1..4.')
    if retry_delay is not None and not callable(retry_delay):
        raise ValueError('Capture retry delay must be callable.')
    history=[]
    for index in range(attempts):
        if now()>=deadline: raise TimeoutError('BATCH_DEADLINE')
        if not is_foreground(): raise RuntimeError('BATCH_FOREGROUND_LOST')
        observed=capture(deadline-now());history.append(observed)
        result=observed.get('result',{})
        retry=(observed.get('exit_status')==1 and result.get('page_error')=='E_DIAGNOSTIC_PAGE_MISMATCH'
            and result.get('startup_page',{}).get('page')=='unknown')
        if not observed.get('passed') and not retry and retry_observation is not None:
            retry = retry_observation(observed) is True
        if observed.get('passed') or not retry or index+1==attempts: break
        delay=.15 if retry_delay is None else retry_delay(observed)
        if type(delay) not in (int,float) or not math.isfinite(delay) or not .01<=delay<=.5:
            raise ValueError('Capture retry delay must be within .01..0.5 seconds.')
        actual=min(delay,max(0,deadline-now()))
        observed['retry_wait_ms']=actual*1000
        wait(actual)
    output=dict(history[-1]);output['attempts']=history;output['attempt_count']=len(history)
    return output


def execute_batch(steps, enter, perform, leave, is_foreground, *, timeout=30, now=time.monotonic):
    if not isinstance(steps, list) or not 1 <= len(steps) <= 20 or not 1 <= timeout <= 30:
        raise ValueError('A batch needs 1..20 steps and a 1..30 second bound.')
    result = dict(passed=False, steps=[], enter_calls=0, leave_calls=0, error=None)
    deadline = now() + timeout
    try:
        result['enter_calls'] += 1
        enter()
        for step in steps:
            if now() >= deadline:
                raise TimeoutError('BATCH_DEADLINE')
            if not is_foreground():
                raise RuntimeError('BATCH_FOREGROUND_LOST')
            observed = perform(step, deadline - now())
            result['steps'].append(observed)
            if not observed.get('passed'):
                raise RuntimeError('BATCH_STEP_FAILED')
            if not is_foreground():
                raise RuntimeError('BATCH_FOREGROUND_LOST')
        result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
    finally:
        result['leave_calls'] += 1
        try:
            result['ide_restored'] = bool(leave())
        except Exception as error:
            result['ide_restored'] = False
            result['restore_error'] = str(error)
        result['passed'] = result['passed'] and result['ide_restored']
    return result
