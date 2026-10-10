"""Hotkey collection runner: the saved tasks, with the top status banner.

--standby (how RelinkStudio.exe starts it, right when the program opens):
load both recognition engines and start the capture service in advance,
then wait for stdin commands from the program:
    start <foreground hwnd> <hotkey>   begin a run (queued while still loading)
    stop                               stop the run, or cancel a queued start
    EOF (program closed)               stop and exit
Without --standby it performs exactly one run (manual use, tests).

A stop is honoured before the next action, after any in-flight favorite
receipt is settled. Collection only: no purchase action exists here. Started
inside the game, the game stays in front at the end; no IDE is involved.
The same unfinished task file resumes at its first unfinished rule (existing
stars are kept and skipped). Long runs continue in fresh foreground leases at
rule boundaries, so evidence memory stays bounded. Status goes to the banner
and, as JSON lines, to stdout for the program log.
"""
from __future__ import annotations

import argparse
import copy
import datetime
import hashlib
import json
import os
from pathlib import Path
import sys
import threading
import time

from collection_paths import project_root

ROOT = project_root(__file__)
RUNS = ROOT / 'artifacts/hotkey_runs'
SEGMENT_SECONDS = 1800
SEGMENT_STEPS = 2500
ROLLOVER_SECONDS = 900
ROLLOVER_STEPS = 1200
HOTKEYS = ['F%d' % n for n in range(1, 13)]
LOGS = ROOT / 'artifacts/logs'
_OUTPUT = threading.Lock()


class _NullLog:
    """Stand-in until standby/main opens the development log (tests, imports)."""
    context = None

    def write(self, *args, **kwargs):
        pass

    info = warn = error = write

    def exception(self, *args, **kwargs):
        pass

    def flush(self, *args, **kwargs):
        return True

    def close(self, *args, **kwargs):
        pass


_LOG = _NullLog()


def devlog():
    return _LOG


def open_devlog():
    """artifacts/logs/YYYY-MM-DD.log for this runner process (see collection_devlog)."""
    global _LOG
    from collection_devlog import DevLog
    _LOG = DevLog(LOGS, source='runner')
    return _LOG


def emit_json(value, stream=None):
    with _OUTPUT:
        stream = stream or sys.stdout
        stream.write(json.dumps(value, ensure_ascii=False) + '\n')
        stream.flush()


class StopFlag:
    """Callable stop request for one run, with the reason that caused it."""

    def __init__(self):
        self.event = threading.Event()
        self.reason = None

    def request(self, reason):
        if not self.event.is_set():
            self.reason = reason
            self.event.set()

    def __call__(self):
        return self.event.is_set()


def _pipe_peek(fd):
    """peek() -> bytes waiting in the pipe behind fd, or None once its writer closed."""
    import ctypes
    from ctypes import wintypes
    import msvcrt
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                                     ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    handle = msvcrt.get_osfhandle(fd)

    def peek():
        available = wintypes.DWORD()
        if kernel.PeekNamedPipe(handle, None, 0, None, ctypes.byref(available), None):
            return available.value
        return None
    return peek


def read_lines(fd, on_line, on_eof, *, poll_seconds=.05):
    """Command lines from a pipe without ever leaving a read pending on it.

    A blocking ReadFile on the stdin pipe makes C-runtime start-up in this
    process wait behind it: importing numpy (OpenBLAS) stalled until the
    program wrote its next line, i.e. "正在加载识别模型" hung until F2 was
    pressed again. Poll with PeekNamedPipe and read only bytes already there.
    Raw fd reads also leave no buffered stdin object locked at exit.
    """
    try:
        peek = _pipe_peek(fd)
        polling = peek() is not None
    except (OSError, ValueError):
        polling = False
    pending = b''
    while True:
        available = 4096
        if polling:
            available = peek()
            if available is None:
                break
            if not available:
                time.sleep(poll_seconds)
                continue
        try:
            chunk = os.read(fd, min(available, 65536))
        except OSError:
            break
        if not chunk:
            break
        pending += chunk
        while b'\n' in pending:
            line, pending = pending.split(b'\n', 1)
            on_line(line.decode('utf-8', 'replace').strip())
    on_eof()


class CommandChannel:
    """Program -> runner commands; start/stop/close are atomic with taking a run."""

    def __init__(self, on_queued=None, on_cancelled=None):
        self.condition = threading.Condition()
        self.pending = None
        self.running = False
        self.closed = False
        self.stop = None
        self.on_queued = on_queued or (lambda: None)
        self.on_cancelled = on_cancelled or (lambda: None)

    def feed(self, line):
        parts = line.split()
        if not parts:
            return
        if parts[0] == 'start':
            hwnd = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
            key = parts[2] if len(parts) > 2 and parts[2] in HOTKEYS else None
            with self.condition:
                if self.running or self.closed or self.pending is not None:
                    return
                self.pending = (hwnd, key)
                self.condition.notify_all()
            self.on_queued()
        elif parts[0] == 'stop':
            with self.condition:
                if self.running:
                    self.stop.request('hotkey')
                    return
                cancelled, self.pending = self.pending is not None, None
            if cancelled:
                self.on_cancelled()

    def close(self):
        with self.condition:
            self.closed = True
            self.pending = None
            if self.running:
                self.stop.request('parent_closed')
            self.condition.notify_all()

    def take(self):
        """Block for the next start; None once the program has closed."""
        with self.condition:
            while self.pending is None and not self.closed:
                self.condition.wait()
            if self.closed:
                return None
            item, self.pending = self.pending, None
            self.running, self.stop = True, StopFlag()
            return item, self.stop

    def end(self):
        with self.condition:
            self.running, self.stop = False, None


def default_backend(recognizers):
    from run_collection_observed import MemoryReviewBackend
    return MemoryReviewBackend(ROOT, local_title=True, local_price=True, fast_capture=True, ready_stream=True,
                               parallel_local_price=True, return_policy='entry_foreground', recognizers=recognizers)


class BackendPool:
    """One prepared backend kept ahead of the next foreground lease.

    The engines load once. After each take, the next backend (fresh capture
    service, same engines) is prepared in the background after a short delay,
    or at once when the next take needs it. No backend touches the game
    before its session enters.
    """

    def __init__(self, factory=default_backend, refill_delay=20.0):
        self.factory = factory
        self.refill_delay = refill_delay
        self.recognizers = None
        self.spare = None
        self.error = None
        self.closed = False
        self.lock = threading.Lock()
        self.ready = threading.Event()
        self.wake = None
        self.load_ms = None
        self.replaced = 0

    def load(self):
        started = time.monotonic()
        backend = self.factory(None).prepare()
        self.recognizers = backend.recognizers()
        self.spare = backend
        self.load_ms = round((time.monotonic() - started) * 1000)
        self.ready.set()
        return self

    def _refill_later(self):
        self.ready.clear()
        wake = self.wake = threading.Event()

        def build():
            wake.wait(self.refill_delay)
            backend = error = None
            if not self.closed:
                try:
                    backend = self.factory(self.recognizers).prepare()
                except Exception as caught:
                    error = caught
            with self.lock:
                if self.closed and backend is not None:
                    backend.discard()
                    backend = None
                self.spare, self.error = backend, error
            self.ready.set()

        threading.Thread(target=build, name='collection-backend-prepare', daemon=True).start()

    def take(self, on_wait=None):
        if self.wake is not None:
            self.wake.set()
        if not self.ready.is_set() and on_wait is not None:
            on_wait()
        self.ready.wait()
        with self.lock:
            backend, self.spare = self.spare, None
            self.error = None
        if backend is not None and not getattr(backend, 'healthy', lambda: True)():
            # The spare's capture service died while idle (killed, crashed):
            # replace it before the run's first capture, not after it fails.
            backend.discard()
            backend = None
            self.replaced += 1
            if on_wait is not None:
                on_wait()
        if backend is None:
            backend = self.factory(self.recognizers).prepare()
        # F2 cycle started inside the game: a lease never activates it again.
        backend.require_target_in_front = getattr(self, 'require_game_in_front', False)
        self._refill_later()
        return backend

    def close(self):
        self.closed = True
        if self.wake is not None:
            self.wake.set()
        self.ready.wait(10)
        with self.lock:
            spare, self.spare = self.spare, None
        if spare is not None:
            spare.discard()


class Checkpoint:
    """summary.json only when completed rules or status change (not per card)."""

    def __init__(self, path):
        self.path = Path(path)
        self.key = None
        self.writes = 0

    def __call__(self, summary, force=False):
        from run_collection_trial import write_progress
        key = (len(summary.get('rows', [])), summary.get('status'), summary.get('error'),
               summary.get('segment_index'), summary.get('segment_finished'))
        if force or key != self.key:
            write_progress(self.path, summary)
            self.key = key
            self.writes += 1


def new_run_directory(runs, now=None):
    now = now or datetime.datetime.now()
    runs.mkdir(parents=True, exist_ok=True)
    stamp = now.strftime('%Y%m%d-%H%M%S')
    for suffix in [''] + ['-%d' % n for n in range(2, 100)]:
        path = runs / (stamp + suffix)
        try:
            path.mkdir()
            return path
        except FileExistsError:
            continue
    raise RuntimeError('COLLECTION_HOTKEY_RUN_DIRECTORY')


def find_resume(runs, snapshot, snapshot_sha256, exclude=None):
    """Most recent run of exactly this snapshot; resumable only if unfinished."""
    from run_collection_trial import validate_resume
    if not runs.is_dir():
        return None
    for directory in sorted((p for p in runs.iterdir() if p.is_dir() and p != exclude), reverse=True):
        path = directory / 'summary.json'
        if not path.is_file():
            continue
        try:
            raw = path.read_bytes()
            previous = json.loads(raw.decode('utf-8'))
        except (OSError, ValueError):
            continue
        if not isinstance(previous, dict) or previous.get('snapshot_sha256') != snapshot_sha256:
            continue
        if previous.get('task_file_fully_completed') or not previous.get('rows'):
            return None
        try:
            validate_resume(snapshot, snapshot_sha256, previous)
        except ValueError:
            return None
        return previous, dict(path=str(path), sha256=hashlib.sha256(raw).hexdigest())
    return None


def step_failure_detail(session):
    """Native error behind a generic BATCH_STEP_FAILED, for the banner and log."""
    steps = getattr(session, 'steps', None) or []
    if not steps:
        return None
    step = steps[-1]
    attempts = step.get('capture_attempts') or []
    result = (attempts[-1].get('result') if attempts else None) or step.get('result') or {}
    if not isinstance(result, dict):
        return step.get('reason') or None
    regions = (result.get('collection_observation') or {}).get('regions') or []
    region = next((r for r in regions if isinstance(r, dict) and r.get('ok') is False and r.get('error')), None)
    parts = [value for value in (result.get('error') or result.get('page_error') or result.get('local_title_error'),
                                 region and '%s:%s' % (region.get('kind'), region['error'])) if value]
    return ' / '.join(dict.fromkeys(parts)) or step.get('reason') or None


def run_collection(args, stop, pool, overlay, *, hotkey, monitor_hwnd=0):
    """One hotkey run over the saved tasks. Returns (exit code, result)."""
    from collections import deque
    from collection_devlog import (capture_failure_preview, event_fields, step_fields, step_level,
                                   write_failure_dossier)
    from collection_status import StatusFormatter, stop_reason
    formatter = StatusFormatter(hotkey=hotkey)
    started = time.monotonic()
    log = devlog()
    recent_events = deque(maxlen=60)
    dossier_written = None

    def status(text, tone='info', log=False):
        overlay.show(text, tone)
        emit_json(dict(type='status', text=text, tone=tone, log=log))

    result = dict(schema='collection-hotkey-run-v1', status='starting', hotkey=hotkey,
                  phase='collection_only', purchase_actions=0, game_image_file_writes=0,
                  config=str(args.config), segments=[], engines_preloaded=pool.recognizers is not None)
    run_dir = None
    exit_code = 1
    overlay.monitor(monitor_hwnd)
    status('收到 %s：开始收藏…' % hotkey, log=True)
    try:
        from collection_run_config import snapshot_from_files, write_run_snapshot
        from run_collection_trial import BUDGET_ERRORS, CollectionTrial, select_scroll_configuration
        from collection_live_session import ForegroundSession
        snapshot = snapshot_from_files(args.config, args.catalog)
        run_dir = new_run_directory(RUNS)
        result['run_directory'] = str(run_dir)
        log.context = run_dir.name
        snapshot_path = run_dir / 'task_snapshot.json'
        snapshot_sha256 = write_run_snapshot(snapshot, snapshot_path)
        result.update(snapshot_sha256=snapshot_sha256, config_sha256=snapshot['config_sha256'],
                      enabled_rules=snapshot['enabled_count'])
        log.info('run begin', hotkey=hotkey, rules=snapshot['enabled_count'], config=snapshot['config_sha256'][:12],
                 snapshot=snapshot_sha256[:12], monitor_hwnd=monitor_hwnd)
        formatter.bind(snapshot['rows'])
        resume = find_resume(RUNS, snapshot, snapshot_sha256, exclude=run_dir)
        resume_doc, resume_source = resume if resume else (None, None)
        if resume_doc is not None:
            done = len(resume_doc['rows'])
            result['resumed_from'] = resume_source
            log.info('resume', completed_rules=done, source=resume_source.get('path'))
            status('同一任务未做完：从第 %d/%d 条继续（前 %d 条已完成）'
                   % (done + 1, snapshot['enabled_count'], done), log=True)
        profile, bank, source = select_scroll_configuration()
        checkpoint = Checkpoint(run_dir / 'summary.json')
        events_file = (run_dir / 'events.jsonl').open('x', encoding='utf-8')
        segment = 0
        trial = None
        grade_fallbacks = set()  # skins not listed under their catalogue grade, kept across segments
        try:
            while True:
                segment += 1
                if stop():
                    raise RuntimeError('COLLECTION_STOP_REQUESTED')
                backend = pool.take(on_wait=lambda: status('正在准备截图识别服务…'))

                def on_step(index, step, segment=segment):
                    log.write(step_level(step), 'step s%d#%d %s %s' % (segment, index, step.get('kind'), step.get('status')),
                              **step_fields(index, step))

                session = ForegroundSession(run_dir / ('segment_%02d' % segment) / 'session.json',
                    backend=backend, timeout_seconds=SEGMENT_SECONDS, max_steps=SEGMENT_STEPS,
                    numeric_price=True, fast_settle=True, async_reports=True,
                    pixel_receipts=True, pipeline_receipts=True, stop_requested=stop, step_listener=on_step)
                log.info('segment begin', index=segment)

                def on_event(event):
                    if stop.reason == 'parent_closed':
                        formatter.stop_text = 'RelinkStudio 已关闭'
                    elif stop.reason == 'collection_deadline':
                        formatter.stop_text = '第一位快解锁，先回去买'
                    events_file.write(json.dumps(event, ensure_ascii=False) + '\n')
                    events_file.flush()
                    recent_events.append(event)
                    if event.get('event') == 'trial_stopped' and event.get('reason') == 'BATCH_STEP_FAILED':
                        event = dict(event, detail=step_failure_detail(session))
                    log.write('WARN' if event.get('event') == 'trial_stopped' else 'INFO',
                              'event ' + str(event.get('event')), **event_fields(event))
                    line = formatter.format(event)
                    if line is not None:
                        status(*line)

                trial = CollectionTrial(session, snapshot, snapshot_path, emit=on_event,
                    snapshot_sha256=snapshot_sha256, resume=resume_doc, resume_source=resume_source,
                    checkpoint=checkpoint, scroll_profile=profile, scroll_profile_bank=bank,
                    scroll_profile_source=source, grade_fallbacks=grade_fallbacks)
                trial.summary.update(session_record=str(session.report_path), event_log=str(run_dir / 'events.jsonl'))
                rows_at_start = len(trial.summary['rows'])
                segment_started = time.monotonic()

                def row_gate(row, trial=trial, session=session, rows_at_start=rows_at_start,
                             segment_started=segment_started):
                    progressed = len(trial.summary['rows']) > rows_at_start
                    return not (progressed and (len(session.steps) >= ROLLOVER_STEPS
                                                or time.monotonic() - segment_started >= ROLLOVER_SECONDS))

                error = None
                preview = None
                try:
                    if stop():
                        backend.discard()
                        raise RuntimeError('COLLECTION_STOP_REQUESTED')
                    with session:
                        overlay.monitor(backend.identities.get('target_hwnd'))
                        log.info('foreground lease', target_hwnd=backend.identities.get('target_hwnd'),
                                 stay_in_game=getattr(backend, 'stay_in_target', None),
                                 integrity=(getattr(backend, '_metadata', None) or {}).get('integrity_levels'))
                        try:
                            trial.run(row_gate=row_gate)
                        except Exception as caught:
                            if str(caught) not in BUDGET_ERRORS and str(caught) != 'COLLECTION_STOP_REQUESTED':
                                # One read-only screenshot while the game is still
                                # leased; no input. Saved with the failure record.
                                preview = capture_failure_preview(session, backend)
                                log.warn('failure preview', captured=preview[1] is not None, error=preview[2])
                            raise
                except Exception as caught:
                    error = caught
                    if str(error) not in BUDGET_ERRORS and str(error) != 'COLLECTION_STOP_REQUESTED':
                        log.exception('segment failed', caught, detail=step_failure_detail(session))
                    trial.stopped(caught)
                trial.summary.update(elapsed_ms=round((time.monotonic() - trial.started) * 1000),
                    ide_restored=bool(session.report.get('ide_restored')), image_file_writes=0,
                    segment_finished=True)
                trial.persist()
                checkpoint(trial.summary, force=True)
                log.info('segment end', index=segment, status=trial.summary['status'], error=trial.summary.get('error'),
                         steps=len(session.steps), new=trial.summary['confirmed_new'],
                         preserved=trial.summary['already_favorited'], unmatched=trial.summary['unmatched'],
                         restored=session.report.get('ide_restored'))
                result['segments'].append(dict(index=segment, status=trial.summary['status'],
                    error=trial.summary.get('error'), confirmed_new=trial.summary['confirmed_new'],
                    already_favorited=trial.summary['already_favorited'], unmatched=trial.summary['unmatched'],
                    steps=len(session.steps), returned_to_entry_window=session.report.get('ide_restored'),
                    stayed_in_game=bool(getattr(backend, 'stay_in_target', False)),
                    session_record=str(session.report_path)))
                if error is None:
                    result['status'] = 'completed'
                    exit_code = 0
                    break
                # A lease that finished no rule cannot roll over again: the
                # same rule would only restart from its top indefinitely.
                rollover = (str(error) in BUDGET_ERRORS and not stop()
                            and len(trial.summary['rows']) > rows_at_start
                            and session.pending is None and session.pending_geometry is None)
                if not rollover:
                    # A stop (F2, or a far head's deadline) that lands on a segment's budget is a stop.
                    stopped = str(error) == 'COLLECTION_STOP_REQUESTED' or (str(error) in BUDGET_ERRORS and stop())
                    result.update(status=trial.summary['status'], error='COLLECTION_STOP_REQUESTED' if stopped else str(error))
                    if stopped and stop.reason == 'collection_deadline':
                        result['status'] = trial.summary['status'] = 'stopped_at_deadline'
                        trial.persist()
                        checkpoint(trial.summary, force=True)
                    exit_code = 3 if stopped else 1
                    if not stopped:
                        dossier_written = write_failure_dossier(run_dir, error=error, session=session, backend=backend,
                            trial=trial, events=recent_events, preview=preview, hotkey=hotkey,
                            extra=dict(detail=step_failure_detail(session), segment=segment))
                    break
                log.info('segment rollover', next_index=segment + 1)
                resume_doc = copy.deepcopy(trial.summary)
                resume_source = dict(path=str(run_dir / 'summary.json'), in_process_segment=segment)
        finally:
            events_file.close()
        if trial is not None:
            result.update(completed_rules=len(trial.summary['rows']), cycle_totals=trial.summary.get('cycle_totals'),
                          next_row_index=trial.summary.get('next_row_index'))
    except Exception as error:
        result.update(status='blocked', error=str(error) or type(error).__name__)
        log.exception('run blocked', error)
        if run_dir is not None and dossier_written is None:
            dossier_written = write_failure_dossier(run_dir, error=error, events=recent_events, hotkey=hotkey)
        status('已停止：' + stop_reason(result['error'], hotkey), 'error', log=True)
        exit_code = 1
    result.update(new_favorites=formatter.new_total, already_favorited_seen=formatter.preserved_total,
                  stop_reason=stop.reason, elapsed_ms=round((time.monotonic() - started) * 1000),
                  overlay=overlay.metadata(), exit_status=exit_code)
    if dossier_written is not None:
        result['failure_record'] = str(dossier_written)
        emit_json(dict(type='status', text='已保存故障记录：%s' % dossier_written, tone='info', log=True))
    if run_dir is not None:
        (run_dir / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', 'utf-8')
    log.write('INFO' if exit_code in (0, 3) else 'ERROR', 'run end', status=result.get('status'),
              error=result.get('error'), new=formatter.new_total, preserved=formatter.preserved_total,
              elapsed_s=round(time.monotonic() - started, 1), failure_record=result.get('failure_record'),
              overlay=result.get('overlay'))
    log.context = None
    log.flush()
    emit_json(dict(type='result', **result))
    return exit_code, result


def run_f2_cycle(args, stop, pool, overlay, *, hotkey, monitor_hwnd=0):
    """F2: the collection serves the purchase (user 2026-10-09). 我的关注 is
    checked first; empty: collect, then back; otherwise buy, listing by
    listing, until stopped (purchase_cycle). A failure ends this run with
    its reason on the banner; the standby runner stays up for the next F2."""
    from purchase_cycle import run_cycle
    from run_exclusive import exclusive_run
    from run_purchase_timed_buy import settings_from, stop_text
    log = devlog()
    summary = dict(status='blocked', kind='cycle')
    try:
        with exclusive_run():
            settings = settings_from(args.config, buy=not getattr(args, 'purchase_rehearsal', False), no_ntp=True)
            log.info('cycle begin', hotkey=hotkey, monitor_hwnd=monitor_hwnd, buy=settings.buy,
                     enter_s=settings.enter_s, delay_ms=settings.delay_ms)
            overlay.monitor(monitor_hwnd)
            summary = run_cycle(args, stop, pool, overlay, hotkey=hotkey, monitor_hwnd=monitor_hwnd,
                                collect=run_collection, runs_dir=RUNS, new_run_directory=new_run_directory, log=log,
                                emit=emit_json, settings=settings)
    except Exception as error:
        reason = str(error) or type(error).__name__
        log.exception('cycle failed', error)
        summary.update(status='blocked', error=reason)
        text = ('另一个收藏/购买正在运行，这次没有开始' if 'RUN_ALREADY_ACTIVE' in reason
                else '收藏+购买没能开始：' + stop_text(reason))
        overlay.show(text, 'error', ttl_ms=10000)
        emit_json(dict(type='status', text=text, tone='error', log=True))
    log.info('cycle end', status=summary.get('status'), error=summary.get('error'),
             attempts=len(summary.get('attempts', [])), collections=len(summary.get('collections', [])))
    emit_json(dict(type='result', kind='cycle', **{k: v for k, v in summary.items() if k not in ('attempts', 'kind')}))
    return (0 if summary.get('status') in ('stopped', 'ended') else 1), summary


def run_exclusively(run, args, stop, pool, overlay, **kw):
    """One game run at a time (run_exclusive); a second start is refused."""
    from run_exclusive import exclusive_run
    try:
        with exclusive_run():
            return run(args, stop, pool, overlay, **kw)
    except RuntimeError as error:
        if str(error) != 'RUN_ALREADY_ACTIVE':
            raise
        text = '另一个收藏/购买正在运行，这次没有开始'
        overlay.show(text, 'error', ttl_ms=10000)
        emit_json(dict(type='status', text=text, tone='error', log=True))
        result = dict(status='blocked', error=str(error))
        emit_json(dict(type='result', **result))
        return 1, result


def default_catalog(script=__file__):
    """The catalogue of this program build: build.ps1 copies the skins.json it
    compiled into RelinkStudio.exe to dist/RelinkStudio/catalog/, next to this
    runtime. Run from the source tree (or before that copy exists), the
    source file. The GUI projects its config from the embedded copy, so both
    sides must read the same bytes (review 2026-10-09)."""
    here = Path(script).resolve().parent
    shipped = here.parent / 'catalog' / 'skins.json'
    if here.name == 'collection' and (here.parent / 'RelinkStudio.exe').is_file() and shipped.is_file():
        return shipped
    return ROOT / 'src/assets/catalog/skins.json'


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, default=default_catalog())
    parser.add_argument('--hotkey', default='F2', choices=HOTKEYS)
    parser.add_argument('--overlay-exe', type=Path, default=ROOT / 'dist/RelinkStudio/RelinkStudio.exe')
    parser.add_argument('--foreground-hwnd', type=int, default=0,
                        help='Single run: window in front when the hotkey was pressed (banner monitor).')
    parser.add_argument('--no-overlay', action='store_true')
    parser.add_argument('--collect-only', action='store_true',
                        help='F2 runs the collection only, without the purchase cycle.')
    parser.add_argument('--purchase-rehearsal', action='store_true',
                        help='Development: the cycle never presses the green button (the F2 of the user buys).')
    parser.add_argument('--standby', action='store_true', help='Preload, then serve start/stop commands on stdin.')
    parser.add_argument('--stdin-control', action='store_true', help='Single run: "stop" line or EOF on stdin stops it.')
    return parser


def standby(args, *, pool=None, stdin_fd=0):
    from collection_status import OverlayChannel
    import os
    log = open_devlog() if isinstance(devlog(), _NullLog) else devlog()
    log.info('standby start', pid=os.getpid(), hotkey=args.hotkey, config=str(args.config),
             python=sys.version.split()[0], script=str(Path(__file__).resolve()))
    # Started while the program is opening: the banner process stays hidden
    # until a run sends text, and lives as long as this runner.
    overlay = OverlayChannel(args.overlay_exe, enabled=not args.no_overlay,
                             extra_arguments=('--idle-exit-seconds', '86400')).start()
    loading = threading.Event()
    loading.set()

    def queued():
        if loading.is_set():
            overlay.show('识别模型还在加载，加载完自动开始收藏…')
            emit_json(dict(type='status', text='识别模型还在加载，加载完自动开始收藏', tone='info', log=True))

    def cancelled():
        overlay.show('已取消开始', 'warn', ttl_ms=3000)
        emit_json(dict(type='status', text='已取消开始', tone='warn', log=True))

    channel = CommandChannel(on_queued=queued, on_cancelled=cancelled)

    def command(line):
        log.info('command', line=line, running=channel.running, pending=channel.pending)
        channel.feed(line)

    def closed():
        log.info('command channel closed (program exit)')
        channel.close()

    threading.Thread(target=read_lines, args=(stdin_fd, command, closed),
                     name='collection-commands', daemon=True).start()
    emit_json(dict(type='state', state='loading'))
    pool = pool or BackendPool()
    try:
        pool.load()
    except Exception as error:
        from collection_status import stop_reason
        log.exception('engine load failed', error)
        text = '识别模型加载失败：' + stop_reason(str(error) or type(error).__name__)
        overlay.show(text, 'error', ttl_ms=8000)
        emit_json(dict(type='status', text=text, tone='error', log=True))
        emit_json(dict(type='state', state='failed', error=str(error)))
        overlay.close()
        return 1
    loading.clear()
    log.info('standby ready', load_ms=pool.load_ms)
    emit_json(dict(type='state', state='ready', load_ms=pool.load_ms))
    emit_json(dict(type='status', text='收藏+购买程序已就绪（识别模型加载 %.1f 秒），在游戏里按 %s 开始'
                   % (pool.load_ms / 1000, args.hotkey), tone='ok', log=True))
    try:
        while True:
            item = channel.take()
            if item is None:
                break
            (hwnd, key), stop = item
            emit_json(dict(type='state', state='running'))
            try:
                run = run_collection if args.collect_only else run_f2_cycle
                run_exclusively(run, args, stop, pool, overlay, hotkey=key or args.hotkey, monitor_hwnd=hwnd)
            except Exception as error:
                log.exception('run crashed', error)
                raise
            finally:
                overlay.linger(10000)
                channel.end()
                emit_json(dict(type='state', state='ready'))
    finally:
        pool.close()
        overlay.close()
        log.info('standby exit')
        log.close()
    return 0


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.standby:
        return standby(args)
    from collection_status import OverlayChannel
    open_devlog().info('single run start', hotkey=args.hotkey, config=str(args.config))
    stop = StopFlag()
    if args.stdin_control:
        threading.Thread(target=read_lines, name='collection-stop-input', daemon=True,
                         args=(0, lambda line: line.lower() == 'stop' and stop.request('hotkey'),
                               lambda: stop.request('parent_closed'))).start()
    overlay = OverlayChannel(args.overlay_exe, enabled=not args.no_overlay,
                             monitor_hwnd=args.foreground_hwnd or None).start()
    pool = BackendPool()
    try:
        try:
            pool.load()
        except Exception as error:
            emit_json(dict(type='result', status='blocked', error=str(error), purchase_actions=0))
            return 1
        run = run_collection if args.collect_only else run_f2_cycle
        code, unused = run_exclusively(run, args, stop, pool, overlay, hotkey=args.hotkey,
                                       monitor_hwnd=args.foreground_hwnd)
        return code
    finally:
        pool.close()
        overlay.close()
        devlog().close()


if __name__ == '__main__':
    raise SystemExit(main())
