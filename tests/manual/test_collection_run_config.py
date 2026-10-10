"""Offline current-config snapshot checks; only isolated temporary outputs are written."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from collection_run_config import build_run_snapshot, snapshot_from_files, write_run_snapshot

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / 'src/assets/catalog/skins.json'


def encoded(value):
    return json.dumps(value, ensure_ascii=False).encode('utf-8')


class CollectionRunConfig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog_bytes = CATALOG.read_bytes()
        cls.catalog = json.loads(cls.catalog_bytes)
        cls.products = {x['product_id']: x for x in cls.catalog['skins']}

    def skin(self, pid):
        p = self.products[pid]
        return dict(id='catalog:' + pid, catalogProductId=pid, name=p['display_name'],
            series=p['season_id'] + ' · ' + p['season_label'], rarity=p['grade'] or '待核对',
            variant=p['variant_label'], skinSeries=p['skin_series'], dataSource='catalog')

    def config(self):
        original = dict(format='savedValue', sourceSha256='a' * 64, dictionarySha256='b' * 64,
            productId='10602', conditionId='0', row=12,
            fields={'启用': False, '成色设置栏': 0, '最低价格设置栏': 10, '最高价格设置栏': 600,
                    '枪名设置栏': 10602, '磨损度设置栏': 5, '限量设置栏': 0})
        return dict(schema_version=1, demo=False, source='catalog_configuration',
            run_settings={'autoCollect': True, 'purchaseDelayMs': 1234, 'burstClick': True},
            skins=[self.skin('10602'), self.skin('10603'), self.skin('11102')],
            tasks=[dict(id='edited-source-task', name='当前编辑过的收藏任务', skinId='catalog:10602',
                        enabled=True, condition='成色B', minPrice=11.25, maxPrice=420.5,
                        maxWear=0.25, quantity=7, importSource=original),
                   dict(id='disabled-task', name='保留但不执行', enabled=False),
                   dict(id='manual-task', name='我的新任务', skinId='catalog:10603', enabled=True,
                        condition='仅磨损', minPrice=0, maxPrice=500, maxWear=5, quantity=0)])

    def snapshot(self, config=None, catalog=None, extension=None):
        return build_run_snapshot(encoded(self.config() if config is None else config),
                                  self.catalog_bytes if catalog is None else encoded(catalog),
                                  None if extension is None else encoded(extension))

    def test_current_values_not_historical_origin(self):
        result = self.snapshot()
        row = result['rows'][0]
        self.assertEqual((row['price_min'], row['price_max'], row['max_wear'], row['limit_raw'], row['condition_label']),
                         ('11.25', '420.5', '0.25', 7, '成色B'))
        self.assertTrue(row['enabled'])
        self.assertEqual(row['import_source'], self.config()['tasks'][0]['importSource'])
        self.assertEqual(row['import_source']['fields']['最高价格设置栏'], 600)
        self.assertFalse(row['import_source']['fields']['启用'])

    def test_only_enabled_order_current_index_and_original_index_separate(self):
        result = self.snapshot()
        self.assertEqual([r['row_index'] for r in result['rows']], [0, 2])
        self.assertEqual([r['task_id'] for r in result['rows']], ['edited-source-task', 'manual-task'])
        self.assertEqual(result['rows'][0]['source_row_index'], 12)
        self.assertEqual((result['config_task_count'], result['disabled_task_count'], result['row_count']), (3, 1, 2))

    def test_product_binding_and_season_from_real_catalog(self):
        row = self.snapshot()['rows'][0]
        self.assertEqual((row['product_id'], row['source_product_id'], row['season_id'], row['season_label']),
                         ('10602', '10602', 'S6', '棱镜攻势S2'))
        self.assertEqual(row['product_name'], 'AUG突击步枪-天命')
        self.assertEqual(row['game_grade'], '史诗品阶')  # the catalogue's own 品阶
        self.assertEqual(row['grade'], 'any')  # the task itself names no grade
        self.assertNotIn('menu_color', row)
        self.assertFalse(self.snapshot()['grade_inferred_from_color'])

    def test_catalogue_grade_must_match_the_programs_projection(self):
        config = self.config()
        config['skins'][0]['rarity'] = '稀有品阶'
        with self.assertRaisesRegex(ValueError, 'CATALOG_PROJECTION_CONFLICT'):
            self.snapshot(config)
        config['skins'][0]['rarity'] = '待核对'  # no colour to vouch for it
        with self.assertRaisesRegex(ValueError, 'CATALOG_PROJECTION_CONFLICT'):
            self.snapshot(config)
        config['skins'][0]['rarity'] = '史诗品阶'
        config['skins'][0]['menuColor'] = 'purple'  # an older file's extra key is ignored
        self.assertEqual(self.snapshot(config)['rows'][0]['game_grade'], '史诗品阶')

    def test_a_config_saved_by_the_previous_program_still_runs(self):
        # The previous GUI saved rarity 待核对 plus the menu colour. Its colour
        # must name the catalogue's 品阶; a different colour is still a conflict.
        config = self.config()
        for skin in config['skins']:
            skin['rarity'] = '待核对'
            skin['menuColor'] = 'purple'
        self.assertEqual([r['game_grade'] for r in self.snapshot(config)['rows']], ['史诗品阶', '史诗品阶'])
        config['skins'][0]['menuColor'] = 'blue'
        with self.assertRaisesRegex(ValueError, 'CATALOG_PROJECTION_CONFLICT'):
            self.snapshot(config)

    def test_added_skin_name_is_built_from_its_fields_like_the_program_does(self):
        for written in ('', 'P90 冲锋枪 - 新系列', '别的名字'):
            extension = dict(schema='relink-skin-catalog-v2', seasons=[dict(id='S12', label='新赛季')],
                skins=[dict(product_id='user:p90', season_id='S12', season_label='新赛季', weapon='P90 冲锋枪',
                            skin_series='新系列', variant_label='', grade='史诗品阶', display_name=written,
                            thumbnail_path='')])
            config = self.config()
            config['skins'][0] = dict(id='catalog:user:p90', catalogProductId='user:p90', name='S12|P90 冲锋枪 - 新系列',
                series='S12 · 新赛季', rarity='史诗品阶', variant='', skinSeries='新系列', dataSource='catalog')
            config['tasks'][0]['skinId'] = 'catalog:user:p90'
            row = self.snapshot(config, extension=extension)['rows'][0]
            self.assertEqual((row['display_name'], row['product_name']), ('S12|P90 冲锋枪 - 新系列', 'P90冲锋枪-新系列'))

    def test_banner_explains_a_program_and_catalogue_mismatch(self):
        from collection_status import stop_reason
        for code in ('RUN_CATALOG_SCHEMA', 'RUN_TASK_CATALOG_PROJECTION_CONFLICT'):
            self.assertIn('重新打开 RelinkStudio', stop_reason(code))

    def test_runner_reads_the_catalogue_shipped_with_its_program(self):
        import run_collection_hotkey as hotkey
        with tempfile.TemporaryDirectory() as directory:
            release = Path(directory) / 'RelinkStudio'
            (release / 'collection').mkdir(parents=True)
            script = release / 'collection' / 'run_collection_hotkey.py'
            script.write_text('', encoding='utf-8')
            self.assertEqual(hotkey.default_catalog(script), hotkey.ROOT / 'src/assets/catalog/skins.json')
            (release / 'RelinkStudio.exe').write_bytes(b'')
            (release / 'catalog').mkdir()
            (release / 'catalog' / 'skins.json').write_bytes(b'{}')
            self.assertEqual(hotkey.default_catalog(script), (release / 'catalog' / 'skins.json').resolve())
        self.assertEqual(hotkey.default_catalog(hotkey.__file__), hotkey.ROOT / 'src/assets/catalog/skins.json')

    def test_every_builtin_skin_has_a_game_grade(self):
        self.assertEqual(self.catalog['schema'], 'relink-skin-catalog-v2')
        grades = [p['grade'] for p in self.catalog['skins']]
        self.assertEqual(len(grades), 149)
        self.assertEqual({g: grades.count(g) for g in set(grades)}, {'传说品阶': 28, '史诗品阶': 33, '稀有品阶': 88})
        self.assertFalse(any('menu_color' in p or 'game_quality_name' in p for p in self.catalog['skins']))

    def test_v1_extension_colour_becomes_its_grade_and_bad_grades_stop(self):
        def extension(**fields):
            skin = dict(product_id='user:old', season_id='S12', season_label='旧赛季', weapon='P90 冲锋枪',
                        skin_series='旧系列', variant_label='', display_name='S12|P90 冲锋枪 - 旧系列',
                        thumbnail_path='', **fields)
            return dict(schema='relink-skin-catalog-v1' if 'menu_color' in fields else 'relink-skin-catalog-v2',
                        seasons=[dict(id='S12', label='旧赛季')], skins=[skin])
        def run(ext, rarity):
            config = self.config()
            config['skins'][0] = dict(id='catalog:user:old', catalogProductId='user:old',
                name='S12|P90 冲锋枪 - 旧系列', series='S12 · 旧赛季', rarity=rarity,
                variant='', skinSeries='旧系列', dataSource='catalog')
            config['tasks'][0]['skinId'] = 'catalog:user:old'
            return self.snapshot(config, extension=ext)['rows'][0]['game_grade']
        for colour, grade in (('orange', '传说品阶'), ('red', '传说品阶'), ('purple', '史诗品阶'), ('blue', '稀有品阶')):
            self.assertEqual(run(extension(menu_color=colour, game_quality_name=''), grade), grade)
        self.assertEqual(run(extension(menu_color='unknown', game_quality_name=''), '待核对'), '')
        self.assertEqual(run(extension(menu_color='blue', game_quality_name='史诗品阶'), '史诗品阶'), '史诗品阶')
        # The old dialog's free-text quality: a grade name (with or without 品阶) wins, else the colour.
        self.assertEqual(run(extension(menu_color='blue', game_quality_name='传说'), '传说品阶'), '传说品阶')
        self.assertEqual(run(extension(menu_color='blue', game_quality_name='紫色'), '稀有品阶'), '稀有品阶')
        self.assertEqual(run(extension(menu_color='unknown', game_quality_name='高级'), '待核对'), '')
        self.assertEqual(run(extension(grade='传说品阶'), '传说品阶'), '传说品阶')
        for bad in (extension(grade='普通品阶'), extension(grade='purple'), extension(menu_color='green', game_quality_name=''),
                    extension(grade='史诗品阶', menu_color='purple')):
            with self.assertRaises(ValueError):
                run(bad, '史诗品阶')

    def test_edited_product_wins_over_origin_product(self):
        config = self.config()
        config['tasks'][0]['skinId'] = 'catalog:10603'
        row = self.snapshot(config)['rows'][0]
        self.assertEqual(row['product_id'], '10603')
        self.assertEqual(row['product_name'], 'P90冲锋枪-天命')
        self.assertEqual(row['import_source']['productId'], '10602')

    def test_real_current_config_hash_and_no_purchase_activation(self):
        config = encoded(self.config())
        result = build_run_snapshot(config, self.catalog_bytes)
        self.assertEqual(result['source_kind'], 'current_config')
        self.assertEqual(result['source_sha256'], hashlib.sha256(config).hexdigest())
        self.assertEqual(result['source_sha256'], result['config_sha256'])
        self.assertNotEqual(result['source_sha256'], result['rows'][0]['import_source']['sourceSha256'])
        self.assertEqual(result['mode'], 'collect_only')
        self.assertFalse(result['purchase_phase_enabled'])
        self.assertFalse(result['run_settings_applied'])
        self.assertNotIn('purchaseDelayMs', result)
        self.assertFalse(result['game_input_sent'])

    def test_unlimited_and_nonzero_limits_retained(self):
        rows = self.snapshot()['rows']
        self.assertEqual(rows[0]['quantity_semantics'], 'confirmed_collection_limit')
        self.assertEqual(rows[0]['limit_raw'], 7)
        self.assertEqual(rows[1]['quantity_semantics'], 'unlimited')
        self.assertEqual(rows[1]['limit_raw'], 0)
        self.assertIsNone(rows[1]['import_source'])
        self.assertIsNone(rows[1]['source_row_index'])

    def test_condition_not_guessed_or_changed(self):
        for condition in ('成色S', '成色A', '成色B', '成色C', '仅磨损', '不限'):
            with self.subTest(condition=condition):
                config = self.config()
                config['tasks'][0]['condition'] = condition
                self.assertEqual(self.snapshot(config)['rows'][0]['condition_label'], condition)

    def test_s11_spelling_is_not_fuzzy_alias(self):
        config = self.config()
        config['tasks'][0]['skinId'] = 'catalog:11102'
        self.assertEqual(self.snapshot(config)['rows'][0]['season_label'], '疾风魅影')
        config['skins'][2]['series'] = 'S11 · 疾光魅影'
        with self.assertRaisesRegex(ValueError, 'PROJECTION_CONFLICT'):
            self.snapshot(config)

    def test_edited_values_change_run_hash_not_import_hash(self):
        config = self.config()
        before = self.snapshot(config)
        config['tasks'][0]['maxPrice'] = 333
        after = self.snapshot(config)
        self.assertNotEqual(before['config_sha256'], after['config_sha256'])
        self.assertEqual(before['rows'][0]['import_source'], after['rows'][0]['import_source'])
        self.assertEqual(after['rows'][0]['price_max'], '333')

    def test_same_source_row_in_different_files_remains_unique(self):
        config = self.config()
        another = copy.deepcopy(config['tasks'][0])
        another['id'] = 'another-import'
        another['importSource']['sourceSha256'] = 'c' * 64
        config['tasks'].append(another)
        rows = self.snapshot(config)['rows']
        self.assertEqual([r['row_index'] for r in rows], [0, 2, 3])
        self.assertEqual(rows[0]['source_row_index'], rows[2]['source_row_index'])

    def test_duplicate_source_row_is_blocked(self):
        config = self.config()
        another = copy.deepcopy(config['tasks'][0])
        another['id'] = 'another-id'
        config['tasks'].append(another)
        with self.assertRaisesRegex(ValueError, 'DUPLICATE_SOURCE_ROW'):
            self.snapshot(config)

    def test_duplicate_task_or_skin_or_catalog_id_blocked(self):
        config = self.config()
        config['tasks'][2]['id'] = config['tasks'][0]['id']
        with self.assertRaisesRegex(ValueError, 'DUPLICATE_ID'):
            self.snapshot(config)
        config = self.config()
        config['skins'].append(copy.deepcopy(config['skins'][0]))
        with self.assertRaisesRegex(ValueError, 'DUPLICATE_SKIN'):
            self.snapshot(config)
        catalog = copy.deepcopy(self.catalog)
        catalog['skins'].append(copy.deepcopy(catalog['skins'][0]))
        with self.assertRaisesRegex(ValueError, 'PRODUCT_ID'):
            self.snapshot(catalog=catalog)

    def test_missing_task_link_and_mismatched_metadata_blocked(self):
        for field, value in [('skinId', 'missing'), ('skinId', 'catalog:999999')]:
            config = self.config()
            config['tasks'][0][field] = value
            with self.assertRaisesRegex(ValueError, 'CATALOG_LINK_MISSING'):
                self.snapshot(config)
        for field, value in [('catalogProductId', ''), ('name', 'different title'), ('rarity', '稀有品阶'),
                             ('series', 'S11 · 疾风魅影'), ('variant', '极品'), ('dataSource', 'test_fixture')]:
            with self.subTest(field=field):
                config = self.config()
                config['skins'][0][field] = value
                with self.assertRaises(ValueError):
                    self.snapshot(config)

    def test_ambiguous_same_season_label_blocked(self):
        catalog = copy.deepcopy(self.catalog)
        duplicate = copy.deepcopy(self.products['10602'])
        duplicate['product_id'] = 'user:duplicate-name'
        catalog['skins'].append(duplicate)
        with self.assertRaisesRegex(ValueError, 'LABEL_AMBIGUOUS'):
            self.snapshot(catalog=catalog)

    def test_catalog_identity_conflict_and_unknown_quality_not_inferred(self):
        for field, value in [('season_label', '疾光魅影'), ('weapon', 'P90 冲锋枪'), ('skin_series', 0)]:
            with self.subTest(field=field):
                catalog = copy.deepcopy(self.catalog)
                next(x for x in catalog['skins'] if x['product_id'] == '10602')[field] = value
                with self.assertRaises(ValueError):
                    self.snapshot(catalog=catalog)
        # The display name follows from the fields (same rule as the program):
        # a wrong one is rebuilt, never trusted as a different product.
        catalog = copy.deepcopy(self.catalog)
        next(x for x in catalog['skins'] if x['product_id'] == '10602')['display_name'] = 'S11|AUG 突击步枪 - 天命'
        row = self.snapshot(catalog=catalog)['rows'][0]
        self.assertEqual((row['display_name'], row['season_id'], row['product_name']),
                         ('S6|AUG 突击步枪 - 天命', 'S6', 'AUG突击步枪-天命'))

    def test_invalid_or_missing_current_values_blocked_not_recovered_from_origin(self):
        for field, value in [('minPrice', -1), ('maxPrice', 1000000000), ('minPrice', '10'),
                             ('maxPrice', True), ('maxPrice', 420.501), ('maxWear', 101),
                             ('maxWear', 0.0000001), ('quantity', -1), ('quantity', 1.5),
                             ('quantity', 10000), ('condition', '成色D'), ('condition', []), ('enabled', 1)]:
            with self.subTest(field=field, value=value):
                config = self.config()
                config['tasks'][0][field] = value
                with self.assertRaises(ValueError):
                    self.snapshot(config)
        config = self.config()
        del config['tasks'][0]['maxPrice']
        with self.assertRaises(ValueError):
            self.snapshot(config)
        config = self.config()
        config['tasks'][0]['minPrice'] = 500
        with self.assertRaisesRegex(ValueError, 'PRICE_RANGE'):
            self.snapshot(config)

    def test_binary_double_noise_matches_gui_precision_not_historical_data(self):
        config = self.config()
        config['tasks'][0]['minPrice'] = 0.1 + 0.2
        row = self.snapshot(config)['rows'][0]
        self.assertEqual(row['price_min'], '0.3')
        self.assertEqual(row['current_value_literals']['minPrice'], '0.30000000000000004')
        self.assertEqual(row['import_source']['fields']['最低价格设置栏'], 10)

    def test_no_active_tasks_and_demo_blocked(self):
        config = self.config()
        for task in config['tasks']:
            task['enabled'] = False
        with self.assertRaisesRegex(ValueError, 'NO_ENABLED_TASKS'):
            self.snapshot(config)
        config = self.config()
        config['demo'] = True
        with self.assertRaisesRegex(ValueError, 'NOT_REAL_V1'):
            self.snapshot(config)

    def test_duplicate_json_nonfinite_and_oversized_inputs_blocked(self):
        for data in (b'{"schema_version":1,"schema_version":1}', b'{"value":NaN}',
                     b'{"value":Infinity}', b'[]', b'{', b'\xff', b' ' * (8 * 1024 * 1024 + 1)):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                build_run_snapshot(data, self.catalog_bytes)

    def test_malformed_source_metadata_is_not_silently_ignored(self):
        for key, value in [('sourceSha256', 'bad'), ('productId', '10603'), ('row', 500), ('fields', {})]:
            config = self.config()
            config['tasks'][0]['importSource'][key] = value
            with self.assertRaises(ValueError):
                self.snapshot(config)

    def test_extension_new_season_supported_and_conflicts_blocked(self):
        extension = dict(schema='relink-skin-catalog-v2', seasons=[dict(id='S12', label='新赛季')],
            skins=[dict(product_id='user:new', season_id='S12', season_label='新赛季',
                        weapon='AUG 突击步枪', skin_series='新系列', variant_label='', grade='',
                        display_name='S12|AUG 突击步枪 - 新系列', thumbnail_path='')])
        config = self.config()
        config['skins'][0] = dict(id='catalog:user:new', catalogProductId='user:new',
            name=extension['skins'][0]['display_name'], series='S12 · 新赛季', rarity='待核对',
            variant='', skinSeries='新系列', dataSource='catalog')
        config['tasks'][0]['skinId'] = 'catalog:user:new'
        row = self.snapshot(config, extension=extension)['rows'][0]
        self.assertEqual((row['product_id'], row['season_id']), ('user:new', 'S12'))
        self.assertIsNotNone(self.snapshot(config, extension=extension)['catalog_extensions_sha256'])
        with self.assertRaisesRegex(ValueError, 'PRODUCT_UNKNOWN'):
            self.snapshot(config)
        with self.assertRaisesRegex(ValueError, 'EXTENSION_PRODUCT_CONFLICT'):
            self.snapshot(extension=self.catalog)

    def test_disk_snapshot_exclusive_and_input_hashes_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config, catalog, output = root / 'config.json', root / 'catalog.json', root / 'run.json'
            config.write_bytes(encoded(self.config()))
            catalog.write_bytes(self.catalog_bytes)
            before = {p: p.read_bytes() for p in (config, catalog)}
            frozen = snapshot_from_files(config, catalog)
            digest = write_run_snapshot(frozen, output)
            self.assertEqual(digest, hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(json.loads(output.read_text(encoding='utf-8')), frozen)
            self.assertEqual({p: p.read_bytes() for p in before}, before)
            original = output.read_bytes()
            with self.assertRaises(FileExistsError):
                write_run_snapshot(frozen, output)
            self.assertEqual(output.read_bytes(), original)
            with self.assertRaisesRegex(ValueError, 'WOULD_REPLACE_INPUT'):
                write_run_snapshot(frozen, config)

    def test_freeze_survives_subsequent_config_edit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config, catalog, output = root / 'config.json', root / 'catalog.json', root / 'run.json'
            document = self.config()
            config.write_bytes(encoded(document))
            catalog.write_bytes(self.catalog_bytes)
            frozen = snapshot_from_files(config, catalog)
            write_run_snapshot(frozen, output)
            document['tasks'][0]['maxPrice'] = 222
            config.write_bytes(encoded(document))
            fresh = snapshot_from_files(config, catalog)
            self.assertEqual(json.loads(output.read_text(encoding='utf-8'))['rows'][0]['price_max'], '420.5')
            self.assertEqual(fresh['rows'][0]['price_max'], '222')
            self.assertNotEqual(fresh['config_sha256'], frozen['config_sha256'])

    def test_extension_adjacent_auto_load_and_explicit_missing_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config, catalog = root / 'config.json', root / 'catalog.json'
            config.write_bytes(encoded(self.config()))
            catalog.write_bytes(self.catalog_bytes)
            with self.assertRaisesRegex(ValueError, 'REGULAR_FILE'):
                snapshot_from_files(config, catalog, root / 'missing.json')
            empty = encoded(dict(schema='relink-skin-catalog-v1', seasons=[], skins=[]))
            (root / 'catalog_extensions.json').write_bytes(empty)
            result = snapshot_from_files(config, catalog)
            self.assertEqual(result['catalog_extensions_sha256'], hashlib.sha256(empty).hexdigest())

    def test_cli_only_writes_requested_new_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config, catalog, output = root / 'config.json', root / 'catalog.json', root / 'run.json'
            config.write_bytes(encoded(self.config()))
            catalog.write_bytes(self.catalog_bytes)
            command = [sys.executable, '-I', '-B', '-X', 'utf8', str(ROOT / 'tests/manual/collection_run_config.py'),
                       '--config', str(config), '--catalog', str(catalog), '--output', str(output)]
            process = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=20)
            self.assertEqual(process.returncode, 0, process.stderr)
            result = json.loads(process.stdout)
            self.assertTrue(result['passed'])
            self.assertEqual(result['enabled_count'], 2)
            self.assertEqual(result['snapshot_sha256'], hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(config.read_bytes(), encoded(self.config()))
            self.assertEqual(catalog.read_bytes(), self.catalog_bytes)
            self.assertEqual(set(root.iterdir()), {config, catalog, output})

    def test_existing_selected_card_matcher_consumes_frozen_current_rule(self):
        packet_path = ROOT / 'artifacts/m2_savedvalue_collection/live_condition_s_v2.json'
        if not packet_path.exists():
            self.skipTest('Local historical selected-card observation absent.')
        from collection_candidate import match_selected_first_card
        packet = json.loads(packet_path.read_text(encoding='utf-8'))['steps'][-1]['result']
        config = self.config()
        config['tasks'][0].update(condition='成色S', minPrice=10, maxPrice=229, maxWear=5, quantity=0)
        rule = self.snapshot(config)['rows'][0]
        self.assertFalse(match_selected_first_card(packet, rule)['eligible'])
        config['tasks'][0]['maxPrice'] = 230
        rule = self.snapshot(config)['rows'][0]
        self.assertTrue(match_selected_first_card(packet, rule)['eligible'])
        self.assertEqual(rule['import_source']['fields']['最高价格设置栏'], 600)


if __name__ == '__main__':
    unittest.main(verbosity=2)
