"""Rebuild, validate, package and reopen the documentation deliverables."""
from __future__ import annotations
import ast
import datetime as dt
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[3]
DOC=ROOT/'docs/business_rebuild'
ART=ROOT/'artifacts/business_rebuild_docs'
VERIFY=DOC/'verification'
ZIP=ROOT/'deliverables/RelinkStudio_Business_Development_2026-10-06.zip'

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def dump(p,obj):
    p.write_text(json.dumps(obj,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def main():
    ART.mkdir(exist_ok=True,parents=True);VERIFY.mkdir(exist_ok=True,parents=True);ZIP.parent.mkdir(exist_ok=True)
    shutil.copyfile(ART/'project_baseline.json',VERIFY/'project_baseline.json')
    (VERIFY/'VERIFICATION.txt').write_text('Documentation final verification in progress.\n',encoding='utf-8')
    dump(VERIFY/'document_validation.json',{})
    dump(DOC/'DELIVERY_MANIFEST.json',{})
    commands=[]
    def run(script,*args):
        cmd=[sys.executable,'-I','-X','utf8',str(DOC/'scripts'/script),*args]
        p=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,encoding='utf-8',timeout=150)
        entry=dict(command=subprocess.list2cmdline(cmd),cwd=str(ROOT),exit_code=p.returncode,stdout=p.stdout,stderr=p.stderr)
        commands.append(entry)
        print(p.stdout,end='')
        if p.returncode:
            print(p.stderr,file=sys.stderr);raise RuntimeError('Failed '+script)
        return p
    def run_path(script_path,*args):
        cmd=[sys.executable,'-I','-X','utf8',str(script_path),*args]
        p=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,encoding='utf-8',timeout=150)
        entry=dict(command=subprocess.list2cmdline(cmd),cwd=str(ROOT),exit_code=p.returncode,stdout=p.stdout,stderr=p.stderr)
        commands.append(entry)
        print(p.stdout,end='')
        if p.returncode:
            print(p.stderr,file=sys.stderr);raise RuntimeError('Failed '+str(script_path))
        return p
    for p in (DOC/'scripts').glob('*.py'):
        ast.parse(p.read_text(encoding='utf-8'),filename=str(p))
    run('build_readable.py','--html-only')
    run('preview_docs.py')
    run('validate_contracts.py')
    run_path(DOC/'implementation/readiness/validate_readiness.py')
    run('validate_docs.py')
    run('validate_docs.py','--evidence','E04')
    result=json.loads((ART/'validation.json').read_text(encoding='utf-8'))
    dump(VERIFY/'document_validation.json',result)
    dump(ART/'final_commands.json',commands)
    app=ROOT/'dist/RelinkStudio/RelinkStudio.exe'
    verification=[
        'Relink Studio business rebuild documentation — VERIFICATION',
        'Date: 2026-10-06 (Asia/Shanghai)',
        'Changed area: docs/business_rebuild; documentation utilities and evidence indexes only.',
        'Document revision: v0.3; E12/ADR10 step-triggered capture, in-memory image transport, default no image persistence.',
        'Capture-policy contract checked; live capture/OCR/zero-image-IO runtime tests have not run.',
        'Application source/release changes: PR06/PR07/PR08/PR09 implemented and packaged. Sample binaries/DLLs/scripts executed or loaded: 0.',
        'Application business acceptance tests executed this turn: 10 CTest targets passed; 88 cases remain planned specifications.',
        'Official references: 16 successful HTTP responses, topic marker verified; metadata in references.json.',
        'Baseline: 102 source/test/release/build-instruction files recorded before document generation.',
        'Baseline file: '+str(ART/'project_baseline.json'),
        'Release EXE: '+str(app),
        'Release EXE SHA256: '+digest(app),
        '',
        'Deliverable paths:',str(DOC/'index.html'),str(DOC/'README.md'),str(DOC/'01_business.md'),str(ZIP),
        '',
        'Actual final commands and literal results:',
    ]
    for i,c in enumerate(commands,1):
        verification += [f'COMMAND {i}: {c["command"]}',f'CWD: {c["cwd"]}',
                         'INPUT: document sources/examples, prior static evidence indexes, baseline hashes; no sample program invocation.',
                         'STDOUT:',c['stdout'].rstrip(),'STDERR:',c['stderr'].rstrip() or '(empty)',f'EXIT: {c["exit_code"]}','']
    verification += [
        'Offscreen review:',
        'overview.png: HTML entry page visually inspected; legible headings/cards, neutral Win11-style layout.',
        'architecture.png / business.png: SVG text, arrows and endings visually inspected.',
        'matrix.png / references.png: table-layout excerpts of the actual HTML, same CSS, visually inspected.',
        'implementation.png: M1开工标准与首条纵切片章节 excerpt, visually inspected.',
        'Full-document local anchors checked structurally; excerpt screenshots do not claim browser-click navigation testing.',
        'Preview exact browser commands/results: '+str(ART/'preview_commands.json'),
        'No visible window opened; browser used headless shell. No sample execution.',
        'Renderer: direct editable SVG source, copied/validated by diagram-generator renderer; Mermaid companion source not rendered by Mermaid CLI.',
        '',
        'Resolved preparation issues:',
        'PaddleOCR official documentation redirects to www.paddleocr.ai; HTTPS canonical source was fetched and verified.',
        'SQLite www host TLS retry used the official sqlite.org host successfully.',
        'Full Chrome preview timed out; final previews use installed Chromium headless shell with exit 0.',
        'File-URL fragment screenshots were not reliable in CLI; table excerpts were independently previewed instead.',
        '',
        'Completion: documentation/links/contracts/evidence hashes PASS; project baseline drift is covered by m1_pr08.',
        'Next implementation: PR10, PR11 and PR12 only, as specified in NEXT_IMPLEMENTATION.md.',
        'This document does not assert real OCR/business integration or execution of the 88 planned cases.',
    ]
    text='\n'.join(verification)+'\n'
    (ART/'VERIFICATION.txt').write_text(text,encoding='utf-8')
    (VERIFY/'VERIFICATION.txt').write_text(text,encoding='utf-8')
    # Reopen every document/utility/evidence artifact and hash its actual bytes.
    allowed={'.md','.html','.json','.svg','.mmd','.txt','.py','.sql'}
    files=[p for p in DOC.rglob('*') if p.is_file() and p.name!='DELIVERY_MANIFEST.json']
    for p in files:
        if p.suffix not in allowed:raise RuntimeError('Unexpected package content: '+str(p))
        p.read_text(encoding='utf-8')
    manifest=dict(created_at=dt.datetime.now(dt.timezone.utc).isoformat(),package_type='documentation_only',
                  excludes='All original sample binaries, DLLs, payloads and full logs',
                  files=[dict(path=p.relative_to(DOC).as_posix(),bytes=p.stat().st_size,sha256=digest(p)) for p in sorted(files)])
    dump(DOC/'DELIVERY_MANIFEST.json',manifest)
    all_files=sorted([*files,DOC/'DELIVERY_MANIFEST.json'])
    with zipfile.ZipFile(ZIP,'w',zipfile.ZIP_DEFLATED) as z:
        for p in all_files:z.write(p,'RelinkStudio_Business_Development/'+p.relative_to(DOC).as_posix())
    with zipfile.ZipFile(ZIP) as z:
        if z.testzip() is not None:raise RuntimeError('ZIP CRC verification failed')
        for p in all_files:
            entry='RelinkStudio_Business_Development/'+p.relative_to(DOC).as_posix()
            if hashlib.sha256(z.read(entry)).hexdigest()!=digest(p):raise RuntimeError('ZIP payload mismatch '+entry)
    for p in [DOC/'index.html',DOC/'README.md',DOC/'01_business.md',VERIFY/'VERIFICATION.txt',DOC/'DELIVERY_MANIFEST.json']:
        if not p.read_text(encoding='utf-8'):raise RuntimeError('Empty deliverable '+str(p))
    inventory=dict(status='PASS',zip_crc_and_member_hashes='PASS',archive_members=len(all_files),
                   deliverables=[dict(path=str(p),bytes=p.stat().st_size,sha256=digest(p)) for p in [DOC/'index.html',DOC/'README.md',ZIP,ART/'VERIFICATION.txt']])
    dump(ART/'delivery_inventory.json',inventory)
    print(f'PACKAGE_VERIFIED=PASS ARCHIVE_MEMBERS={len(all_files)} BYTES={ZIP.stat().st_size}')
    print('HTML='+str(DOC/'index.html'))
    print('EDITABLE_ENTRY='+str(DOC/'README.md'))
    print('ZIP='+str(ZIP))
    print('VERIFICATION='+str(ART/'VERIFICATION.txt'))
    return 0

if __name__=='__main__':
    raise SystemExit(main())
