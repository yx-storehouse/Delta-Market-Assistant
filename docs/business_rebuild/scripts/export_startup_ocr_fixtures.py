"""Export an allowlisted text/geometry projection, never sample code or images.

The adjacent normalized OCR record supplies its declared coordinate space; no
extra DPI scaling is guessed here. Legacy page
labels are retained for comparison, NOT used as classifier truth. Some legacy
labels describe settings screens incorrectly; expectations are reviewed anchors.
"""
from __future__ import annotations
import ast
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
DEST = ROOT / 'docs/business_rebuild/implementation/runtime/fixtures/startup_pages.json'
P1 = 'BBZ_20260924_090240.log'
P2 = 'BBZ_20260924_142356.log'
P3 = 'BBZ_20260930_155706.log'
P4 = 'BBZ_20260930_113211.log'
P6 = 'BBZ_20260928_104709.log'
CASES = [
    ('p1_lobby', P1, 11, 'lobby', 'none'),
    ('p1_warehouse', P1, 14, 'warehouse', 'none'),
    ('p1_mandel', P1, 20, 'mandel', 'none'),
    ('p1_skin_home', P1, 23, 'skin_home', 'none'),
    ('p1_transition', P1, 26, 'unknown', 'none'),
    ('p1_empty_watchlist_1', P1, 29, 'empty_watchlist', 'none'),
    ('p1_empty_watchlist_2', P1, 32, 'empty_watchlist', 'none'),
    ('p1_home_after_empty', P1, 35, 'skin_home', 'none'),
    ('p1_catalog_filter', P1, 38, 'catalog_filter', 'none'),
    ('p1_skin_list', P1, 61, 'skin_listings', 'none'),
    ('p1_watch_list', P1, 1795, 'watchlist_listings', 'none'),
    ('p2_home', P2, 11, 'skin_home', 'none'),
    ('p2_empty_1', P2, 17, 'empty_watchlist', 'none'),
    ('p2_empty_2', P2, 20, 'empty_watchlist', 'none'),
    ('p2_home_after_empty', P2, 23, 'skin_home', 'none'),
    ('p2_filter', P2, 26, 'catalog_filter', 'none'),
    ('p3_list_filter_open', P3, 11, 'skin_listings', 'listing_filter'),
    ('p3_home', P3, 34, 'skin_home', 'none'),
    ('p3_empty', P3, 40, 'empty_watchlist', 'none'),
    ('p3_filter', P3, 49, 'catalog_filter', 'none'),
    ('p4_settings_2880', P4, 11, 'game_settings', 'none'),
    ('p4_sparse_lobby', P4, 23, 'unknown', 'none'),
    ('p4_mandel_2880', P4, 26, 'mandel', 'none'),
    ('p4_home_2880', P4, 29, 'skin_home', 'none'),
    ('p4_empty_2880', P4, 35, 'empty_watchlist', 'none'),
    ('p4_filter_2880', P4, 47, 'catalog_filter', 'none'),
    ('p6_settings_mislabeled', P6, 56, 'game_settings', 'none'),
]
LABELS = (
    '开始游戏', '仓库', '特战干员', '部门', '交易行', '特勤处', '改枪台', '行前备战', '进入特勤处',
    '装备价值', '口袋', '安全箱', '背包', '全部移出', '曼德尔砖', '典藏外观', '典藏挂饰',
    '当季产出', '往季产出', '开启', '拥有：', '当前市场流通量', '流通量：', '成交均价',
    '我的关注', '暂未添加任何关注', '典藏', '筛选', '赛季', '全部赛季', '查看', '已拥有', '未拥有',
    '品阶', '传说品阶', '史诗品阶', '稀有品阶', '普通品阶', '枪种', '步枪', '冲锋枪',
    '霰弹枪', '轻机枪', '精准射手步枪', '狙击步枪', '手枪', '确认', '确定', '取消', '返回',
    '不限公示期', '公示中', '默认排序', '按稀有度升序', '相似皮肤', '成色', '价格区间',
    '购买', '售出', '在售', '分辨率', '显示模式', '局内帧数上限', '视频', '应用',
)


def selected(text):
    return len(text) <= 100 and not re.search(r'\d{12,}|UID|世界|战术联盟', text) and (
        any(label in text for label in LABELS) or re.fullmatch(r'第\d+页', text))


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    cases = []
    hashes = {}
    for filename in sorted({case[1] for case in CASES}):
        path = ROOT / 'BBZPS/log' / filename
        hashes[filename] = digest(path)
        maximum = max(case[2] for case in CASES if case[1] == filename) + 2
        rows, offset = {}, 0
        with path.open('rb') as stream:
            for number in range(1, maximum + 1):
                raw = stream.readline()
                rows[number] = (offset, raw)
                offset += len(raw)
        for identifier, name, line, expected, modal in CASES:
            if name != filename:
                continue
            offset, raw = rows[line]
            normalized_offset, normalized_raw = rows[line + 1]
            body = raw.decode('utf-8-sig').split(' - ', 2)[-1]
            raw_words = json.loads(body[body.index('[{'):])
            normalized_text = normalized_raw.decode('utf-8-sig').split('当前页面识别结果:', 1)[1]
            normalized = ast.literal_eval(normalized_text.strip())
            assert normalized and len(normalized) == len(raw_words)
            region = normalized[0]['region']
            assert region[0:2] == (0, 0) and all(item['region'] == region for item in normalized)
            words = []
            for item, original in zip(normalized, raw_words):
                assert item['words'] == original['words'].strip()
                if not selected(item['words']):
                    continue
                x1, y1, x2, y2 = item['box']
                words.append(dict(text=item['words'], x=x1, y=y1, width=x2-x1, height=y2-y1, score=original['score']))
            label_text = rows[line+2][1].decode('utf-8-sig')
            label = label_text.split('当前页面:', 1)[1].strip() if '当前页面:' in label_text else None
            cases.append(dict(id=identifier, expected_page=expected, expected_modal=modal,
                legacy_page_label=label,
                source=dict(file='BBZPS/log/'+filename, line=line, byte_offset=offset, byte_length=len(raw),
                    line_sha256=hashlib.sha256(raw).hexdigest(), file_sha256=hashes[filename],
                    normalized_line=line+1, normalized_byte_offset=normalized_offset,
                    normalized_line_sha256=hashlib.sha256(normalized_raw).hexdigest()),
                observation=dict(width=region[2], height=region[3], coverage='full_client', words=words)))
    cases.sort(key=lambda case: [item[0] for item in CASES].index(case['id']))
    data = dict(schema_version=1, source_kind='historical_ocr_projection', pixels_included=False,
        live_game_test=False, expectations='reviewed_visual_anchors_not_legacy_labels', cases=cases)
    DEST.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    assert json.loads(DEST.read_text(encoding='utf-8')) == data
    print(f'STARTUP_OCR_FIXTURES=PASS; cases={len(cases)}; image_files=0; sample_executed=false')
    for case in cases:
        print(case['id'], case['observation']['width'], case['observation']['height'], len(case['observation']['words']), case['legacy_page_label'])


if __name__ == '__main__':
    main()
