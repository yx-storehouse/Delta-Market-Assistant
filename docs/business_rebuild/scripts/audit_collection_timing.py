"""Read historical JSON timing records; never capture frames or send game input."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SERIES = 'artifacts/m2_savedvalue_collection/live_series_s6_v5'
DEFAULT_JOURNAL = 'artifacts/m2_savedvalue_collection/journal'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(values):
    if any(isinstance(v, bool) or not isinstance(v, (int, float))
           or not math.isfinite(v) or v < 0 for v in values):
        raise ValueError('TIMING_VALUE_INVALID')
    ordered = sorted(values)
    if not ordered:
        return {'n': 0, 'min': None, 'median': None, 'p95': None, 'max': None}
    return {'n': len(ordered), 'min': ordered[0], 'median': statistics.median(ordered),
            'p95': ordered[math.ceil(len(ordered) * .95) - 1], 'max': ordered[-1]}


def capture_attempts(step):
    """The outer result repeats the last attempt; do not count it twice."""
    if step.get('kind') != 'capture' or step.get('skipped'):
        return []
    attempts = step.get('attempts')
    return attempts if attempts else [step]


def foreground_ms(record):
    """Observed game-foreground intervals, not full batch wall-clock duration."""
    target = record.get('identities', {}).get('target_hwnd')
    transitions = record.get('foreground_transitions', [])
    if target is None or len(transitions) < 2:
        return None
    if transitions[-1]['hwnd'] == target:
        return None  # No measured end; do not invent a duration.
    total = 0
    found = False
    for start, end in zip(transitions, transitions[1:]):
        delta = end['monotonic_ms'] - start['monotonic_ms']
        if delta < 0:
            raise ValueError('FOREGROUND_CLOCK_ORDER')
        if start['hwnd'] == target:
            total += delta
            found = True
    return total if found else None


def record_key(path):
    return str(path).replace('\\', '/').casefold()


def summarize(series: Path, journal: Path):
    if not series.is_dir() or not journal.is_dir():
        raise ValueError('EVIDENCE_DIRECTORY_MISSING')
    paths = sorted(series.glob('live_*.json'), key=lambda p: int(p.stem.split('_')[-1]))
    if not paths:
        raise ValueError('NO_LIVE_RECORDS')
    journals = []
    seen = set()
    for path in sorted(journal.glob('*.json')):
        item = json.loads(path.read_text(encoding='utf-8'))
        if item['key'] in seen:
            raise ValueError('DUPLICATE_JOURNAL_KEY')
        seen.add(item['key'])
        journals.append((path, item))
    batches, observations, sources = [], [], {}
    for path in paths:
        data = json.loads(path.read_text(encoding='utf-8'))
        sources[str(path)] = digest(path)
        batch_observations = []
        for step_index, step in enumerate(data['steps']):
            for attempt_index, attempt in enumerate(capture_attempts(step)):
                result = attempt.get('result') or {}
                sample = {'record': str(path), 'step_index': step_index,
                          'attempt_index': attempt_index, 'passed': attempt.get('passed', False),
                          'capture_ms': [f['capture_ms'] for f in result.get('frames', [])
                                         if 'capture_ms' in f],
                          'observation_age_ms': result.get('ocr', {}).get('frame_age_at_result_ms')}
                batch_observations.append(sample)
        confirmed = []
        for jpath, item in journals:
            origin = item.get('evidence', {}).get('record', '')
            if record_key(origin) == record_key(path.resolve()) and item['status'] in (
                    'confirmed', 'confirmed_reconciliation'):
                confirmed.append(str(jpath))
                sources[str(jpath)] = digest(jpath)
        dispatched = sum(bool(s.get('collection_attempt')) for s in data['steps'])
        if len(confirmed) > dispatched:
            raise ValueError('CONFIRMED_EXCEEDS_DISPATCHED')
        batches.append({'record': str(path), 'passed': data['passed'],
                        'binary_sha256': data.get('binary_sha256'),
                        'observations': len(batch_observations),
                        'foreground_ms': foreground_ms(data),
                        'dispatched_collections': dispatched,
                        'confirmed_new_collections': len(confirmed),
                        'confirming_journals': confirmed})
        observations.extend(batch_observations)
    return {'method': 'historical_json_read_only', 'game_input_sent': False,
            'percentile_method': 'nearest_rank',
            'metric_definitions': {
                'capture_ms': 'Per-frame adapter capture() wall time, not disk I/O.',
                'observation_age_ms': 'Frame source age when all OCR/refinements finish; NOT pure full-frame OCR time.',
                'foreground_ms': 'Observed game foreground residency; excludes pre/post-focus work; sampling 10 ms in this cohort.',
                'confirmed_new_collections': 'Confirmed journal actions attributed to their original input record, not current watchlist size.'},
            'unmeasured': ['single_roi_ms', 'ocr_engine_cold_start_ms', 'whole_batch_wall_ms',
                           'game_render_ready_ms', 'production_executor_throughput'],
            'capture_ms': stats([v for s in observations for v in s['capture_ms']]),
            'observation_age_ms': stats([s['observation_age_ms'] for s in observations
                                         if s['observation_age_ms'] is not None]),
            'foreground_ms': stats([b['foreground_ms'] for b in batches if b['foreground_ms'] is not None]),
            'batches': batches, 'observations': observations, 'source_sha256': sources}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--series', type=Path, default=ROOT / DEFAULT_SERIES)
    parser.add_argument('--journal', type=Path, default=ROOT / DEFAULT_JOURNAL)
    args = parser.parse_args()
    print(json.dumps(summarize(args.series.resolve(), args.journal.resolve()), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
