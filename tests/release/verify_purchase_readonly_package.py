"""Purchase observation delivery reuses the established isolated rollback harness."""
from pathlib import Path
import sys
import verify_collection_scroll_speed_package as verified


def main():
    verified.TX=Path(__file__).resolve().parents[2]/'artifacts/purchase_readonly'
    verified.BASELINE=verified.TX/'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256='20d3ed757e53c7dbf46277901753ea8261c04515143b6f124199937f5a81d89e'
    if '--source-module' not in sys.argv:sys.argv+=['--source-module','tests/manual/collection_candidate.py']
    if '--runtime-module' not in sys.argv:sys.argv+=['--runtime-module','collection/collection_candidate.py']
    return verified.main()


if __name__=='__main__':main()
