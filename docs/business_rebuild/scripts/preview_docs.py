"""Offscreen HTML/SVG preview using an existing trusted browser, never the sample."""
from pathlib import Path
import json
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[3]
DOC=ROOT/'docs/business_rebuild'
ART=ROOT/'artifacts/business_rebuild_docs'
CHROME=Path.home()/'AppData/Local/ms-playwright/chromium_headless_shell-1243/chrome-headless-shell-win64/chrome-headless-shell.exe'

def main():
    if not CHROME.is_file():
        raise FileNotFoundError('Previously inspected browser not found: '+str(CHROME))
    ART.mkdir(parents=True,exist_ok=True)
    jobs=[('overview',DOC/'index.html',1500,1100,''),('architecture',DOC/'diagrams/architecture.svg',1200,1100,''),
          ('business',DOC/'diagrams/business.svg',1200,1100,''),('matrix',DOC/'index.html',1500,1100,'doc-01_business'),
          ('references',DOC/'index.html',1500,1100,'doc-06_evidence_references'),('implementation',DOC/'index.html',1500,1100,'doc-08_implementation_ready')]
    outputs=[]
    for name,src,w,h,anchor in jobs:
        original=src
        if anchor:
            # CLI capture does not reliably repaint after file-URL fragment scrolling.
            # Inspect the actual chapter markup with the identical CSS, at the top of an audit-only copy.
            page=src.read_text(encoding='utf-8')
            page=re.sub(r'<header class="hero">.*?</header>','',page,flags=re.S)
            page=re.sub(r'<section id="([^"]+)">.*?</section>',lambda m:m.group(0) if m.group(1)==anchor else '',page,flags=re.S)
            src=ART/(name+'-layout-preview.html')
            src.write_text(page,encoding='utf-8')
        out=ART/(name+'.png')
        command=[str(CHROME),'--headless','--disable-gpu','--disable-background-networking','--disable-sync','--no-first-run',
                 '--hide-scrollbars','--virtual-time-budget=3000','--run-all-compositor-stages-before-draw',f'--user-data-dir={ART/("shell-profile-"+name)}',
                 f'--screenshot={out}',f'--window-size={w},{h}','--timeout=6000',src.as_uri()]
        p=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=40,creationflags=subprocess.CREATE_NO_WINDOW)
        outputs.append(dict(name=name,command=command,input=src.as_posix(),source_document=original.as_posix(),
                            layout_excerpt=bool(anchor),output=str(out),exit_code=p.returncode,stdout=p.stdout,stderr=p.stderr))
        if p.returncode or not out.is_file() or out.stat().st_size<15000:
            raise RuntimeError('Preview failed: '+name+' '+p.stderr)
        print(f'OFFSCREEN {name}: exit={p.returncode}, image_bytes={out.stat().st_size}')
    (ART/'preview_commands.json').write_text(json.dumps(outputs,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
