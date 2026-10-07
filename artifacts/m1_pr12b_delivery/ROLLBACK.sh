#!/usr/bin/env bash
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -ne 2 ]]; then echo 'Usage: ROLLBACK.sh <python-exe> <isolated-target>' >&2; exit 2; fi
"$1" "$(cygpath -w "$here/restore_config.py")" "$2"
