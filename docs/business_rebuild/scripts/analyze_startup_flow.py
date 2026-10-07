"""Reconstruct BBZPS startup from local historical text only; no sample imports.

Streams every log to count run entries and bind evidence to exact LF line,
byte offset, byte length, line SHA256 and source SHA256. Only allowlisted UI
anchors and short business events are exported; no OCR full text/credentials.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'BBZPS'
OUTPUT = ROOT / 'docs/business_rebuild/evidence/startup_flow_evidence.json'
EVENTS = {
    'health_begin': '开始首次服务器健康度检查',
    'health_complete': '首次健康检查完成',
    'health_failure': '健康检查失败',
    'fastest_server': '最快服务器:',
    'season': '处理赛季:',
    'season_present': '赛季: .* 存在',
    'ownership': '处理拥有情况:',
    'ownership_noop': '按钮状态正确',
    'quality_clear': '取消选择品质:',
    'quality_required': '需要选择品质:',
    'quality_select': '点击选择品质:',
    'find_begin': '开始查找 .* 个物品',
    'found_product': '在第 .* 页找到物品:',
    'found_position': '^位置:',
    'missing_product': '第 .* 页未找到:',
    'next_page': '翻页继续查找',
    'all_found': '所有武器都已找到',
    'season_complete': '已完成赛季',
    'publicity_match': '公示期匹配',
    'price_parse_failure': '价格解析失败:',
    'price_match': '^价格匹配:',
    'wear_found': '^找到磨损值:',
    'joint_match': '^价格True:.*磨损True:',
    'joint_mismatch': '^价格(?:True|False):.*不符合条件',
    'overprice_end': '剩余皮肤价格高于设置价格本次收藏完毕',
    'watchlist_full': '已达关注上限',
    'sync_begin': '开始校准时间',
    'sync_result': '^成功从 .* 个服务器获取时间',
    'system_time_set': '超过阈值，正在设置系统时间',
    'system_time_preserve': '在允许范围内，不调整系统时间',
    'countdown_first': '^首次检测到剩余',
    'countdown_correction': '^修正剩余时间',
    'trajectory_open': '^轨迹运行打开页面',
    'confirm_first': '^点击第1次确认',
    'confirm_second': '^已点击二次确认',
    'purchase_timeout': '^购买时间过长',
    'result_timeout': '^购买结果公示过长',
    'round_finished': '^购买全部结束',
    'market_closed': '^当前时间市场未开放',
    'invalid_recognition': '^识别结果无效',
    'capture_failure': '^截图时出现异常',
    'stop_collect': '^停止收藏',
}
PATTERNS = {key: re.compile(value) for key, value in EVENTS.items()}
ANCHORS = ['曼德尔砖', '典藏外观', '典藏挂饰', '我的关注', '暂未添加任何关注的皮肤',
           '取消全部关注', '全部取消关注', '清空关注', '成功添加至我的关注', '已达关注上限',
           '该商品已被购买或已下架', '获得枪械外观', '应用外观', '不限公示期',
           '按稀有度升序', '所有成色', '价格区间']
ANCHOR_BYTES = {word: ('"words":"' + word + '"').encode() for word in ANCHORS}


def locator(file, number, offset, raw, message=None):
    result = dict(file=file, line=number, byte_offset=offset, byte_length=len(raw),
                  line_sha256=hashlib.sha256(raw).hexdigest())
    if message is not None:
        result['message'] = message
    result['timestamp'] = raw[:23].decode('ascii', errors='replace')
    return result


def extract():
    witnesses = {name: [] for name in EVENTS}
    anchors = {word: [] for word in ANCHORS}
    sessions, files = [], []
    counts = Counter()
    for path in sorted((SOURCE / 'log').glob('*.log')):
        relative = path.relative_to(SOURCE).as_posix()
        digest = hashlib.sha256()
        offset, number, active, last_health = 0, 0, None, None
        with path.open('rb', buffering=1024 * 1024) as stream:
            for number, raw in enumerate(stream, 1):
                digest.update(raw)
                start = offset
                offset += len(raw)
                # Business log text is UTF-8; decode only short events or a
                # bounded number of selected anchor-bearing OCR observations.
                if b' - ' not in raw:
                    continue
                body = raw.split(b' - ', 2)[-1].rstrip(b'\r\n')
                if len(body) < 600 and not body.startswith((b'(', b'[', b'{')):
                    message = body.decode('utf-8-sig', errors='replace')
                    if '识别结果:' in message or '识别结果：' in message:
                        continue
                    if '开始首次服务器健康度检查' in message:
                        last_health = locator(relative, number, start, raw, message)
                    if message.startswith('开始执行:'):
                        active = dict(start=locator(relative, number, start, raw, message),
                                      preceding_health=last_health, entry_page=None, early_events=[])
                        sessions.append(active)
                    if message.startswith('当前页面:'):
                        page = message.split(':', 1)[1].strip()
                        if active and active['entry_page'] is None:
                            active['entry_page'] = page
                    if active and number <= active['start']['line'] + 120 and len(active['early_events']) < 60:
                        if re.search('^当前页面:|^处理赛季|^处理拥有|^取消选择品质|^需要选择品质|^点击选择品质|^开始查找|^开始校准|^购买全部结束', message):
                            active['early_events'].append(locator(relative, number, start, raw, message))
                    for key, pattern in PATTERNS.items():
                        if pattern.search(message):
                            counts[key] += 1
                            if len(witnesses[key]) < 3:
                                witnesses[key].append(locator(relative, number, start, raw, message))
                elif body.startswith(b'(') and b'[{"location"' in body:
                    wanted = [word for word, pattern in ANCHOR_BYTES.items()
                              if len(anchors[word]) < 3 and pattern in body]
                    if wanted:
                        split = body.index(b'[{')
                        try:
                            observations = json.loads(body[split:])
                        except ValueError:
                            continue
                        for word in wanted:
                            evidence = locator(relative, number, start, raw)
                            evidence['roi_expression'] = body[:split].decode('ascii', errors='replace')
                            evidence['anchors'] = [item for item in observations if item.get('words') == word]
                            anchors[word].append(evidence)
        files.append(dict(file=relative, bytes=offset, lf_lines=number, sha256=digest.hexdigest()))
        print(f'SCANNED={relative}; lines={number}; bytes={offset}', flush=True)
    return dict(method='static_text_only', sample_executed=False, source_files=files,
                run_entry_count=len(sessions), run_types=dict(Counter(s['start']['message'] for s in sessions)),
                first_page_distribution=dict(Counter(s['entry_page'] or '<no_page_record>' for s in sessions)),
                sessions=sessions, event_counts=dict(counts), event_witnesses=witnesses, anchor_witnesses=anchors)


def verify(data):
    checked = 0
    evidence = [item for values in data['event_witnesses'].values() for item in values]
    evidence += [item for values in data['anchor_witnesses'].values() for item in values]
    for session in data['sessions']:
        evidence.append(session['start'])
        if session['preceding_health']:
            evidence.append(session['preceding_health'])
        evidence += session['early_events']
    for item in evidence:
        with (SOURCE / item['file']).open('rb') as stream:
            stream.seek(item['byte_offset'])
            raw = stream.read(item['byte_length'])
        assert hashlib.sha256(raw).hexdigest() == item['line_sha256'], item
        if 'message' in item:
            assert item['message'] in raw.decode('utf-8-sig'), item
        for anchor in item.get('anchors', []):
            assert json.dumps(anchor['words'], ensure_ascii=False) in raw.decode('utf-8-sig'), item
        checked += 1
    return checked


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    if args.verify_only:
        data = json.loads(OUTPUT.read_text(encoding='utf-8'))
    else:
        data = extract()
        OUTPUT.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    checked = verify(data)
    print(json.dumps(dict(result='PASS', locators_checked=checked, run_entry_count=data['run_entry_count'],
                         first_page_distribution=data['first_page_distribution']), ensure_ascii=False))


if __name__ == '__main__':
    main()
