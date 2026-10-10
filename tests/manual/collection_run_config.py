"""Freeze current GUI tasks into a data-only collection run; never touch the game."""
from __future__ import annotations

import argparse
from collections import defaultdict
from decimal import Decimal, InvalidOperation
import hashlib
import json
import os
from pathlib import Path
import re
import unicodedata

MAX_CONFIG_BYTES = 8 * 1024 * 1024
MAX_CATALOG_BYTES = 4 * 1024 * 1024
CONDITIONS = {'成色S', '成色A', '成色B', '成色C', '仅磨损', '不限'}
PRODUCT_ID = re.compile(r'(?:[1-9][0-9]{0,17}|user:[a-zA-Z0-9][a-zA-Z0-9._-]{0,79})\Z')
HASH = re.compile(r'[0-9a-f]{64}\Z')


def _fail(code):
    raise ValueError(code)


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            _fail('RUN_CONFIG_DUPLICATE_JSON_KEY')
        result[key] = value
    return result


def _load(data, limit, code):
    if not isinstance(data, bytes) or not 0 < len(data) <= limit:
        _fail(code + '_SIZE')
    try:
        result = json.loads(data.decode('utf-8-sig'), object_pairs_hook=_pairs,
                            parse_float=Decimal, parse_constant=lambda _: _fail(code + '_NUMBER'))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError(code + '_JSON') from error
    if not isinstance(result, dict):
        _fail(code + '_ROOT')
    return result


def _text(value, code, maximum=4096, allow_empty=False):
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        _fail(code)
    if len(value.encode('utf-16-le')) // 2 > maximum or value != value.strip():
        _fail(code)
    if any(unicodedata.category(c) in ('Cc', 'Cs') or c in '\u2028\u2029' for c in value):
        _fail(code)
    return value


def _number(value, places, maximum, code):
    if isinstance(value, bool) or not isinstance(value, (int, Decimal)):
        _fail(code)
    number = Decimal(value)
    if not number.is_finite() or number < 0 or number > maximum:
        _fail(code)
    # Match the GUI codec's tolerance for binary-double representation tails,
    # not meaningful extra precision. Keep the exact literal separately below.
    scale = Decimal(10) ** places
    try:
        rounded = number.quantize(Decimal(1) / scale)
    except InvalidOperation as error:
        raise ValueError(code) from error
    tolerance = max(Decimal('1e-8') / scale,
                    abs(number) * Decimal('2.220446049250313e-16') * 4)
    if abs(number - rounded) > tolerance:
        _fail(code)
    if rounded == 0:
        return '0'
    result = format(rounded, 'f')
    return result.rstrip('0').rstrip('.') if '.' in result else result


def _integer(value, low, high, code):
    if isinstance(value, bool) or not isinstance(value, (int, Decimal)):
        _fail(code)
    if not Decimal(value).is_finite() or value < low or value > high or int(value) != value:
        _fail(code)
    return int(value)


def _normalized(value):
    # Only established label normalization; no fuzzy names or character repair.
    return re.sub(r'\s+', '', unicodedata.normalize('NFKC', value))


# The game's 品阶 labels a catalogue entry may carry ('' = not recorded).
GRADES = ('传说品阶', '史诗品阶', '稀有品阶')
# v1 files recorded the original menu colour instead (user, 2026-10-09).
GRADE_BY_MENU_COLOR = dict(orange='传说品阶', red='传说品阶', purple='史诗品阶', blue='稀有品阶', unknown='')


def _catalog(data):
    root = _load(data, MAX_CATALOG_BYTES, 'RUN_CATALOG')
    if set(root) != {'schema', 'seasons', 'skins'} or root['schema'] not in (
            'relink-skin-catalog-v2', 'relink-skin-catalog-v1'):
        _fail('RUN_CATALOG_SCHEMA')
    legacy = root['schema'] == 'relink-skin-catalog-v1'
    if not isinstance(root['seasons'], list) or not isinstance(root['skins'], list):
        _fail('RUN_CATALOG_ARRAYS')
    if len(root['seasons']) > 999 or len(root['skins']) > 20000:
        _fail('RUN_CATALOG_COUNT')
    seasons, products = {}, {}
    for season in root['seasons']:
        if not isinstance(season, dict) or set(season) != {'id', 'label'}:
            _fail('RUN_CATALOG_SEASON')
        sid = _text(season['id'], 'RUN_CATALOG_SEASON_ID', 160)
        label = _text(season['label'], 'RUN_CATALOG_SEASON_LABEL', 160)
        if not re.fullmatch(r'S[1-9][0-9]{0,2}', sid) or sid in seasons:
            _fail('RUN_CATALOG_SEASON_ID')
        seasons[sid] = label
    required = {'product_id', 'season_id', 'season_label', 'weapon'} | (
        {'menu_color'} if legacy else set())
    optional = {'skin_series', 'variant_label', 'thumbnail_path', 'display_name'} | (
        {'game_quality_name'} if legacy else {'grade'})
    for original in root['skins']:
        if not isinstance(original, dict) or not required <= set(original) or set(original) - required - optional:
            _fail('RUN_CATALOG_PRODUCT_FIELDS')
        product = {k: _text(original[k], 'RUN_CATALOG_PRODUCT_TEXT', 320 if k == 'display_name' else 160)
                   for k in required}
        product.update({k: _text('' if original.get(k) is None else original[k], 'RUN_CATALOG_PRODUCT_TEXT',
                                 320 if k == 'display_name' else 160, True)
                        for k in optional})
        pid = product['product_id']
        if not PRODUCT_ID.fullmatch(pid) or pid in products:
            _fail('RUN_CATALOG_PRODUCT_ID')
        if seasons.get(product['season_id']) != product['season_label']:
            _fail('RUN_CATALOG_PRODUCT_SEASON')
        if legacy:
            if product['menu_color'] not in GRADE_BY_MENU_COLOR:
                _fail('RUN_CATALOG_MENU_COLOR')
            # The old dialog saved free text here (same rule as the C++ reader):
            # a text naming a 品阶 wins, anything else falls back to the colour.
            named = product.pop('game_quality_name')
            named = named if named.endswith('品阶') else named + '品阶'
            colour = GRADE_BY_MENU_COLOR[product.pop('menu_color')]
            product['grade'] = named if named in GRADES else colour
        if product['grade'] not in GRADES + ('',):
            _fail('RUN_CATALOG_GRADE')
        if product['variant_label'] not in {'', '极品', '优品'} or product['thumbnail_path']:
            _fail('RUN_CATALOG_PRODUCT_METADATA')
        # "S6|AUG 突击步枪 - 天命" identifies the product (and the game title
        # it must show). It follows from the other fields: a missing or
        # differently written name is rebuilt, exactly as the C++ reader does
        # (canonicalizeDisplayName), so both sides agree on every file.
        parts = product['display_name'].split('|')
        series = product['skin_series']
        variant = product['variant_label']
        suffix = series + (' - ' if series and variant else '') + variant
        expected = product['weapon'] + (' - ' + suffix if suffix else '')
        if (len(parts) != 2 or parts[0].strip() != product['season_id']
                or _normalized(parts[1]) != _normalized(expected)):
            product['display_name'] = product['season_id'] + '|' + expected
        if len(product['display_name']) > 320:
            _fail('RUN_CATALOG_DISPLAY_CONFLICT')
        products[pid] = product
    return seasons, products


def _provenance(value):
    if value is None:
        return None
    if not isinstance(value, dict) or set(value) != {
            'format', 'sourceSha256', 'dictionarySha256', 'productId', 'conditionId', 'row', 'fields'}:
        _fail('RUN_IMPORT_SOURCE_FIELDS')
    if value['format'] != 'savedValue' or any(not isinstance(value[k], str) or not HASH.fullmatch(value[k])
            for k in ('sourceSha256', 'dictionarySha256')):
        _fail('RUN_IMPORT_SOURCE_HASH')
    row = _integer(value['row'], 0, 499, 'RUN_IMPORT_SOURCE_ROW')
    fields = value['fields']
    numeric = {'成色设置栏', '最低价格设置栏', '最高价格设置栏', '枪名设置栏', '磨损度设置栏', '限量设置栏'}
    if not isinstance(fields, dict) or set(fields) != numeric | {'启用'} or type(fields['启用']) is not bool:
        _fail('RUN_IMPORT_SOURCE_RAW')
    fields = dict(fields)
    for key in numeric:
        fields[key] = _integer(fields[key], 0, 9007199254740991, 'RUN_IMPORT_SOURCE_RAW')
    if (value['productId'] != str(fields['枪名设置栏'])
            or value['conditionId'] != str(fields['成色设置栏'])
            or fields['最低价格设置栏'] > fields['最高价格设置栏']):
        _fail('RUN_IMPORT_SOURCE_CONFLICT')
    return dict(value, row=row, fields=fields)


def build_run_snapshot(config_bytes: bytes, catalog_bytes: bytes, extension_bytes: bytes | None = None):
    """Pure preparation; uses current Task values, never historical import fields.

    row_index is the position in the current configuration, not the old source
    row. source_row_index/import_source retain that old identity separately.
    Nonzero quantity is retained; this module does not run a quantity counter.
    """
    config = _load(config_bytes, MAX_CONFIG_BYTES, 'RUN_CONFIG')
    if type(config.get('schema_version')) is not int or config['schema_version'] != 1 or config.get('demo') is not False:
        _fail('RUN_CONFIG_NOT_REAL_V1')
    if not isinstance(config.get('skins'), list) or not isinstance(config.get('tasks'), list):
        _fail('RUN_CONFIG_ARRAYS')
    if len(config['skins']) > 10000 or len(config['tasks']) > 10000:
        _fail('RUN_CONFIG_COUNT')
    seasons, products = _catalog(catalog_bytes)
    if extension_bytes is not None:
        added_seasons, added_products = _catalog(extension_bytes)
        if any(sid in seasons and seasons[sid] != label for sid, label in added_seasons.items()):
            _fail('RUN_CATALOG_EXTENSION_SEASON_CONFLICT')
        if products.keys() & added_products.keys():
            _fail('RUN_CATALOG_EXTENSION_PRODUCT_CONFLICT')
        seasons.update(added_seasons)
        products.update(added_products)
        if len(seasons) > 999 or len(products) > 20000:
            _fail('RUN_CATALOG_COUNT')
    labels = defaultdict(list)
    for product in products.values():
        labels[(product['season_id'], _normalized(product['display_name'].split('|')[1]))].append(product['product_id'])
    config_skins, config_products = {}, set()
    for skin in config['skins']:
        if not isinstance(skin, dict):
            _fail('RUN_CONFIG_SKIN')
        sid = _text(skin.get('id'), 'RUN_CONFIG_SKIN_ID')
        if sid in config_skins:
            _fail('RUN_CONFIG_DUPLICATE_SKIN')
        pid = skin.get('catalogProductId')
        if pid:
            if not isinstance(pid, str) or pid in config_products:
                _fail('RUN_CONFIG_DUPLICATE_PRODUCT')
            config_products.add(pid)
        config_skins[sid] = skin
    task_ids, source_rows, rows = set(), set(), []
    for index, task in enumerate(config['tasks']):
        if not isinstance(task, dict) or type(task.get('enabled')) is not bool:
            _fail('RUN_TASK_ENABLED')
        tid = _text(task.get('id'), 'RUN_TASK_ID')
        if tid in task_ids:
            _fail('RUN_TASK_DUPLICATE_ID')
        task_ids.add(tid)
        origin = _provenance(task.get('importSource'))
        if origin:
            origin_key = (origin['sourceSha256'], origin['row'])
            if origin_key in source_rows:
                _fail('RUN_TASK_DUPLICATE_SOURCE_ROW')
            source_rows.add(origin_key)
        if not task['enabled']:
            continue
        name = _text(task.get('name'), 'RUN_TASK_NAME', 60)
        sid = _text(task.get('skinId'), 'RUN_TASK_SKIN_ID')
        skin = config_skins.get(sid)
        if skin is None or not skin.get('catalogProductId') or skin.get('dataSource') == 'test_fixture':
            _fail('RUN_TASK_CATALOG_LINK_MISSING')
        product = products.get(skin['catalogProductId'])
        if product is None:
            _fail('RUN_TASK_PRODUCT_UNKNOWN')
        if sid != 'catalog:' + product['product_id']:
            _fail('RUN_TASK_PRODUCT_ID_CONFLICT')
        expected = {'name': product['display_name'], 'series': product['season_id'] + ' · ' + product['season_label'],
                    'rarity': product['grade'] or '待核对', 'variant': product['variant_label'],
                    'skinSeries': product['skin_series']}
        legacy_projection = (skin.get('rarity') == '待核对' and skin.get('menuColor') in GRADE_BY_MENU_COLOR
                             and GRADE_BY_MENU_COLOR[skin['menuColor']] == product['grade'])
        if any(skin.get(key) != value for key, value in expected.items()
               if not (key == 'rarity' and legacy_projection)):
            _fail('RUN_TASK_CATALOG_PROJECTION_CONFLICT')
        product_name = _normalized(product['display_name'].split('|')[1])
        if len(labels[(product['season_id'], product_name)]) != 1:
            _fail('RUN_TASK_PRODUCT_LABEL_AMBIGUOUS')
        condition = task.get('condition')
        if not isinstance(condition, str) or condition not in CONDITIONS:
            _fail('RUN_TASK_CONDITION_UNKNOWN')
        minimum = _number(task.get('minPrice'), 2, Decimal(999999999), 'RUN_TASK_MIN_PRICE')
        maximum = _number(task.get('maxPrice'), 2, Decimal(999999999), 'RUN_TASK_MAX_PRICE')
        wear = _number(task.get('maxWear'), 6, Decimal(100), 'RUN_TASK_WEAR')
        if Decimal(minimum) > Decimal(maximum):
            _fail('RUN_TASK_PRICE_RANGE')
        quantity = _integer(task.get('quantity'), 0, 9999, 'RUN_TASK_QUANTITY')
        rows.append(dict(row_index=index, task_id=tid, task_name=name, enabled=True,
            dictionary_resolved=True, product_id=product['product_id'], source_product_id=product['product_id'],
            display_name=product['display_name'], product_name=product_name, season_id=product['season_id'],
            season_label=_normalized(product['season_label']), season_label_display=product['season_label'],
            condition_label=condition, price_min=minimum, price_max=maximum, max_wear=wear,
            limit_raw=quantity, quantity_semantics='unlimited' if quantity == 0 else 'confirmed_collection_limit',
            ownership='any', grade='any', game_grade=product['grade'], variant_label=product['variant_label'],
            source_row_index=origin['row'] if origin else None,
            import_source=origin,
            current_value_literals={key: str(task[key]) for key in ('minPrice', 'maxPrice', 'maxWear', 'quantity')}))
    if not rows:
        _fail('RUN_CONFIG_NO_ENABLED_TASKS')
    config_hash = hashlib.sha256(config_bytes).hexdigest()
    return dict(schema='collection-run-snapshot-v1', valid=True, ready=True, source_kind='current_config',
        mode='collect_only', purchase_phase_enabled=False, run_settings_applied=False,
        source_sha256=config_hash, config_sha256=config_hash,
        catalog_sha256=hashlib.sha256(catalog_bytes).hexdigest(),
        catalog_extensions_sha256=hashlib.sha256(extension_bytes).hexdigest() if extension_bytes is not None else None,
        row_index_basis='current_config_task_array', config_task_count=len(config['tasks']),
        disabled_task_count=len(config['tasks']) - len(rows), row_count=len(rows), enabled_count=len(rows),
        product_count=len({r['product_id'] for r in rows}), rows=rows,
        grade_inferred_from_color=False, game_input_sent=False)


def _read(path, limit):
    path = Path(path)
    if not path.is_file() or path.is_symlink():
        _fail('RUN_INPUT_NOT_REGULAR_FILE')
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if not 0 < len(data) <= limit:
        _fail('RUN_INPUT_SIZE')
    return data


def snapshot_from_files(config_path, catalog_path, extension_path=None):
    """Read-only; auto-load the config-adjacent catalog_extensions.json if present."""
    config_path, catalog_path = Path(config_path), Path(catalog_path)
    implicit_extension = config_path.parent / 'catalog_extensions.json'
    extension = Path(extension_path) if extension_path is not None else implicit_extension
    if extension_path is not None or extension.exists():
        extension_bytes = _read(extension, MAX_CATALOG_BYTES)
    else:
        extension_bytes = None
    result = build_run_snapshot(_read(config_path, MAX_CONFIG_BYTES), _read(catalog_path, MAX_CATALOG_BYTES), extension_bytes)
    result['source_paths'] = dict(config=str(config_path.resolve()), catalog=str(catalog_path.resolve()),
                                  extensions=str(extension.resolve()) if extension_bytes is not None else None)
    return result


def write_run_snapshot(snapshot, output_path):
    """Publish once to an explicitly chosen path; never replace an existing file.

    The snapshot records input hashes; subsequent source edits intentionally do
    not change this run. Output is immutable by this API (exclusive creation).
    """
    if snapshot.get('schema') != 'collection-run-snapshot-v1' or snapshot.get('ready') is not True:
        _fail('RUN_SNAPSHOT_NOT_READY')
    path = Path(output_path)
    resolved = path.resolve()
    if any(value and Path(value).resolve() == resolved for value in snapshot.get('source_paths', {}).values()):
        _fail('RUN_SNAPSHOT_WOULD_REPLACE_INPUT')
    data = (json.dumps(snapshot, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')
    # An interrupted write is never returned as success; the caller freezes a
    # snapshot before launching its runner and verifies the returned byte hash.
    with path.open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--catalog', required=True, type=Path)
    parser.add_argument('--extensions', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = snapshot_from_files(args.config, args.catalog, args.extensions)
    sha256 = write_run_snapshot(result, args.output)
    print(json.dumps(dict(passed=True, output=str(args.output.resolve()), snapshot_sha256=sha256,
        config_sha256=result['config_sha256'], enabled_count=result['enabled_count'],
        product_count=result['product_count'], game_input_sent=False), ensure_ascii=False))


if __name__ == '__main__':
    main()
