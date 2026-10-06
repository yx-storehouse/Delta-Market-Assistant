"""Capture a waited native process rather than relying on PowerShell GUI launch semantics."""
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
args = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', '.\\build.ps1', '-Test', '-Package']
result = subprocess.run(args, cwd=ROOT, capture_output=True)
stdout = result.stdout.decode('utf-8', 'replace')
stderr = result.stderr.decode('utf-8', 'replace')
(HERE / 'logs/final_build.stdout').write_bytes(result.stdout)
(HERE / 'logs/final_build.stderr').write_bytes(result.stderr)
(HERE / 'build_result.json').write_text(json.dumps(dict(command=subprocess.list2cmdline(args),
    cwd=str(ROOT), exit_status=result.returncode, stdout=stdout, stderr=stderr), ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(stdout[-10000:])
print('FINAL_BUILD_EXIT=' + str(result.returncode))
raise SystemExit(result.returncode)
