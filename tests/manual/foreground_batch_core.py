"""Pure bounded batch coordinator; OS operations are injected by the manual runner."""
import time


def execute_batch(steps, enter, perform, leave, is_foreground, *, timeout=30, now=time.monotonic):
    if not isinstance(steps, list) or not 1 <= len(steps) <= 12 or not 1 <= timeout <= 30:
        raise ValueError('A batch needs 1..12 steps and a 1..30 second bound.')
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
