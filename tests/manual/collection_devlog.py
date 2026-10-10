"""Development log for hotkey collection runs: what happened and why it stopped.

All output is local; nothing leaves the machine.

- artifacts/logs/YYYY-MM-DD.log: one readable line per runner lifecycle
  event, trial event and session step (each capture attempt with its error
  codes, failing regions and retry flags, timings). Written by a background
  thread so collection never waits on the disk; days older than KEEP_DAYS
  are removed.
- <run>/failure/: written when a run stops for any reason other than the
  user's own stop: failure.txt (readable summary), failure.json (the failing
  step in full, the steps and events before it, backend/OCR/capture
  metadata) and, while the game is still in front, one read-only screenshot
  (failure_preview.jpg) plus the price/title/catalog crops of the last read.
- `python collection_devlog.py --latest` (or a run directory) prints the
  summary for any recorded run; `--tail N` prints today's last log lines.
"""
from __future__ import annotations

import argparse
import base64
from collections import Counter
import datetime
import io
import json
import re
from pathlib import Path
import queue
import sys
import threading
import traceback

from collection_paths import project_root

ROOT = project_root(__file__)
LOGS = ROOT / 'artifacts/logs'
RUNS = ROOT / 'artifacts/hotkey_runs'
KEEP_DAYS = 14
LINE_LIMIT = 2000


def _short(value, limit=240):
    if isinstance(value, float):
        text = ('%.3f' % value).rstrip('0').rstrip('.')
    elif isinstance(value, (dict, list, tuple)):
        text = json.dumps(value, ensure_ascii=False, separators=(',', ':'), default=str)
    else:
        text = str(value)
    text = text.replace('\r', ' ').replace('\n', ' | ')
    return text if len(text) <= limit else text[:limit - 1] + '…'


def format_line(now, level, source, message, fields):
    extras = ' '.join('%s=%s' % (key, _short(value)) for key, value in fields.items()
                      if value is not None and value != '' and value != [] and value != {})
    line = '%s %-5s [%s] %s%s' % (now.strftime('%H:%M:%S.%f')[:-3], level, source, message,
                                  ' ' + extras if extras else '')
    return line[:LINE_LIMIT] + '\n'


class DevLog:
    """Append-only daily text log; every failure to write is swallowed."""

    def __init__(self, directory=LOGS, *, source='runner', clock=datetime.datetime.now, keep_days=KEEP_DAYS,
                 background=True):
        self.directory = Path(directory)
        self.source = source
        self.clock = clock
        self.keep_days = keep_days
        self.context = None
        self.errors = 0
        self._pruned = False
        self._queue = queue.Queue() if background else None
        self._thread = None
        if background:
            self._thread = threading.Thread(target=self._run, name='collection-devlog', daemon=True)
            self._thread.start()

    def path(self, day=None):
        return self.directory / ('%s.log' % (day or self.clock()).strftime('%Y-%m-%d'))

    def write(self, level, message, **fields):
        now = self.clock()
        source = self.source if not self.context else '%s %s' % (self.source, self.context)
        line = format_line(now, level, source, message, fields)
        if self._queue is not None:
            self._queue.put((now, line))
        else:
            self._append([(now, line)])

    def info(self, message, **fields):
        self.write('INFO', message, **fields)

    def warn(self, message, **fields):
        self.write('WARN', message, **fields)

    def error(self, message, **fields):
        self.write('ERROR', message, **fields)

    def exception(self, message, error, **fields):
        self.write('ERROR', message, error=str(error) or type(error).__name__, **fields)
        for text in traceback.format_exception(type(error), error, error.__traceback__):
            for part in text.rstrip().splitlines():
                self.write('TRACE', part)

    def _append(self, batch):
        try:
            self.directory.mkdir(parents=True, exist_ok=True)
            by_day = {}
            for now, line in batch:
                by_day.setdefault(self.path(now), []).append(line)
            for path, lines in by_day.items():
                with path.open('a', encoding='utf-8') as output:
                    output.writelines(lines)
            if not self._pruned:
                self._pruned = True
                self._prune(batch[-1][0])
        except OSError:
            self.errors += 1

    def _prune(self, now):
        cutoff = (now - datetime.timedelta(days=self.keep_days)).strftime('%Y-%m-%d')
        for path in self.directory.glob('????-??-??*.log'):
            if path.name[:10] < cutoff:
                try:
                    path.unlink()
                except OSError:
                    pass

    def _run(self):
        while True:
            items = [self._queue.get()]
            while True:
                try:
                    items.append(self._queue.get_nowait())
                except queue.Empty:
                    break
            batch = [item for item in items if isinstance(item, tuple)]
            if batch:
                self._append(batch)
            for item in items:
                if isinstance(item, threading.Event):
                    item.set()
            if any(item is None for item in items):
                return

    def flush(self, timeout=3.0):
        """Wait until everything queued so far is on disk (the writer keeps running)."""
        if self._queue is None or self._thread is None:
            return True
        marker = threading.Event()
        self._queue.put(marker)
        return marker.wait(timeout)

    def close(self, timeout=3.0):
        if self._queue is not None and self._thread is not None:
            self._queue.put(None)
            self._thread.join(timeout)
            self._thread = None


# ----------------------------------------------------------------- summaries

def _region_errors(result):
    regions = (result.get('collection_observation') or {}).get('regions') or []
    return ['%s:%s' % (r.get('kind'), r.get('error')) for r in regions
            if isinstance(r, dict) and r.get('ok') is False and r.get('error')]


def attempt_summary(attempt):
    result = attempt.get('result') or {}
    codes = [code for code in dict.fromkeys((result.get('error'), result.get('page_error'), result.get('local_title_error'),
             result.get('collection_geometry_error'), result.get('collection_receipt_error'))) if code]
    # Every retry flag (collection_*, capture_acquire, catalog_filter), without its prefix.
    flags = [re.sub(r'^(collection_|capture_|catalog_)', '', key[:-len('_retryable')]) for key, value in attempt.items()
             if key.endswith('_retryable') and value]
    return dict(passed=bool(attempt.get('passed')), exit=attempt.get('exit_status'),
                ms=attempt.get('capture_roundtrip_ms'), codes=codes, regions=_region_errors(result),
                retry=flags, page=(result.get('startup_page') or {}).get('page'))


def step_fields(index, step):
    """Compact, readable fields of one session step record."""
    fields = dict(ms=(step.get('timings') or {}).get('total_ms'))
    request = step.get('step') or {}
    if step.get('error'):
        fields['error'] = step['error']
    if request.get('expected_page'):
        fields['page'] = request['expected_page']
    bar = ((step.get('result') or {}).get('collection_layout') or {}).get('scrollbar')
    if isinstance(bar, dict) and bar.get('track_bounds') and bar.get('thumb_bounds'):
        # Scroll planning matches these against measured profiles.
        track, thumb = bar['track_bounds'], bar['thumb_bounds']
        fields['bar'] = 'x%s track%s+%s thumb%s+%s' % (track[0], track[1], track[3], thumb[1], thumb[3])
    if step.get('point'):
        fields['point'] = step['point']
    if request.get('key'):
        fields['key'] = request['key']
    motion = step.get('motion') or {}
    if motion.get('elapsed_ms') is not None:
        fields['motion_ms'] = motion.get('elapsed_ms')
    candidate = step.get('candidate') or step.get('collection_attempt')
    if isinstance(candidate, dict):
        fields['candidate'] = {key: candidate.get(key) for key in ('condition', 'price', 'wear', 'eligible')
                               if candidate.get(key) is not None}
    if step.get('reason'):
        fields['reason'] = step['reason']
    if step.get('receipt_mode'):
        fields['receipt'] = step['receipt_mode']
    settlements = step.get('receipt_settlements') or []
    if settlements:
        # Step records keep the receipt job (kind, result, outcome); session
        # outcomes and layout rechecks are flat {mode, layout_unchanged}.
        settled = []
        for record in settlements:
            if not isinstance(record, dict):
                continue
            outcome = record.get('outcome') if isinstance(record.get('outcome'), dict) else record
            text = '%s:%s' % (outcome.get('mode') or record.get('kind'),
                              'same' if outcome.get('layout_unchanged') else 'changed')
            if outcome.get('layout_recheck') is not None:
                text += '/recheck:%s' % ('same' if outcome['layout_recheck'] else 'changed')
            if record.get('request_roundtrip_ms') is not None:
                text += '/%dms' % round(record['request_roundtrip_ms'])
            settled.append(text)
        fields['settled'] = settled
    attempts = step.get('capture_attempts') or []
    if attempts:
        parts = []
        for number, attempt in enumerate(attempts, 1):
            summary = attempt_summary(attempt)
            text = 'a%d:%s' % (number, 'ok' if summary['passed'] else 'FAIL')
            if summary['ms'] is not None:
                text += '/%dms' % round(summary['ms'])
            if summary['codes']:
                text += ' ' + ','.join(summary['codes'])
            if summary['regions']:
                text += ' {' + ','.join(summary['regions']) + '}'
            if summary['retry']:
                text += ' retry=' + '+'.join(summary['retry'])
            parts.append(text)
        fields['attempts'] = ' ; '.join(parts)
    return fields


def step_level(step):
    return 'INFO' if step.get('status') == 'finished' and step.get('passed') is not False else 'WARN'


def event_fields(event):
    fields = {key: event.get(key) for key in ('row', 'reason', 'error', 'delta', 'shift', 'season', 'product',
                                                'game_grade', 'ownership', 'toggled',
                                                'condition', 'minimum', 'maximum', 'wear_max', 'detail')
              if event.get(key) is not None}
    rejections = [item.get('reason') for item in event.get('candidate_rejections') or [] if isinstance(item, dict)]
    if rejections:
        fields['rejected'] = ','.join('%s×%d' % item for item in Counter(rejections).most_common())
    candidate = event.get('candidate')
    if isinstance(candidate, dict):
        fields.update({key: candidate.get(key) for key in ('price', 'wear', 'condition', 'eligible')
                       if candidate.get(key) is not None})
    if event.get('receipt_mode'):
        fields['receipt'] = event['receipt_mode']
    return fields


# ------------------------------------------------------------ failure dossier

def _strip_images(value):
    if isinstance(value, dict):
        return {key: _strip_images(item) for key, item in value.items()
                if not (isinstance(key, str) and (key.endswith('_base64') or key in ('png', 'pixels')))}
    if isinstance(value, list):
        return [_strip_images(item) for item in value]
    return value


def capture_failure_preview(session, backend, timeout=10):
    """One read-only full capture with a preview while the lease is still held.

    Sends no input. Returns (packet without images, jpeg bytes or None, error).
    """
    try:
        if not backend.foreground():
            return None, None, 'game_not_in_front'
        command = session._command(dict(kind='capture', collection_observation=True, collection_layout=True,
                                        collection_numeric_price=True)) + ['--preview-stdout']
        process = backend.capture(command, timeout)
        packet = json.loads(process.stdout)
        pixels = packet.pop('preview_png_base64', None)
        packet = _strip_images(packet)
        jpeg = None
        if pixels:
            from PIL import Image
            image = Image.open(io.BytesIO(base64.b64decode(pixels))).convert('RGB')
            image.thumbnail((1920, 1080))
            stream = io.BytesIO()
            image.save(stream, format='JPEG', quality=82)
            jpeg = stream.getvalue()
        return packet, jpeg, None
    except Exception as error:
        return None, None, str(error) or type(error).__name__


def write_failure_dossier(run_dir, *, error, session=None, backend=None, trial=None, events=(), preview=None,
                          hotkey='F2', extra=None):
    """Write <run>/failure/; never raises. Returns the folder or None."""
    try:
        folder = Path(run_dir) / 'failure'
        folder.mkdir(parents=True, exist_ok=True)
        steps = list(getattr(session, 'steps', None) or [])
        failing = next((step for step in reversed(steps) if step.get('status') == 'failed'), steps[-1] if steps else None)
        files = []
        crops = {}
        for name in ('last_price_image', 'last_title_image', 'last_catalog_image'):
            image = getattr(session, name, None)
            if isinstance(image, dict) and image.get('png_base64'):
                target = folder / (name.replace('last_', '').replace('_image', '') + '.png')
                target.write_bytes(base64.b64decode(image['png_base64']))
                files.append(target.name)
                crops[target.name] = {key: value for key, value in image.items() if key != 'png_base64'}
        preview_packet, preview_jpeg, preview_error = preview if preview else (None, None, 'not_captured')
        if preview_jpeg:
            (folder / 'failure_preview.jpg').write_bytes(preview_jpeg)
            files.append('failure_preview.jpg')
        metadata = {}
        if backend is not None:
            try:
                metadata = backend.metadata()
            except Exception as caught:
                metadata = dict(error=str(caught))
        document = dict(schema='collection-failure-dossier-v1', error=str(error), hotkey=hotkey,
            written=datetime.datetime.now().astimezone().isoformat(),
            failing_step_index=steps.index(failing) + 1 if failing in steps else None,
            failing_step=_strip_images(failing),
            previous_steps=[dict(index=len(steps) - offset, kind=step.get('kind'), status=step.get('status'),
                                 **step_fields(len(steps) - offset, step))
                            for offset, step in reversed(list(enumerate(reversed(steps[-25:-1]), 1)))],
            events_tail=_strip_images(list(events)[-40:]),
            session=dict(error=(getattr(session, 'report', {}) or {}).get('error'),
                         pending_collection=getattr(session, 'pending', None) is not None,
                         pending_geometry=_strip_images(getattr(session, 'pending_geometry', None)),
                         receipt_settlements=_strip_images(list(getattr(session, 'receipt_settlements', []))[-10:]),
                         steps=len(steps)),
            trial=dict(status=(getattr(trial, 'summary', {}) or {}).get('status'),
                       current_row_index=(getattr(trial, 'summary', {}) or {}).get('current_row_index'),
                       completed_rows=len((getattr(trial, 'summary', {}) or {}).get('rows', []))),
            backend=_strip_images(metadata), crops=crops,
            preview=dict(error=preview_error, packet=preview_packet), files=files, extra=extra or {})
        (folder / 'failure.json').write_text(json.dumps(document, ensure_ascii=False, indent=2, default=str), 'utf-8')
        (folder / 'failure.txt').write_text(summarize_run(Path(run_dir), dossier=document, hotkey=hotkey), 'utf-8')
        return folder
    except Exception:
        return None


# ------------------------------------------------------------------ diagnosis

def _load(path):
    try:
        return json.loads(Path(path).read_text('utf-8'))
    except (OSError, ValueError):
        return None


def summarize_run(run_dir, *, dossier=None, hotkey='F2', last_steps=10):
    """Readable account of one recorded hotkey run (with or without a dossier)."""
    from collection_status import stop_reason
    run_dir = Path(run_dir)
    lines = []
    result = _load(run_dir / 'result.json') or {}
    lines.append('收藏运行 %s' % run_dir.name)
    if result:
        error = result.get('error')
        reason = stop_reason(error, hotkey)
        if error == 'COLLECTION_STOP_REQUESTED' and result.get('stop_reason') == 'collection_deadline':
            reason = '第一位快解锁，先回去买'
        lines.append('结果：%s%s' % (result.get('status'), ('，原因 %s（%s）' % (error, reason)) if error else ''))
        lines.append('本次新收藏 %s 把；已完成任务 %s 条；用时 %.1f 秒' % (
            result.get('new_favorites'), result.get('completed_rules'), (result.get('elapsed_ms') or 0) / 1000))
    events = []
    events_path = run_dir / 'events.jsonl'
    if events_path.is_file():
        for line in events_path.read_text('utf-8').splitlines():
            try:
                events.append(json.loads(line))
            except ValueError:
                pass
    if events:
        counts = Counter(event.get('event') for event in events)
        lines.append('事件：' + '，'.join('%s×%d' % item for item in counts.most_common(8)))
        # A stop raised outside a screen read (scroll planning, coverage) has
        # its reasons only in the event that reported it.
        failed = [event for event in events if event.get('error') and event.get('event') != 'trial_stopped']
        if failed:
            event = failed[-1]
            rejections = Counter(item.get('reason') for item in event.get('candidate_rejections') or []
                                 if isinstance(item, dict))
            lines.append('出错事件：%s error=%s%s' % (event.get('event'), event.get('error'), (
                '；被拒原因 ' + '，'.join('%s×%d' % item for item in rejections.most_common())) if rejections else ''))
    steps = []
    for segment in sorted(run_dir.glob('segment_*')):
        for path in sorted((segment / 'session.steps').glob('*.json')):
            step = _load(path)
            if step:
                steps.append(step)
    if steps:
        by_kind = {}
        for step in steps:
            total = (step.get('timings') or {}).get('total_ms')
            if total is not None:
                by_kind.setdefault(step.get('kind'), []).append(total)
        lines.append('步骤 %d 个；平均耗时：%s' % (len(steps), '，'.join(
            '%s %.0fms' % (kind, sum(values) / len(values)) for kind, values in by_kind.items())))
        retries = Counter()
        failures = Counter()
        for step in steps:
            for attempt in step.get('capture_attempts') or []:
                summary = attempt_summary(attempt)
                if not summary['passed']:
                    failures.update(summary['codes'] + summary['regions'] or ['(no code)'])
                retries.update(summary['retry'])
        if failures:
            lines.append('读图失败（含已重试成功的）：' + '，'.join('%s×%d' % item for item in failures.most_common(10)))
        if retries:
            lines.append('触发的重读：' + '，'.join('%s×%d' % item for item in retries.most_common()))
        lines.append('最后 %d 步：' % min(last_steps, len(steps)))
        for index, step in list(enumerate(steps, 1))[-last_steps:]:
            fields = step_fields(index, step)
            lines.append('  #%d %s %s %s' % (index, step.get('kind'), step.get('status'),
                         ' '.join('%s=%s' % (key, _short(value, 400)) for key, value in fields.items() if value not in (None, ''))))
    dossier = dossier or _load(run_dir / 'failure/failure.json')
    if dossier:
        lines.append('故障记录：failure/ （%s）' % '，'.join(dossier.get('files') or ['无图片']))
        preview = (dossier.get('preview') or {})
        if preview.get('error'):
            lines.append('失败后截图：未取得（%s）' % preview['error'])
        packet = preview.get('packet') or {}
        if packet:
            # The same screen read again right after the stop: tells whether
            # the failure was a one-off read or a lasting screen state.
            regions = (packet.get('collection_observation') or {}).get('regions') or []
            fields = next((r for r in regions if isinstance(r, dict) and r.get('kind') == 'card_fields'), {})
            lines.append('失败后重读：页面 %s，读图%s%s；选中卡字段 %s' % (
                (packet.get('startup_page') or {}).get('page'), '通过' if packet.get('capture_passed') else '未通过',
                ('（%s）' % ','.join(_region_errors(packet) or [packet.get('local_title_error') or packet.get('page_error') or '']))
                if not packet.get('capture_passed') else '',
                _short([w.get('text') for w in fields.get('words', []) if isinstance(w, dict)], 200)))
        failing = dossier.get('failing_step') or {}
        for number, attempt in enumerate(failing.get('capture_attempts') or [], 1):
            result_packet = attempt.get('result') or {}
            local = result_packet.get('local_price_ocr') or {}
            if local:
                lines.append('  a%d 本地价格识别：texts=%s scores=%s error=%s' % (
                    number, local.get('raw_texts'), local.get('scores'), local.get('error')))
            selected = result_packet.get('collection_selected_card') or {}
            if selected:
                lines.append('  a%d 选中卡：%s' % (number, _short({key: selected.get(key) for key in
                             ('card_id', 'bounds', 'favorite_warm_fraction', 'favorite_bright_fraction')}, 400)))
    return '\n'.join(lines) + '\n'


def latest_run(runs=RUNS):
    candidates = sorted((path for path in Path(runs).glob('20*') if path.is_dir()), reverse=True)
    return candidates[0] if candidates else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run', nargs='?', type=Path, help='Run directory (default: --latest).')
    parser.add_argument('--latest', action='store_true')
    parser.add_argument('--tail', type=int, help="Print the last N lines of today's log instead.")
    parser.add_argument('--steps', type=int, default=10)
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    if args.tail:
        path = DevLog(background=False).path()
        lines = path.read_text('utf-8').splitlines()[-args.tail:] if path.is_file() else []
        print('\n'.join(lines))
        return 0
    run = args.run or latest_run()
    if run is None:
        print('没有收藏运行记录')
        return 1
    print(summarize_run(run, last_steps=args.steps), end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
