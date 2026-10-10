"""Independent package transaction for the collection-local recognition path."""
from pathlib import Path
import sys
import verify_collection_scroll_speed_package as verified


def configure():
    root = Path(__file__).resolve().parents[2]
    verified.TX = root / 'artifacts/collection_local_hotpath'
    verified.BASELINE = verified.TX / 'baseline/release'
    verified.EXPECTED_BASELINE_EXE_SHA256 = '8f5ba63a4eab429f5ba9761b93a5998b6c3a4441a7e20eacda4bd76849ce41cf'
    return verified


def main():
    configure()
    if '--source-module' not in sys.argv:
        sys.argv += ['--source-module', 'tests/manual/collection_live_session.py']
    if '--runtime-module' not in sys.argv:
        sys.argv += ['--runtime-module', 'collection/collection_live_session.py']
    return verified.main()


if __name__ == '__main__':
    raise SystemExit(main())
