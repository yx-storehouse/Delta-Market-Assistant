"""Independent transaction for observed paired-tail gaps and title latency."""
from pathlib import Path
import sys
import verify_collection_scroll_speed_package as verified


def main():
    verified.TX=Path(__file__).resolve().parents[2]/'artifacts/collection_tail_latency'
    verified.BASELINE=verified.TX/'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256='fbc63018a389f177439e9ef34c56c9e7b3a5bf1448f63290dfae296a5d309a06'
    if '--source-module' not in sys.argv:
        sys.argv+=['--source-module','tests/manual/collection_local_title.py']
    if '--runtime-module' not in sys.argv:
        sys.argv+=['--runtime-module','collection/collection_local_title.py']
    return verified.main()


if __name__=='__main__':main()
