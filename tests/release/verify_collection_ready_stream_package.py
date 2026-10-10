"""Independent release transaction for session-scoped latest-frame capture."""
from pathlib import Path
import sys
import verify_collection_scroll_speed_package as verified


def configure():
    verified.TX = Path(__file__).resolve().parents[2] / 'artifacts/collection_ready_stream'
    verified.BASELINE = verified.TX / 'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256 = 'c733a612eb1c7f15570cf291e14abc82361cf865c107e5a0b33fcdc4f1dc90ac'
    return verified


def main():
    configure()
    if '--source-module' not in sys.argv:
        sys.argv += ['--source-module', 'tests/manual/run_collection_observed.py']
    if '--runtime-module' not in sys.argv:
        sys.argv += ['--runtime-module', 'collection/run_collection_observed.py']
    return verified.main()


if __name__ == '__main__':
    raise SystemExit(main())
