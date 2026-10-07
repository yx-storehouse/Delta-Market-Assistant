#!/usr/bin/env bash
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -ne 1 ]]; then echo "Usage: ROLLBACK.sh <absolute-target-directory>" >&2; exit 2; fi
powershell="$(cygpath -u "${SYSTEMROOT:-C:\Windows}\System32\WindowsPowerShell\v1.0\powershell.exe")"
"$powershell" -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/restore_release.ps1")" -Target "$1"
