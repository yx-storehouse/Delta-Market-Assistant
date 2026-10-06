"""Validate planning artifacts only. Does not import application code or run samples."""
from pathlib import Path
import json
import re
import hashlib
from datetime import datetime, timezone

HERE=Path(__file__).resolve().parent
DOC=HERE.parents[1]

def load(p):
    return json.loads(p.read_text(encoding='utf-8'))

def need(condition, message):
    if not condition:
        raise AssertionError(message)

def main():
    backlog=load(HERE/'backlog.json')
    tests=load(HERE/'test_matrix.json')
    catalog=load(DOC/'business_catalog.json')
    old_tests=load(DOC/'acceptance_cases.json')
    evidence=load(DOC/'evidence/index.json')
    references=load(DOC/'references.json')
    b={x['id'] for x in catalog}; t={x['id'] for x in old_tests}
    e={x['id'] for x in evidence}; r={x['id'] for x in references}
    tickets=backlog['tickets']; ids={x['id'] for x in tickets}
    need(len(ids)==len(tickets)==12,'12 unique tickets required')
    need(backlog['status']=='specification_only' and not backlog['application_implemented'],'Unexpected implemented claim')
    work=set()
    for x in tickets:
        need(x['status']=='planned_not_implemented', 'Ticket incorrectly marked implemented: '+x['id'])
        need(set(x['depends_on'])<=ids and x['id'] not in x['depends_on'],'Invalid dependency '+x['id'])
        need(set(x['business_ids'])<=b and set(x['acceptance_ids'])<=t,'Unknown B/T ref '+x['id'])
        need(set(x['evidence_ids'])<=e and set(x['reference_ids'])<=r,'Unknown E/R ref '+x['id'])
        for key in ['inputs','outputs','suggested_files','acceptance','failure_rollback','non_goals']:
            need(isinstance(x[key],list) and x[key], 'Missing '+key+' '+x['id'])
        work.update(x['work_items'])
    need(work=={'WI01','WI02','WI03','WI04','WI05'},'WI01-WI05 coverage')
    by_id={x['id']:x for x in tickets}
    done=set(); active=set()
    def visit(k):
        need(k not in active,'Dependency cycle '+k)
        if k in done:return
        active.add(k)
        for dep in by_id[k]['depends_on']:visit(dep)
        active.remove(k);done.add(k)
    for k in ids:visit(k)
    need(set(backlog['decision_impact'])=={f'D{i:02}' for i in range(1,14)},'D01-D13 coverage')
    for d,v in backlog['decision_impact'].items():
        need(set(v['tickets'])<=ids and bool(v['blocking']),'Invalid decision mapping '+d)
    cases=tests['cases']; xids={x['id'] for x in cases}
    need(len(xids)==len(cases)==30,'30 unique detailed tests required')
    need(tests['application_tests_run']==0,'Document may not claim application execution')
    for x in cases:
        need(x['status']=='planned_not_run','Test falsely marked run '+x['id'])
        need(set(x['business_acceptance_refs'])<=b|t,'Unknown detailed test refs '+x['id'])
        need(set(x['tickets'])<=ids,'Unknown detailed ticket refs '+x['id'])
        for key in ['given','when','then','fixture','command_plan','not_proved']:
            need(isinstance(x[key],str) and bool(x[key].strip()),'Missing GWT/command '+x['id']+' '+key)
    layers={x['layer'] for x in cases}
    need({'document_schema','pure_rule','replay','image_quality','capture_io','ui_offscreen','release_package'}<=layers,'Missing required testing layer')
    link_count=0
    for p in HERE.glob('*.md'):
        txt=p.read_text(encoding='utf-8')
        for dest in re.findall(r'!?\[[^\]\n]*\]\(([^)\n]+)\)',txt):
            if dest.startswith(('https:','http:','#')):continue
            need((p.parent/dest.split('#')[0]).is_file(),'Missing MD link '+p.name+': '+dest)
            link_count+=1
    files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(HERE.iterdir())
           if p.is_file() and p.name not in {'verification.json','VERIFICATION.txt'}}
    need(all((HERE/n).read_bytes() for n in files),'Empty claimed artifact')
    result=dict(result='PASS',checked_at=datetime.now(timezone.utc).isoformat(),
        ticket_count=len(ids), detailed_test_spec_count=len(xids), work_items=sorted(work),
        decision_mapping_count=13, markdown_links=link_count, application_tests_run=0,
        sample_executions=0, files_sha256=files,
        note='Only planning structure, references, dependency graph and document links; no business engine/vision/IO verification.')
    (HERE/'verification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    output=f'READINESS_DOCS=PASS TICKETS={len(ids)} DETAILED_TEST_SPECS={len(xids)} DECISIONS=13 LINKS={link_count} APPLICATION_TESTS_RUN=0 SAMPLE_EXECUTIONS=0'
    command=r'python -I -X utf8 docs\business_rebuild\implementation\readiness\validate_readiness.py'
    (HERE/'VERIFICATION.txt').write_text('Readiness documentation verification\nCOMMAND: '+command+'\nINPUT: backlog.json, test_matrix.json, 44 business refs, 88 existing acceptance refs, E/R indices, local Markdown links\nSTDOUT: '+output+'\nEXIT_STATUS: 0\nAPPLICATION_TESTS_RUN: 0\nSAMPLE_EXECUTIONS: 0\nRESULT: planning artifacts checked; no application source or release change claimed.\n',encoding='utf-8')
    need(load(HERE/'verification.json')['result']=='PASS','Reopen result failed')
    need((HERE/'VERIFICATION.txt').read_text(encoding='utf-8').endswith('claimed.\n'),'Reopen text failed')
    print(output)
    return 0

if __name__=='__main__':
    raise SystemExit(main())
