"""Read-only purchase rehearsal of a recorded capture; never opens the game.

Use --packet with an explicitly captured --purchase-observation JSON and the
current config/catalog. Output is not an order, click plan or game receipt.
"""
import argparse
import json
from pathlib import Path
import sys

from collection_run_config import snapshot_from_files
from purchase_observation import match_current_watchlist
from purchase_plan import Scope,ReadonlyPurchasePlanner


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--packet',type=Path,required=True)
    p.add_argument('--config',type=Path,required=True)
    p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    data=args.packet.read_bytes()
    if not 0<len(data)<=8*1024*1024:raise ValueError('PURCHASE_PACKET_SIZE')
    def pairs(items):
        d={}
        for k,v in items:
            if k in d:raise ValueError('PURCHASE_DUPLICATE_JSON_KEY')
            d[k]=v
        return d
    packet=json.loads(data.decode('utf-8-sig'),object_pairs_hook=pairs,
        parse_constant=lambda _:(_ for _ in ()).throw(ValueError('PURCHASE_NONFINITE_JSON')))
    snapshot=snapshot_from_files(args.config,args.catalog)
    observed=match_current_watchlist(packet,snapshot)
    scope=Scope('offline-rehearsal','recorded-frame-clock',snapshot['config_sha256'],0,0)
    source=observed['screen']['source_frame']
    # Replaying at the recorded frame's own time is explicitly not current
    # freshness proof and cannot enable input in this CLI.
    planner=ReadonlyPurchasePlanner(scope)
    plan=planner.consume(observed,source['source_mono_ms']+source['source_uncertainty_ms'],scope)
    output=dict(mode='recorded_readonly_rehearsal',config_sha256=snapshot['config_sha256'],
        observation=observed,plan=plan,game_input_sent=False,purchase_phase_started=False,
        current_live_frame_proven=False)
    if args.output.resolve() in (args.packet.resolve(),args.config.resolve(),args.catalog.resolve()):
        raise ValueError('PURCHASE_OUTPUT_IS_INPUT')
    with args.output.open('x',encoding='utf8') as f:
        json.dump(output,f,ensure_ascii=False,indent=2,allow_nan=False);f.write('\n')
    assert json.loads(args.output.read_text('utf8'))==output
    print(json.dumps(dict(status='passed',decision=observed['decision'],state=plan['state'],
        output=str(args.output.resolve()),game_input_sent=False,purchase_phase_started=False),ensure_ascii=False))
    return 0


if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError,KeyError,TypeError) as error:
        print(json.dumps(dict(status='blocked',error=str(error),game_input_sent=False),ensure_ascii=False),file=sys.stderr)
        raise SystemExit(1)
