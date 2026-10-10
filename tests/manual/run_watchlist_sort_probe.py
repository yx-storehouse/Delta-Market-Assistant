"""Live check of the 我的关注 sort step: navigate, sort by 按稀有度升序, read
back. Inputs: navigation, the sort dropdown and its option only.

Importing this module is inert; input is sent only from main().
"""
import argparse
import base64
import json
from pathlib import Path

from collection_paths import project_root
from purchase_observation import read_screen
from purchase_watchlist_sort import controls_text, ensure_rarity_sort, sort_step_permitted
from run_purchase_probe import RETURN_POLICIES, allowed_step, navigate, return_policy


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--return-to', choices=sorted(RETURN_POLICIES), default='entry')
    p.add_argument('--first', help='sort by this label first (to test the switch), then by 按稀有度升序')
    args = p.parse_args()
    root = project_root(__file__)
    output = args.output.resolve()
    if not output.is_relative_to(root / 'artifacts'):
        raise ValueError('SORT_PROBE_OUTPUT_DIRECTORY')
    output.mkdir(parents=True, exist_ok=False)
    from collection_live_session import ForegroundSession
    from run_collection_observed import MemoryReviewBackend
    from run_exclusive import exclusive_run
    plan = dict(sort_armed=False)

    class SortSession(ForegroundSession):
        def perform(self, step, remaining=None):
            if not (allowed_step(step) or sort_step_permitted(step, plan)):
                raise ValueError('SORT_PROBE_STEP_NOT_ALLOWED')
            return super().perform(step, remaining)

    backend = MemoryReviewBackend(root, local_title=True, local_price=True, fast_capture=True, local_hotpath=False,
                                  ready_stream=False, fast_actions=False, parallel_local_price=True,
                                  return_policy=return_policy(args.return_to))
    session = SortSession(output / 'session.json', backend=backend, timeout_seconds=60, max_steps=40,
                          numeric_price=True, fast_settle=False)
    result = dict(mode='watchlist_sort_probe', reads=[], status='blocked')
    counter = [0]

    def read():
        counter[0] += 1
        session.perform(dict(kind='capture', collection_observation=True, ui_regions=True, purchase_observation=True,
                             preview=True))
        if session.last_preview:
            (output / ('read_%02d.png' % counter[0])).write_bytes(base64.b64decode(session.last_preview))
        screen = read_screen(session.previous)
        result['reads'].append(dict(n=counter[0], page=screen['page'], overlay=screen['overlay'],
                                    controls=controls_text(session.previous),
                                    lines={k: [l['text'] for l in v] for k, v in screen['regions'].items()}))
        return screen, session.previous

    try:
        with exclusive_run(), session:
            result['page'] = navigate(session)
            if args.first:
                first = result['first_record'] = {}
                result['first'] = ensure_rarity_sort(session, backend, read, plan, first, label=args.first)
            record = {}
            result['sort'] = ensure_rarity_sort(session, backend, read, plan, record)
            result['record'] = record
        result['status'] = 'passed'
    except Exception as error:
        result['error'] = str(error) or type(error).__name__
    result.update(ide_restored=session.report.get('ide_restored'), plan=plan)
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2, default=str) + '\n',
                                        encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'reads'}, ensure_ascii=False, default=str))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
