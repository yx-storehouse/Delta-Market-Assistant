"""Collection workflow: real input, durable GUI/CLI import, isolated rollback."""
from __future__ import annotations
import argparse,difflib,json,os,shutil,subprocess,sys
from pathlib import Path
import verify_pr12a_package as h
import verify_m2_capture_package as old
ROOT=Path(__file__).resolve().parents[2];TX=ROOT/'artifacts/collection_workflow';RELEASE=ROOT/'dist/RelinkStudio';BASELINE=TX/'baseline/release';BUILD=ROOT/'build_relocated'
h.TX,h.BASELINE=TX,BASELINE;old.TX,old.BASELINE=TX,BASELINE
SOURCE=ROOT/'artifacts/m2_savedvalue_collection/input/source.savedValue'
DICTIONARY=ROOT/'docs/business_rebuild/implementation/domain/relink_0925_catalog.json';CATALOG=ROOT/'src/assets/catalog/skins.json'

def expect(label,args,inputs,status):
    r=subprocess.run(args,cwd=ROOT,env=h.environment(RELEASE),capture_output=True,text=True,encoding='utf-8',errors='replace',creationflags=subprocess.CREATE_NO_WINDOW,timeout=60)
    record=dict(label=label,command=subprocess.list2cmdline(args),input=inputs,stdout=r.stdout,stderr=r.stderr,exit_status=r.returncode,expected_exit_status=status)
    path=TX/'commands.json';records=json.loads(path.read_text(encoding='utf-8'));records.append(record);path.write_text(json.dumps(records,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(label+'_EXIT='+str(r.returncode));assert r.returncode==status,record

def build():h.build()

def verify():
    original=json.loads((TX/'baseline_release_hashes.json').read_text(encoding='utf-8'))
    assert all(h.sha(BASELINE/name)==digest for name,digest in original.items())
    manifest=json.loads((RELEASE/'file_manifest.json').read_text(encoding='utf-8'))
    for name,item in manifest['files'].items():
        file=(RELEASE/name).resolve();assert file.is_relative_to(RELEASE) and h.sha(file)==item['sha256'] and file.stat().st_size==item['bytes'],name
    for name in ('platforms/qwindows.dll','platforms/qoffscreen.dll','sqldrivers/qsqlite.dll','Qt6Sql.dll','docs/COLLECTION_TASK_IMPORT.md'):assert (RELEASE/name).is_file()
    shutil.copy2(RELEASE/'RelinkStudio.exe',TX/'MODIFIED_FILE.exe')
    assert h.sha(TX/'MODIFIED_FILE.exe')==h.sha(BUILD/'RelinkStudio.exe')!=h.sha(BASELINE/'RelinkStudio.exe')
    h.ui('MODIFIED',RELEASE,TX/'fixture_snapshots')
    h.run('MODIFIED_CATALOG',[str(RELEASE/'RelinkStudio.exe'),'--catalog-self-test','--snapshot-dir',str(TX/'catalog_snapshots')],'Existing catalogue regression; isolated data.','CATALOG_SELF_TEST=PASS')
    sourceHash=h.sha(SOURCE);assert sourceHash=='cff452bcd4b77a17fb3cc9a23f55e0131304098f7ac0057205f619bbd4406253'
    h.run('MODIFIED_IMPORT_UI',[str(RELEASE/'RelinkStudio.exe'),'--collection-import-self-test','--task-source',str(SOURCE),'--snapshot-dir',str(TX/'real_import_snapshots')],
          'Read actual frozen 8926-byte source; real MainWindow dialog, failure/retry/atomic save/reload/dedup; all config outputs temporary.','COLLECTION_IMPORT_SELF_TEST=PASS')
    env=h.environment(RELEASE);env['QT_PLUGIN_PATH']=str(RELEASE)
    for target in ('collection_task_import_tests','collection_task_commit_tests','collection_import_dialog_tests'):
        h.run(target.upper(),[str(BUILD/(target+'.exe')),str(DICTIONARY),str(CATALOG),str(SOURCE)],'Actual source read-only; packaged dependencies; isolated stores.',env=env)
    h.run('COLLECTION_OFFLINE_TESTS',[sys.executable,'-X','utf8','-m','unittest','discover','-s','tests/manual','-p','test_collection*.py','-v'],
          'Pure offline candidate/journal/scroll same-frame and stale-coordinate regressions; no OS input.',env=os.environ.copy())
    cli=TX/'cli_tests';cli.mkdir(exist_ok=True);config=cli/'config.json'
    production=Path(os.environ['LOCALAPPDATA'])/'RelinkStudio/RelinkStudio/config.json'
    originalHash=h.sha(production);shutil.copy2(production,config)
    h.run('MODIFIED_CLI_IMPORT',[str(RELEASE/'RelinkStudio.exe'),'--import-collection-tasks',str(SOURCE),'--config',str(config)],
          'Actual source into a COPY of production config; no production file writes.','COLLECTION_IMPORT=PASS')
    data=json.loads(config.read_text(encoding='utf-8'));imported=[t for t in data['tasks'] if t.get('importSource',{}).get('sourceSha256')==sourceHash]
    assert len(data['skins'])>=149 and len(imported)==50 and sum(t['enabled'] for t in imported)==21
    afterHash=h.sha(config)
    h.run('MODIFIED_CLI_DUPLICATE',[str(RELEASE/'RelinkStudio.exe'),'--import-collection-tasks',str(SOURCE),'--config',str(config)],'Same file, same target; 50 duplicates, no writes.','added=0; duplicates=50')
    assert h.sha(config)==afterHash
    bad=cli/'bad.savedValue';bad.write_bytes(b'not a configuration')
    expect('INVALID_SOURCE_PRESERVES_CONFIG',[str(RELEASE/'RelinkStudio.exe'),'--import-collection-tasks',str(bad),'--config',str(config)],'Malformed task input; existing config hash must remain unchanged.',1)
    assert h.sha(config)==afterHash
    expect('EXPLICIT_CONFIG_REQUIRED',[str(RELEASE/'RelinkStudio.exe'),'--import-collection-tasks',str(SOURCE)],'No implicit production destination allowed.',2)
    assert h.sha(production)==originalHash and h.sha(SOURCE)==sourceHash
    h.run('MODIFIED_STORAGE',[str(RELEASE/'RelinkStudio.exe'),'--storage-self-test'],'Packaged SQLite dependency and transaction checks.','STORAGE_SELF_TEST=PASS')
    additions={p.relative_to(RELEASE).as_posix():h.sha(p) for p in RELEASE.rglob('*') if p.is_file() and p.relative_to(RELEASE).as_posix() not in original}
    (TX/'added_release_files.json').write_text(json.dumps(additions,indent=2),encoding='utf-8');old.prepare_rollback()
    rollback=TX/'rollback_test';shutil.copytree(RELEASE,rollback,dirs_exist_ok=True)
    sentinel=rollback/'user_tasks_and_catalog.json';sentinel.write_bytes(config.read_bytes());sentinelHash=h.sha(sentinel)
    bash=Path(r'C:\Program Files\Git\bin\bash.exe');script=TX.relative_to(ROOT).as_posix()+'/ROLLBACK.sh'
    h.run('ROLLBACK',[str(bash),'-c',f'chmod +x "{script}" && "./{script}" "$1"','rollback',str(rollback)],'Separate updated release copy; restore baseline program; retain task/catalog sentinel.','ROLLBACK_RESTORED=PASS',runtime=bash.parent)
    assert all(h.sha(rollback/name)==digest for name,digest in original.items()) and h.sha(sentinel)==sentinelHash
    assert all(not (rollback/name).exists() for name in additions)
    h.ui('RESTORED',rollback,TX/'restored_snapshots')
    h.run('RESTORED_CATALOG',[str(rollback/'RelinkStudio.exe'),'--catalog-self-test','--snapshot-dir',str(TX/'restored_catalog_snapshots')],'Restored previous catalogue version; isolated original supported state.','CATALOG_SELF_TEST=PASS',runtime=rollback)
    assert h.sha(RELEASE/'RelinkStudio.exe')==h.sha(TX/'MODIFIED_FILE.exe')
    print('COLLECTION_WORKFLOW_PACKAGE=PASS; original_hash_restored=true; task_data_retained=true; source_unchanged=true; delivery_modified=true')

def finalize():
    records=json.loads((TX/'commands.json').read_text(encoding='utf-8'));latest={r['label']:r for r in records}
    for name in ('BASELINE','BASELINE_STORAGE','BASELINE_CATALOG','MODIFIED_BUILD','MODIFIED','MODIFIED_IMPORT_UI','MODIFIED_CLI_IMPORT','MODIFIED_CLI_DUPLICATE','MODIFIED_STORAGE','ROLLBACK','RESTORED','RESTORED_CATALOG'):
        assert latest[name]['exit_status']==0,name
    revision=(TX/'baseline/source_revision.txt').read_text().strip();names=set(subprocess.check_output(['git','diff','--name-only',revision],cwd=ROOT,text=True).splitlines());names.update(subprocess.check_output(['git','ls-files','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines());patch=[]
    for name in sorted(names):
        if not (name.startswith(('src/','tests/','docs/')) or name in {'.gitignore','CMakeLists.txt','build.ps1','resources.qrc','SESSION_START.md','README.md'}):continue
        before=subprocess.run(['git','show',f'{revision}:{name}'],cwd=ROOT,capture_output=True);a=before.stdout.decode('utf-8-sig').splitlines(True) if before.returncode==0 else [];b=(ROOT/name).read_text(encoding='utf-8-sig').splitlines(True);patch.extend(difflib.unified_diff(a,b,fromfile='a/'+name,tofile='b/'+name))
    (TX/'DIFF_FILE').write_text(''.join(patch),encoding='utf-8')
    lines=['Delta Market Assistant — Collection workflow','Date: 2026-10-07 (Asia/Shanghai)','Changed branch: main','Baseline source: '+revision,
        'Changed fields: Task.quantity=0 unlimited, Task.importSource, data-only savedValue preview/dedup, durable append/backup, task GUI entry, explicit-config CLI, offline scroll geometry leases.',
        'Actual source: 50 rows / 21 enabled / 13 total products / 9 enabled products, SHA256 '+h.sha(SOURCE),
        'Live precheck used the baseline binary; final independent empty-watchlist confirmation used the final package. No favorites or purchases changed; IDE restored at each batch end. Exact binary hashes are in each persisted live result.',
        'Dynamic scroll contract tests are offline only; no C++ dynamic edge detector or collection executor is claimed complete.',
        'Restored behavior/status: original catalogue/UI/workspace self-tests pass with exit 0 on isolated restored copy. Task/config sentinel preserved.',
        'Rollback changes only program files; it does not remove user tasks or undo historical game favorites. Previous version may not load new quantity=0 configurations; pre-import backups preserve original supported config.',
        'DELIVERY: '+str(RELEASE/'RelinkStudio.exe')]
    for name in ('MODIFIED_FILE.exe','DIFF_FILE','VERIFICATION.txt','ROLLBACK.sh'):lines.append(name+': '+str(TX/name))
    for key,path in [('BASELINE_SHA256',BASELINE/'RelinkStudio.exe'),('MODIFIED_SHA256',TX/'MODIFIED_FILE.exe'),('RESTORED_SHA256',TX/'rollback_test/RelinkStudio.exe')]:lines.append(key+': '+h.sha(path))
    for r in records:lines+=['',r['label'],'COMMAND: '+r['command'],'INPUT: '+r['input'],('RESULT_FILE:' if r.get('result_file_not_stdout') else 'STDOUT:'),r['stdout'].rstrip(),'STDERR:',r['stderr'].rstrip() or '(empty)','EXIT_STATUS: '+str(r['exit_status'])]
    (TX/'VERIFICATION.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    for name in ('MODIFIED_FILE.exe','DIFF_FILE','VERIFICATION.txt','ROLLBACK.sh'):assert (TX/name).read_bytes();print('REOPEN=PASS; path='+str(TX/name))
    assert h.sha(RELEASE/'RelinkStudio.exe')==h.sha(TX/'MODIFIED_FILE.exe');print('COLLECTION_WORKFLOW_TRANSACTION=PASS')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('phase',choices=['build','verify','finalize']);args=p.parse_args();globals()[args.phase]()
