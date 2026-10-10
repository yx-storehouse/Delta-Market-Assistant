"""Run a finite legacy plan through one shared collection focus session.

The CLI still accepts 1..20 steps and 1..30 seconds. Continuous orchestration
imports ForegroundSession instead, keeping one focus lease across subplans.
"""
import argparse
import json
from pathlib import Path

from collection_live_session import ForegroundSession, KINDS, ROOT


def validate_plan(plan):
    steps = plan['steps']
    if not isinstance(steps, list) or not 1 <= len(steps) <= 20:
        raise ValueError('A batch needs 1..20 steps.')
    if any(not isinstance(step, dict) or step.get('kind') not in KINDS for step in steps):
        raise ValueError('COLLECTION_STEP_KIND')
    if steps[0]['kind'] != 'capture':
        raise ValueError('A batch starts with observation.')
    timeout = plan.get('timeout_seconds', 25)
    if type(timeout) not in (int, float) or not 1 <= timeout <= 30:
        raise ValueError('A batch needs a 1..30 second bound.')
    return steps, timeout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True)
    parser.add_argument('--record', required=True)
    args = parser.parse_args()
    plan = json.loads(Path(args.plan).read_text(encoding='utf-8'))
    steps, timeout = validate_plan(plan)
    session = ForegroundSession(args.record, timeout_seconds=timeout, max_steps=20, plan=plan)
    try:
        with session:
            for step in steps:
                session.perform(step)
    except Exception:
        # Detail is persisted before cleanup. Never retry a failed input.
        pass
    print(json.dumps(dict(metadata=session.report, preview_jpeg=session.preview_jpeg()), ensure_ascii=False))
    return 0 if session.report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
