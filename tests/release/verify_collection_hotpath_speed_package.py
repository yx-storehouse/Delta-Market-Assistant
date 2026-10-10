"""Isolated delivery transaction for hot capture resources and sort refinement."""
from pathlib import Path
import sys
import verify_collection_scroll_speed_package as verified

def configure():
    root=Path(__file__).resolve().parents[2]
    verified.TX=root/'artifacts/collection_hotpath_speed'
    verified.BASELINE=verified.TX/'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256='623f380f3a293b87fafe145e012a4f640a1540a88fba45d7a299fa219d503a1c'
    return verified

def main():
    configure()
    if '--source-module' not in sys.argv:
        sys.argv+=['--source-module','tests/manual/run_collection_observed.py']
    if '--runtime-module' not in sys.argv:
        sys.argv+=['--runtime-module','collection/run_collection_observed.py']
    return verified.main()

if __name__=='__main__':raise SystemExit(main())
