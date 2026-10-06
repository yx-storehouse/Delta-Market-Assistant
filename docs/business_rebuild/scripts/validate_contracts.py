"""Offline structural validation of design schemas/fixtures, not the future business engine."""
from __future__ import annotations
import json
from pathlib import Path
import sys
from urllib.parse import urldefrag

ROOT=Path(__file__).resolve().parents[3]
DOC=ROOT/'docs/business_rebuild'
ART=ROOT/'artifacts/business_rebuild_docs'
sys.path.insert(0,str(ROOT/'.tools/doc-render'))
from jsonschema import Draft202012Validator, FormatChecker
from referencing import Registry, Resource
from referencing.exceptions import NoSuchResource

def read(p):
    return json.loads(p.read_text(encoding='utf-8'))

def no_network(uri):
    raise NoSuchResource(ref=uri)

def refs(value):
    if isinstance(value,dict):
        for k,v in value.items():
            if k=='$ref':yield v
            else:yield from refs(v)
    elif isinstance(value,list):
        for v in value:yield from refs(v)

def pointer(value,ptr):
    if not ptr:return value
    if not ptr.startswith('/'):raise ValueError('Invalid JSON pointer '+ptr)
    for token in ptr[1:].split('/'):
        token=token.replace('~1','/').replace('~0','~')
        value=value[int(token)] if isinstance(value,list) else value[token]
    return value

def main():
    schemas={};paths={}
    for p in sorted((DOC/'implementation').rglob('*.schema.json')):
        data=read(p)
        Draft202012Validator.check_schema(data)
        sid=data['$id']
        if sid in schemas:raise ValueError('Duplicate schema ID '+sid)
        if not sid.startswith('https://schemas.relink.invalid/business/v1/'):
            raise ValueError('Unexpected schema ID '+sid)
        schemas[sid]=data;paths[sid]=p
    if len(schemas)<3:raise ValueError('Missing implementation schemas')
    registry=Registry(retrieve=no_network).with_resources([(sid,Resource.from_contents(s)) for sid,s in schemas.items()])
    references_checked=0
    for sid,s in schemas.items():
        resolver=registry.resolver(base_uri=sid)
        for ref in refs(s):
            resolver.lookup(ref)
            references_checked+=1
    manifests=sorted((DOC/'implementation').rglob('validation_manifest.json'))
    if len(manifests)<2:raise ValueError('Domain/runtime validation manifests are both required')
    ids=set();records=[]
    for m in manifests:
        for case in read(m)['cases']:
            cid=m.parent.name+':'+case['id']
            if cid in ids:raise ValueError('Duplicate fixture '+cid)
            ids.add(cid)
            if type(case['valid']) is not bool:raise ValueError('Expected validity must be bool '+cid)
            filepath,fragment=urldefrag(case['instance'])
            p=(m.parent/filepath).resolve()
            if not p.is_relative_to(m.parent.resolve()):raise ValueError('Fixture path escapes owner directory '+cid)
            instance=pointer(read(p),case.get('instance_pointer',fragment))
            validator=Draft202012Validator({'$ref':case['schema_id']},registry=registry,format_checker=FormatChecker())
            # Resolution errors are fatal; an unresolved schema never counts as an expected rejection.
            errors=list(validator.iter_errors(instance))
            actual=not errors
            rec=dict(id=cid,schema_id=case['schema_id'],instance=str(p.relative_to(DOC)),expected_valid=case['valid'],
                     actual_valid=actual,result='PASS' if actual==case['valid'] else 'FAIL',
                     diagnostics=[dict(instance_path='/'+('/'.join(map(str,e.absolute_path))),message=e.message) for e in errors[:4]])
            records.append(rec)
    failures=[x for x in records if x['result']=='FAIL']
    result=dict(scope='JSON Schema syntax, offline references, and documented shape acceptance/rejection only',
                result='FAIL' if failures else 'PASS',schemas_checked=len(schemas),schema_refs_resolved=references_checked,
                schema_fixture_cases=len(records),expected_valid_cases=sum(x['expected_valid'] for x in records),
                expected_invalid_cases=sum(not x['expected_valid'] for x in records),failures=len(failures),
                business_engine_tests_executed=0,live_capture_tests_executed=0,network_schema_fetches=0,cases=records)
    ART.mkdir(parents=True,exist_ok=True)
    (ART/'contract_validation.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'CONTRACT_STRUCTURAL_CHECKS={result["result"]} SCHEMAS={len(schemas)} RESOLVED_REFS={references_checked} FIXTURES={len(records)}')
    print(f'EXPECTED_ACCEPT={result["expected_valid_cases"]} EXPECTED_REJECT={result["expected_invalid_cases"]} FAILURES={len(failures)} NETWORK_SCHEMA_FETCHES=0')
    print('BUSINESS_ENGINE_TESTS_EXECUTED=0 LIVE_CAPTURE_TESTS_EXECUTED=0')
    for x in failures:print(json.dumps(x,ensure_ascii=False))
    return 1 if failures else 0

if __name__=='__main__':
    raise SystemExit(main())
