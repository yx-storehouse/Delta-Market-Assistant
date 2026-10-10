"""Isolated reuse of the proven verifier for diagnostic-overhead reduction."""
from pathlib import Path
import sys

import verify_collection_scroll_speed_package as verified

def configure():
    root=Path(__file__).resolve().parents[2]
    verified.TX=root/'artifacts/collection_roi_speed'
    verified.BASELINE=verified.TX/'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256='3fe79e8410fc06df30838d5f3e41a1fe401e1de831b8d7f57fa12a9c2d8cd41b'
    return verified

def main():
    configure()
    if '--source-module' not in sys.argv:
        sys.argv+=['--source-module','tests/manual/run_collection_observed.py']
    if '--runtime-module' not in sys.argv:
        sys.argv+=['--runtime-module','collection/run_collection_observed.py']
    return verified.main()

if __name__=='__main__':raise SystemExit(main())
