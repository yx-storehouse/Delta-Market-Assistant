"""Independent transaction for real selection-to-star latency reduction."""
from pathlib import Path
import sys
import verify_collection_scroll_speed_package as verified

def configure():
    root=Path(__file__).resolve().parents[2]
    verified.TX=root/'artifacts/collection_action_speed'
    verified.BASELINE=verified.TX/'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256='272bf1491d5ae94f419513cbf75014496250d32fa6aaa08fff43c6016750602c'
    return verified

def main():
    configure()
    if '--source-module' not in sys.argv:
        sys.argv+=['--source-module','tests/manual/collection_live_session.py']
    if '--runtime-module' not in sys.argv:
        sys.argv+=['--runtime-module','collection/collection_live_session.py']
    return verified.main()

if __name__=='__main__':raise SystemExit(main())
