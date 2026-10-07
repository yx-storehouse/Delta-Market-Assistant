"""Validate documentation and example contracts only; never launch target binaries."""
from __future__ import annotations
import argparse
from decimal import Decimal
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[3]
DOC=ROOT/'docs/business_rebuild'
ART=ROOT/'artifacts/business_rebuild_docs'

def read(name):
    return json.loads((DOC/name).read_text(encoding='utf-8'))

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):
            h.update(b)
    return h.hexdigest()

def development_baselines():
    # Retain all historical M1 snapshots. M2 is a new explicitly scoped
    # snapshot, not a silent replacement of the original evidence hashes.
    return sorted([*ART.glob('m1_pr*_baseline.json'), *ART.glob('m2_*_baseline.json')])

def load_baseline(path):
    """Load either the original flat baseline or a scoped M1 baseline.

    The original project_baseline.json is intentionally kept as a flat mapping for
    compatibility with the documentation package.  New development baselines carry
    metadata and put hashes under ``files``.  Returning one normalized mapping keeps
    the validation logic explicit and makes intentional source drift visible instead
    of silently replacing the original baseline.
    """
    payload=json.loads(path.read_text(encoding='utf-8'))
    if isinstance(payload,dict) and isinstance(payload.get('files'),dict):
        return payload, payload['files']
    if isinstance(payload,dict) and all(isinstance(v,str) for v in payload.values()):
        return {'baseline_id':path.stem,'files':payload}, payload
    raise ValueError(f'Invalid baseline format: {path}')

def baseline_diff(entries):
    """Return deterministic path/hash differences for a normalized baseline."""
    differences=[]
    for name,expected in sorted(entries.items()):
        path=ROOT/name
        if not path.is_file():
            differences.append({'path':name,'expected':expected,'actual':None,'state':'missing'})
            continue
        actual=sha(path)
        if actual != expected:
            differences.append({'path':name,'expected':expected,'actual':actual,'state':'modified'})
    return differences

def need(condition,message):
    if not condition:
        raise AssertionError(message)

class HtmlAudit(HTMLParser):
    def __init__(self):
        super().__init__();self.ids=[];self.hrefs=[];self.resources=[];self.scripts=0;self.sections=0;self.svgs=0
    def handle_starttag(self,tag,attrs):
        a=dict(attrs)
        if 'id' in a:self.ids.append(a['id'])
        if tag=='a' and 'href' in a:self.hrefs.append(a['href'])
        if tag in ['img','script','link','iframe']:
            self.resources.append(a.get('src',a.get('href','')))
        if tag=='script':self.scripts+=1
        if tag=='section':self.sections+=1
        if tag=='svg':self.svgs+=1

def evidence_check(eid=None):
    groups=read('evidence/index.json')
    if eid:
        groups=[x for x in groups if x['id']==eid]
        need(bool(groups),'Unknown evidence ID '+eid)
    total=0
    evidence_diffs=[]
    m1_entries={}
    m1_candidates=development_baselines()
    if m1_candidates:
        _meta,m1_entries=load_baseline(m1_candidates[-1])
    for e in groups:
        for f in e['files']:
            path=ROOT/f['path']
            need(path.is_file(),'Missing evidence '+f['path'])
            actual=sha(path)
            if actual != f['sha256']:
                # Evidence records describe the original observation snapshot. A
                # deliberate implementation change is accepted only when the exact
                # current hash is captured by the scoped M1 baseline; keep the
                # difference visible in command output rather than rewriting evidence.
                covered=(f['path'] in m1_entries and m1_entries[f['path']]==actual)
                need(covered,'Evidence hash changed '+f['path'])
                evidence_diffs.append({'path':f['path'],'expected':f['sha256'],'actual':actual,'covered_by_m1':True})
            total+=1
    for difference in evidence_diffs:
        print('BASELINE_DIFF evidence path={path} expected={expected} actual={actual} covered_by_m1={covered_by_m1}'.format(**difference))
    return total

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence')
    args=parser.parse_args()
    if args.evidence:
        n=evidence_check(args.evidence)
        print(f'EVIDENCE {args.evidence}: {n} source files hash verified; sample execution=0')
        return 0
    catalog=read('business_catalog.json');cases=read('acceptance_cases.json');refs=read('references.json');evidence=read('evidence/index.json')
    bids={x['id'] for x in catalog};tids={x['id'] for x in cases};rids={x['id'] for x in refs};eids={x['id'] for x in evidence}
    need(len(catalog)==len(bids)==44,'Business count or duplicate ID')
    need(len(cases)==len(tids)==88,'Acceptance specification count or duplicate ID')
    need(len(refs)==len(rids)==16,'Reference count or duplicate ID')
    need(len(evidence)==len(eids)==12,'Evidence count or duplicate ID')
    for b in catalog:
        need(set(b['evidence_ids'])<=eids,'Unknown evidence '+b['id'])
        need(set(b['reference_ids'])<=rids,'Unknown official reference '+b['id'])
        need(len(b['acceptance_ids'])==2 and set(b['acceptance_ids'])<=tids,'Missing cases '+b['id'])
        need(b['implementation_status']=='specified_not_implemented','Unexpected implemented claim '+b['id'])
        for tid in b['acceptance_ids']:
            need(next(c for c in cases if c['id']==tid)['business_id']==b['id'],'Case reverse mapping '+tid)
    for c in cases:
        need(c['status']=='planned_not_run','Acceptance mislabeled executed '+c['id'])
        need(c['business_id'] in bids,'Unknown business '+c['id'])
    allowed={'docs.opencv.org','learn.microsoft.com','paddlepaddle.github.io','www.paddleocr.ai','doc.qt.io','sqlite.org','www.sqlite.org','www.rfc-editor.org'}
    for r in refs:
        need(r['status']=='verified' and r['http_status']==200 and r['marker_found'],'Reference not verified '+r['id'])
        need(urlparse(r['final_url']).hostname in allowed,'Non-primary reference host '+r['id'])
        need(len(r['sha256'])==64 and r['bytes']>1000,'Missing response identity '+r['id'])
    parsed_json=0
    for p in DOC.rglob('*.json'):
        json.loads(p.read_text(encoding='utf-8'));parsed_json+=1
    s=read('examples/schema-v2.sample.json');v=read('examples/vision-result.sample.json')
    t=s['tasks'][0]
    need(s['example_only'] and s['schema_version']==2 and s['mode']=='replay','Wrong schema example mode')
    need(not t['enabled'] and t['review_required'],'Legacy preview must be disabled')
    need(t['limits']['quantity_target'] is None,'Unknown legacy columns mapped to quantity')
    need(not s['runtime']['actions_enabled'] and not s['runtime']['clock']['set_system_time'],'Example enables actions or system time writes')
    legacy=read('evidence/task_schema.json')['rows']
    need(len(legacy)==15 and all(len(row)==13 for row in legacy),'Legacy task fixture lost columns')
    need(t['legacy_raw']['columns']==legacy[0],'Schema example lost original row')
    need(t['legacy_raw']['columns'][11:]==['不限','不限'],'Unknown columns changed')
    need(v['example_only'] and v['image_sha256'] is None,'Fabricated real-image metadata')
    need(v['provider']['name']=='mock' and v['source_kind']=='contract_example_without_pixels','Example mislabeled OCR measurement')
    need(v['image_saved'] is False and bool(v['step_id']),'Vision example must carry step and no image-persistence claim')
    policy=read('examples/observation-policy.sample.json')
    need(policy['example_only'] and policy['source_evidence']=='E12','Observation policy provenance')
    need(policy['trigger']=='business_step_demand','Capture must be step-triggered')
    need(not any(policy[k] for k in ['idle_capture_enabled','paused_capture_enabled','long_wait_capture_enabled']),'Unexpected continuous capture')
    need(policy['require_step_id'] and policy['require_fresh_frame_after_step_barrier'],'Missing step freshness')
    need(policy['bounded_watch_requires_deadline'] and policy['bounded_watch_requires_frame_budget'],'Unbounded observation')
    need(policy['frame_transport']=='shared_memory_descriptor','Live capture uses file transport')
    need(policy['memory_pool']=={'max_in_flight':1,'max_pending':1,'replace_stale_pending':True},'Unbounded image buffer example')
    persistence=policy['image_persistence']
    need(not any(persistence[k] for k in ['normal_run','on_success','on_error','temporary_image_files','disk_file_ipc','debug_export_enabled']),'Automatic image persistence enabled')
    need(persistence['manual_export_requires_explicit_request'] and persistence['debug_session_requires_limits'],'Unbounded export')
    need(not policy['logging']['per_frame_image_payload'] and not policy['logging']['per_frame_ocr_dump'],'Per-frame logging enabled')
    need(policy['replay']['existing_fixture_reads_allowed'] and not policy['replay']['write_intermediate_images'],'Replay persistence mismatch')
    need((DOC/'examples'/s['runtime']['observation_policy_example']).is_file(),'Missing policy sample reference')
    vision_spec=(DOC/'02_vision_architecture.md').read_text(encoding='utf-8')
    need('第一版读本次会话帧目录下文件' not in vision_spec,'Obsolete disk IPC specification')
    for x in v['texts']:
        need(0<=x['score']<=1 and len(x['polygon'])==4,'Invalid OCR contract')
        for px,py in x['polygon']:
            need(0<=px<=v['frame_size']['width'] and 0<=py<=v['frame_size']['height'],'Out-of-frame contract point')
    # Mathematical/example invariants only, not an implementation of the future rule engine.
    need(Decimal('1.187788')<=Decimal('1.249'),'Wear example arithmetic')
    need(Decimal('230')<=Decimal('340')<=Decimal('450'),'Price example arithmetic')
    need(Decimal('399')/(Decimal(10)**3)==Decimal('0.399'),'Decimal encoding example')
    need(Decimal('1187788')/(Decimal(10)**6)==Decimal('1.187788'),'Decimal precision example')
    links=0
    for p in DOC.rglob('*.md'):
        text=p.read_text(encoding='utf-8')
        for dest in re.findall(r'!?\[[^\]\n]*\]\(([^)\n]+)\)',text):
            if dest.startswith(('http:','https:','#','mailto:')):continue
            path=unquote(dest.split('#',1)[0])
            if not path:continue
            need((p.parent/path).is_file(),f'Broken local link {p.name}: {dest}')
            links+=1
        if p.name!='static-analysis-report.md':
            need(set(re.findall(r'\bE\d{2}\b',text))<=eids,'Unknown E reference '+p.name)
            need(set(re.findall(r'\bR\d{2}\b',text))<=rids,'Unknown R reference '+p.name)
    for name in ['architecture','business']:
        g=read('diagrams/'+name+'.graph.json');nodes={x[0] for x in g['nodes']}
        need(len(nodes)==len(g['nodes']),'Duplicate diagram node')
        for a,b,_ in g['edges']:need(a in nodes and b in nodes,'Unknown diagram node')
        ET.parse(DOC/'diagrams'/f'{name}.svg')
        mmd=(DOC/'diagrams'/f'{name}.mmd').read_text(encoding='utf-8')
        need(mmd.startswith('flowchart TD') and all(f'{n}[' in mmd for n in nodes),'Mermaid source node mismatch')
    audit=HtmlAudit();audit.feed((DOC/'index.html').read_text(encoding='utf-8'))
    need(audit.sections==11 and audit.svgs==2,'HTML sections/diagrams missing')
    need(audit.scripts==0 and not audit.resources,'HTML not self-contained static document')
    need(len(audit.ids)==len(set(audit.ids)),'Duplicate HTML IDs')
    for href in audit.hrefs:
        if href.startswith('#'):need(href[1:] in audit.ids,'Broken HTML anchor '+href)
        elif not href.startswith(('http:','https:','mailto:')):need((DOC/unquote(href)).is_file(),'Broken HTML local link '+href)
    evidence_files=evidence_check()

    # Keep the original application/release baseline immutable.  M1 development is
    # allowed to add the explicitly scoped implementation files and the build target;
    # those intentional changes are captured by the second baseline rather than being
    # erased from project_baseline.json.  A project drift not covered by the M1 scope
    # remains a hard failure.
    project_meta, project_baseline=load_baseline(ART/'project_baseline.json')
    # Prefer the newest explicit development baseline while retaining every
    # previous baseline as an immutable audit record.
    m1_candidates=development_baselines()
    need(bool(m1_candidates),'Missing scoped development baseline in '+str(ART))
    m1_path=m1_candidates[-1]
    m1_meta, m1_baseline=load_baseline(m1_path)
    project_diffs=baseline_diff(project_baseline)
    m1_diffs=baseline_diff(m1_baseline)
    expected_project_drift=set(m1_meta.get('expected_project_baseline_drift',[]))
    uncovered_project_diffs=[]
    for difference in project_diffs:
        path=difference['path']
        covered=(path in m1_baseline and difference['actual']==m1_baseline[path])
        difference['covered_by_m1']=covered
        difference['expected_m1_drift']=path in expected_project_drift
        if not covered:
            uncovered_project_diffs.append(difference)
    need(not uncovered_project_diffs,
         'Project baseline drift outside M1 scope: '+', '.join(x['path'] for x in uncovered_project_diffs))
    need(not m1_diffs,
         f'{m1_meta.get("baseline_id", m1_path.stem)} baseline changed: '+', '.join(x['path'] for x in m1_diffs))

    for difference in project_diffs:
        print('BASELINE_DIFF project path={path} state={state} expected={expected} actual={actual} covered_by_m1={covered_by_m1}'.format(**difference))
    if not project_diffs:
        print('BASELINE_DIFF project=NONE')
    if not m1_diffs:
        print(f'BASELINE_DIFF {m1_meta.get("baseline_id", m1_path.stem)}=NONE')
    m1_id=m1_meta.get('baseline_id',m1_path.stem)
    result=dict(result='PASS',business_items=len(bids),acceptance_specifications=len(tids),business_tests_executed=0,
                verified_primary_references=len(rids),evidence_groups=len(eids),evidence_files_verified=evidence_files,
                json_files_parsed=parsed_json,relative_markdown_links_checked=links,html_sections=audit.sections,svg_diagrams=audit.svgs,
                application_baseline_files=len(project_baseline),
                application_baseline_files_unchanged=len(project_baseline)-len(project_diffs),
                application_baseline_differences=project_diffs,
                m1_baseline_id=m1_id,
                m1_baseline_files=len(m1_baseline),
                m1_baseline_files_unchanged=len(m1_baseline)-len(m1_diffs),
                m1_baseline_differences=m1_diffs,
                sample_execution=0,step_capture_contract='PASS',
                capture_runtime_tests_executed=0,
                note='Checks documentation and example contracts only; not production business/vision acceptance.')
    ART.mkdir(parents=True,exist_ok=True)
    (ART/'validation.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('DOCUMENTATION_CHECKS=PASS')
    print(f'BUSINESS_ITEMS={len(bids)} ACCEPTANCE_SPECIFICATIONS={len(tids)} BUSINESS_TESTS_EXECUTED=0')
    print(f'OFFICIAL_REFERENCES_VERIFIED={len(rids)} EVIDENCE_GROUPS={len(eids)} EVIDENCE_FILES_HASH_VERIFIED={evidence_files}')
    print(f'JSON_FILES_PARSED={parsed_json} INTERNAL_MD_LINKS={links} HTML_SECTIONS={audit.sections} SVG_DIAGRAMS={audit.svgs}')
    print(f'APPLICATION_BASELINE_FILES={len(project_baseline)} APPLICATION_BASELINE_FILES_UNCHANGED={len(project_baseline)-len(project_diffs)} APPLICATION_BASELINE_DIFFS={len(project_diffs)}')
    print(f'M1_BASELINE_ID={m1_id} M1_BASELINE_FILES={len(m1_baseline)} M1_BASELINE_FILES_UNCHANGED={len(m1_baseline)-len(m1_diffs)} M1_BASELINE_DIFFS={len(m1_diffs)} SAMPLE_EXECUTIONS=0')
    print('STEP_CAPTURE_CONTRACT=PASS LIVE_CAPTURE_TESTS_EXECUTED=0')
    print(f'DEVELOPMENT_BASELINE_ID={m1_id}; legacy_m1_output_fields_retained=true')
    return 0

if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (AssertionError,OSError,ValueError) as e:
        print('DOCUMENTATION_CHECKS=FAIL: '+str(e),file=sys.stderr)
        raise SystemExit(1)
