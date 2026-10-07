"""Pure bounded batch coordinator; OS operations are injected by the manual runner."""
import time


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


def stabilize_capture(capture, is_foreground, *, attempts=3, deadline, now=time.monotonic, wait=time.sleep):
    """Retry only an unknown-page observation, never clicks or window failures."""
    if type(attempts) is not int or not 1 <= attempts <= 3:
        raise ValueError('Capture attempts must be 1..3.')
    history=[]
    for index in range(attempts):
        if now()>=deadline: raise TimeoutError('BATCH_DEADLINE')
        if not is_foreground(): raise RuntimeError('BATCH_FOREGROUND_LOST')
        observed=capture(deadline-now());history.append(observed)
        result=observed.get('result',{})
        retry=(observed.get('exit_status')==1 and result.get('page_error')=='E_DIAGNOSTIC_PAGE_MISMATCH'
            and result.get('startup_page',{}).get('page')=='unknown')
        if observed.get('passed') or not retry or index+1==attempts: break
        wait(min(.15,max(0,deadline-now())))
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
