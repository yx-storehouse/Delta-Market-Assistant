"""Compatibility CLI: preflight + action + readback share ONE foreground batch.
Prefer run_foreground_batch.py with the entire planned navigation sequence.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m2_lobby_calibration'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--key',required=True)
    parser.add_argument('--from-page',required=True)
    parser.add_argument('--to-page')
    parser.add_argument('--x',type=int,required=True)
    parser.add_argument('--y',type=int,required=True)
    parser.add_argument('--width',type=int,required=True)
    parser.add_argument('--height',type=int,required=True)
    parser.add_argument('--action',choices=['hover','click'],required=True)
    parser.add_argument('--preview',action='store_true')
    args=parser.parse_args()
    assert re.fullmatch('[a-z0-9_]{1,60}',args.key)
    plan=dict(name=args.key,timeout_seconds=25,steps=[
        dict(kind='capture',expected_page=args.from_page),
        dict(kind=args.action,point=[args.x,args.y],viewport=[args.width,args.height],expected_before=args.from_page),
        dict(kind='capture',expected_page=args.to_page,preview=args.preview)])
    plan_path=TX/'plans'/(args.key+'.json')
    plan_path.parent.mkdir(parents=True,exist_ok=True)
    with plan_path.open('x',encoding='utf-8') as file:json.dump(plan,file,ensure_ascii=False,indent=2)
    command=[sys.executable,str(ROOT/'tests/manual/run_foreground_batch.py'),'--plan',str(plan_path),
        '--record',str(TX/('navigation_'+args.key+'.json'))]
    return subprocess.run(command,creationflags=subprocess.CREATE_NO_WINDOW).returncode

if __name__=='__main__':raise SystemExit(main())
