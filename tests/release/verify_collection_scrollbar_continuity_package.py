"""Separate transaction for pointer routing and receipt recovery overhead."""
from pathlib import Path
import sys
import verify_collection_scroll_speed_package as verified

def configure():
    verified.TX=Path(__file__).resolve().parents[2]/'artifacts/collection_scrollbar_continuity'
    verified.BASELINE=verified.TX/'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256='c733a612eb1c7f15570cf291e14abc82361cf865c107e5a0b33fcdc4f1dc90ac'
    return verified

def main():
    configure()
    if '--source-module' not in sys.argv:
        sys.argv+=['--source-module','tests/manual/collection_live_session.py']
    if '--runtime-module' not in sys.argv:
        sys.argv+=['--runtime-module','collection/collection_live_session.py']
    return verified.main()

if __name__=='__main__':raise SystemExit(main())
