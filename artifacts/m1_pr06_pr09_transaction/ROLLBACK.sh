#!/usr/bin/env bash
set -euo pipefail
TARGET="$1"
BASELINE="$2"
cp -- "$BASELINE" "$TARGET"
printf 'ROLLBACK_RESTORED=%s\n' "$TARGET"
