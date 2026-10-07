"""Offscreen real catalogue delivery, migration, package and isolated rollback proof."""
from __future__ import annotations
import argparse, difflib, json, os, shutil, subprocess, sys
from pathlib import Path
import verify_pr12a_package as h
import verify_m2_capture_package as previous
ROOT=Path(__file__).resolve().parents[2]
TX=ROOT/'artifacts/real_skin_catalog'
RELEASE=ROOT/'dist/RelinkStudio'
BASELINE=TX/'baseline/release'
BUILD=ROOT/'build_relocated'
h.TX,h.BASELINE=TX,BASELINE
previous.TX,previous.BASELINE=TX,BASELINE

def build(): h.build()

def verify():
    baseline=json.loads((TX/'baseline_release_hashes.json').read_text(encoding='utf-8'))
    assert all(h.sha(BASELINE/name)==digest for name,digest in baseline.items())
    manifest=json.loads((RELEASE/'file_manifest.json').read_text(encoding='utf-8'))
    for name,entry in manifest['files'].items():
        path=(RELEASE/name).resolve()
        assert path.is_relative_to(RELEASE) and h.sha(path)==entry['sha256'] and path.stat().st_size==entry['bytes'],name
    assert {p.relative_to(RELEASE).as_posix() for p in RELEASE.rglob('*') if p.is_file()} == set(manifest['files'])|{'file_manifest.json'}
    for name in ('platforms/qwindows.dll','platforms/qoffscreen.dll','sqldrivers/qsqlite.dll','Qt6Sql.dll','docs/REAL_SKIN_CATALOG.md'):
        assert (RELEASE/name).is_file(),name
    shutil.copy2(RELEASE/'RelinkStudio.exe',TX/'MODIFIED_FILE.exe')
    assert h.sha(TX/'MODIFIED_FILE.exe')==h.sha(BUILD/'RelinkStudio.exe')!=h.sha(BASELINE/'RelinkStudio.exe')
    h.ui('MODIFIED',RELEASE,TX/'fixture_snapshots')
    h.run('MODIFIED_REAL_CATALOG',[str(RELEASE/'RelinkStudio.exe'),'--catalog-self-test','--snapshot-dir',str(TX/'real_snapshots')],
          'Same startup as normal application; embedded 149 skins; isolated extensions/config; actual dialog append, persistence, duplicate/collision rejection; no game input.',
          'CATALOG_SELF_TEST=PASS')
    report=json.loads((TX/'real_snapshots/catalog_report.json').read_text(encoding='utf-8'))
    assert report['passed'] and not report['game_connected'] and not report['system_input_sent']
    h.run('MODIFIED_STORAGE',[str(RELEASE/'RelinkStudio.exe'),'--storage-self-test'],
          'Packaged SQLite driver, temporary database, transactions and recovery.','STORAGE_SELF_TEST=PASS')
    env=h.environment(RELEASE);env['QT_PLUGIN_PATH']=str(RELEASE)
    for target,args in [('skin_catalog_tests',[str(ROOT/'src/assets/catalog/skins.json'),str(ROOT/'docs/business_rebuild/skin_reference/skin_reference.json')]),
                        ('catalog_configuration_tests',[str(ROOT/'src/assets/catalog/skins.json')]),
                        ('skin_catalog_dialog_tests',[str(ROOT/'src/assets/catalog/skins.json')]),
                        ('ui_module_tests',[])]:
        h.run(target.upper(),[str(BUILD/(target+'.exe')),*args],'Temporary state; packaged dependencies; explicit offline test data; no game.',env=env)
    # Test the actual historical user config on a separate copy, never in place.
    production=Path(os.environ['LOCALAPPDATA'])/'RelinkStudio/RelinkStudio/config.json'
    if production.exists():
        oldhash=h.sha(production)
        copied=TX/'production_config_copy/config.json';copied.parent.mkdir(exist_ok=True)
        shutil.copy2(production,copied)
        h.run('MODIFIED_PRODUCTION_COPY',[str(RELEASE/'RelinkStudio.exe'),'--snapshot-dir',str(TX/'production_copy_snapshots'),'--config',str(copied)],
              'Read-only source copy; original SHA256='+oldhash+'; same normal startup migration + geometry checks.','UI_SELF_TEST=PASS')
        value=json.loads(copied.read_text(encoding='utf-8'))
        assert value['demo'] is False and len(value['skins'])>=149
        assert all(not skin['id'].startswith('demo-') for skin in value['skins'])
        assert h.sha(production)==oldhash
        assert h.sha(Path(str(copied)+'.before-real-catalog.bak'))==oldhash
        (TX/'production_config_migration.json').write_text(json.dumps({'source_unchanged':True,'source_sha256':oldhash,'migrated_skins':len(value['skins']),'migrated_tasks':len(value['tasks']),'backup_exact':True},indent=2),encoding='utf-8')
    old=json.loads((TX/'baseline/ui_snapshots/ui_results.json').read_text(encoding='utf-8'))
    new=json.loads((TX/'fixture_snapshots/ui_results.json').read_text(encoding='utf-8'))
    names=lambda obj:{c.get('check',c.get('name')) for c in obj['checks']}
    replaced={'ART_RESOURCE','ART_TRANSPARENCY'}
    assert names(old)-replaced <= names(new)
    (TX/'intentional_ui_changes.json').write_text(json.dumps({'replaced_checks':sorted(replaced),'new_checks':['ART_RESOURCE_REMOVED','NO_DEMO_THUMBNAILS'],'reason':'User requested blank thumbnails; full real UI has 149 catalogue rows and no synthetic market data. Pixel equality to old demo is not an acceptance criterion.'},indent=2),encoding='utf-8')
    additions={p.relative_to(RELEASE).as_posix():h.sha(p) for p in RELEASE.rglob('*') if p.is_file() and p.relative_to(RELEASE).as_posix() not in baseline}
    (TX/'added_release_files.json').write_text(json.dumps(additions,indent=2),encoding='utf-8')
    previous.prepare_rollback()
    rollback=TX/'rollback_test';shutil.copytree(RELEASE,rollback,dirs_exist_ok=True)
    sentinels={}
    for name,value in [('catalog_extensions.json','{"schema":"relink-skin-catalog-v1","seasons":[],"skins":[]}\n'),('user_config.json','{"preserve":"user settings"}\n')]:
        path=rollback/name;path.write_text(value,encoding='utf-8');sentinels[name]=h.sha(path)
    bash=Path(r'C:\Program Files\Git\bin\bash.exe')
    script=TX.relative_to(ROOT).as_posix()+'/ROLLBACK.sh'
    h.run('ROLLBACK',[str(bash),'-c',f'chmod +x "{script}" && "./{script}" "$1"','rollback',str(rollback)],
          'Separate modified release copy; restore baseline managed files; keep extension/config sentinels.','ROLLBACK_RESTORED=PASS',runtime=bash.parent)
    assert all(h.sha(rollback/name)==digest for name,digest in baseline.items())
    assert all(h.sha(rollback/name)==digest for name,digest in sentinels.items())
    assert all(not (rollback/name).exists() for name in additions)
    h.ui('RESTORED',rollback,TX/'restored_snapshots')
    h.run('RESTORED_STORAGE',[str(rollback/'RelinkStudio.exe'),'--storage-self-test'],'Original package on isolated copy; temporary data.','STORAGE_SELF_TEST=PASS',runtime=rollback)
    assert h.sha(RELEASE/'RelinkStudio.exe')==h.sha(TX/'MODIFIED_FILE.exe')
    print('REAL_CATALOG_PACKAGE=PASS; original_hash_restored=true; user_data_preserved=true; modified_retained=true')

def finalize():
    records=json.loads((TX/'commands.json').read_text(encoding='utf-8'))
    latest={r['label']:r for r in records}
    for label in ('BASELINE','BASELINE_STORAGE','MODIFIED_BUILD','MODIFIED','MODIFIED_REAL_CATALOG','MODIFIED_STORAGE','ROLLBACK','RESTORED','RESTORED_STORAGE'):
        assert latest[label]['exit_status']==0,label
    revision=(TX/'baseline/source_revision.txt').read_text(encoding='ascii').strip()
    names=set(subprocess.check_output(['git','diff','--name-only',revision],cwd=ROOT,text=True).splitlines())
    names.update(subprocess.check_output(['git','ls-files','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines())
    patch=[]
    for name in sorted(names):
        if not (name.startswith(('src/','tests/','docs/')) or name in {'.gitignore','CMakeLists.txt','build.ps1','resources.qrc','SESSION_START.md','README.md'}):continue
        before=subprocess.run(['git','show',f'{revision}:{name}'],cwd=ROOT,capture_output=True)
        old=before.stdout.decode('utf-8-sig').splitlines(True) if before.returncode==0 else []
        new=(ROOT/name).read_text(encoding='utf-8-sig').splitlines(True)
        patch.extend(difflib.unified_diff(old,new,fromfile='a/'+name,tofile='b/'+name))
    (TX/'DIFF_FILE').write_text(''.join(patch),encoding='utf-8')
    lines=['Delta Market Assistant — Real Skin Catalog','Date: 2026-10-07 (Asia/Shanghai)','Changed branch: main',
           'Baseline source: '+revision,
           'Changed fields: catalogProductId/menuColor/variant/skinSeries/known flags/dataSource; default startup real catalog; empty artwork/history; season append/import; atomic extensions; legacy config backups.',
           'Sources: user screenshot-backed S1-S11 149-item reference, not a claim about later game updates.',
           'Test fixtures remain explicitly opt-in diagnostics, never normal startup or simulated running buttons.',
           'Restored behavior/status: isolated baseline UI/workspace/SQLite tests pass with exit 0; user extension/config sentinels preserved.',
           'Delivery and MODIFIED_FILE remain modified; no game window, game capture or input used in this transaction.',
           'DELIVERY: '+str(RELEASE/'RelinkStudio.exe')]
    for name in ('MODIFIED_FILE.exe','DIFF_FILE','VERIFICATION.txt','ROLLBACK.sh'):lines.append(name+': '+str(TX/name))
    for label,path in [('BASELINE_SHA256',BASELINE/'RelinkStudio.exe'),('MODIFIED_SHA256',TX/'MODIFIED_FILE.exe'),('RESTORED_SHA256',TX/'rollback_test/RelinkStudio.exe')]:lines.append(label+': '+h.sha(path))
    lines+=['','Exact commands, input, literal output/result, and exit status:']
    for record in records:
        lines+=['',record['label'],'COMMAND: '+record['command'],'INPUT: '+record['input'],'STDOUT:',record['stdout'].rstrip(),'STDERR:',record['stderr'].rstrip() or '(empty)','EXIT_STATUS: '+str(record['exit_status'])]
    (TX/'VERIFICATION.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    for name in ('MODIFIED_FILE.exe','DIFF_FILE','VERIFICATION.txt','ROLLBACK.sh'):
        assert (TX/name).read_bytes();print('REOPEN=PASS; path='+str(TX/name))
    assert h.sha(RELEASE/'RelinkStudio.exe')==h.sha(TX/'MODIFIED_FILE.exe')
    print('REAL_CATALOG_TRANSACTION=PASS')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('phase',choices=['build','verify','finalize']);args=parser.parse_args();globals()[args.phase]()
