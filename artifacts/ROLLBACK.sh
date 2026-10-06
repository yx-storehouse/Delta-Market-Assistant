#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
[[ $# -eq 1 ]] || { printf 'Usage: %s TARGET_JSON\n' "$0" >&2; exit 2; }
exec python "$ROOT/rollback_config.py" "$1"
