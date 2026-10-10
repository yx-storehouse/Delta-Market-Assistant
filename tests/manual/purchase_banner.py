"""Top banner while a timed purchase waits, and the per-grade purchase count.

User 2026-10-09 (screenshot of the original program): "购买：橙色0/10 紫皮2/10
蓝皮0/10 购买延迟：833 倒计时监控：0分9秒" — "在购买等待的时候显示这个，左边的收
藏osd不用显示"; then "限购不要了无需在意你就做一个已抢多少多少就行了". Grades as in
the catalogue: 传说品阶 = 橙色, 史诗品阶 = 紫皮, 稀有品阶 = 蓝皮.

已抢 counts the purchases recorded as bought today (local date) in a ledger
next to the program's config. No purchase limit is applied.
"""
import datetime
import json
import math
from pathlib import Path
import re
import threading
import time
import unicodedata

GRADES = (('传说品阶', 'orange', '橙色'), ('史诗品阶', 'purple', '紫皮'), ('稀有品阶', 'blue', '蓝皮'))
LEDGER_NAME = 'purchase_ledger.jsonl'


def _squash(text):
    return re.sub(r'[\s\-－—一·]', '', unicodedata.normalize('NFKC', text or ''))


def grade_of(title, snapshot):
    """The catalogue grade of a listing title from the configured tasks, or None."""
    key = _squash(title)
    grades = {row.get('game_grade') for row in snapshot.get('rows', []) if key and _squash(row.get('product_name')) == key}
    grades.discard(None)
    return grades.pop() if len(grades) == 1 else None


def ledger_path(config_path):
    return Path(config_path).parent / LEDGER_NAME


def ledger_counts(path, day=None):
    """Bought purchases per grade key on `day` (default: today, local)."""
    day = day or datetime.date.today().isoformat()
    counts = {key: 0 for _, key, _ in GRADES}
    path = Path(path)
    if not path.is_file():
        return counts
    for line in path.read_text(encoding='utf-8').splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if entry.get('outcome') == 'bought' and str(entry.get('time', ''))[:10] == day and entry.get('grade_key') in counts:
            counts[entry['grade_key']] += 1
    return counts


def ledger_append(path, entry):
    entry = dict(entry, time=datetime.datetime.now().astimezone().isoformat(timespec='seconds'))
    with open(path, 'a', encoding='utf-8') as handle:
        handle.write(json.dumps(entry, ensure_ascii=False) + '\n')
    return entry


def grade_key(grade):
    return next((key for name, key, _ in GRADES if name == grade), None)


def display_seconds(remaining_ms):
    """What the game's countdown shows: whole seconds up to the zero."""
    if remaining_ms is None:
        return None
    return max(0, int(math.ceil(remaining_ms / 1000.0 - 1e-9)))


def banner_text(counts, delay_ms, remaining_ms, rehearsal):
    """购买：已抢 橙色0 紫皮2 蓝皮0 购买延迟：830 倒计时监控：0分9秒"""
    parts = ['%s%d' % (label, counts[key]) for _, key, label in GRADES]
    shown = display_seconds(remaining_ms)
    countdown = '--' if shown is None else '%d分%d秒' % (shown // 60, shown % 60)
    delay = ('%d' % delay_ms) if float(delay_ms).is_integer() else ('%.1f' % delay_ms)
    return '%s：已抢 %s 购买延迟：%s 倒计时监控：%s' % ('购买(演练)' if rehearsal else '购买', ' '.join(parts), delay, countdown)


class BannerTicker:
    """Refreshes the banner from text_fn() every interval while it runs;
    best effort, never raises into the purchase."""

    def __init__(self, overlay, text_fn, interval=.2):
        self.overlay, self.text_fn, self.interval = overlay, text_fn, interval
        self._stop = threading.Event()
        self._thread = None
        self.last = None
        self.errors = 0

    def start(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._run, name='purchase-banner', daemon=True)
            self._thread.start()
        return self

    def _run(self):
        while not self._stop.is_set():
            try:
                text = self.text_fn()
                if text and text != self.last:
                    self.overlay.show(text)
                    self.last = text
            except Exception:
                self.errors += 1
            self._stop.wait(self.interval)

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(1.0)
            self._thread = None
