"""One hidden, read-only diagnostic process per foreground session.

Only JSONL protocol envelopes cross this pipe; image bytes stay in memory.
A timeout, malformed envelope or broken pipe poisons the client permanently.
The caller must finish its session instead of silently replaying a request in
a new process. No module import launches a process or performs OS input.
"""
import json
import math
from pathlib import Path
import queue
import subprocess
import threading
import time


class CaptureWorkerError(RuntimeError):
    pass


class CaptureWorkerClient:
    MAX_REQUEST_BYTES = 65536
    MAX_RESPONSE_BYTES = 32 * 1024 * 1024
    MAX_STDERR_BYTES = 8192

    def __init__(self, executable, *, startup_arguments=('--live-capture-server',),
                 process_factory=subprocess.Popen, close_timeout=3.0):
        self.executable = Path(executable).resolve()
        self.startup_arguments = tuple(startup_arguments)
        self.process_factory = process_factory
        self.close_timeout = close_timeout
        self.process = None
        self._stdout_thread = self._stderr_thread = None
        self._writer_thread = None
        self._responses = queue.Queue(maxsize=2)
        self._stderr = bytearray()
        self._lock = threading.Lock()
        self._closed = False
        self._poisoned = False
        self._request_count = self._completed_count = 0
        self._pid = self._exit_status = None
        self._last_roundtrip_ms = None
        self._start_count = 0

    def _emit(self, value):
        # A compliant service has at most one in-flight response. More than
        # two unsolicited packets are corruption, not unbounded memory growth.
        try:
            self._responses.put_nowait(value)
        except queue.Full:
            self._poisoned = True

    def _read_stdout(self):
        try:
            while True:
                line = self.process.stdout.readline(self.MAX_RESPONSE_BYTES + 1)
                if not line:
                    self._emit(('error', 'COLLECTION_CAPTURE_WORKER_EOF'))
                    return
                if len(line) > self.MAX_RESPONSE_BYTES or not line.endswith(b'\n'):
                    self._emit(('error', 'COLLECTION_CAPTURE_WORKER_RESPONSE_LIMIT'))
                    return
                self._emit(('response', line))
        except (OSError, ValueError):
            self._emit(('error', 'COLLECTION_CAPTURE_WORKER_READ'))

    def _read_stderr(self):
        try:
            while True:
                chunk = self.process.stderr.read(4096)
                if not chunk:
                    return
                self._stderr.extend(chunk)
                del self._stderr[:-self.MAX_STDERR_BYTES]
        except (OSError, ValueError):
            return

    def _start(self):
        if self.process is not None:
            return
        self.process = self.process_factory(
            [str(self.executable), *self.startup_arguments],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=-1, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self._start_count += 1
        self._pid = self.process.pid
        self._stdout_thread = threading.Thread(target=self._read_stdout,
            name='collection-capture-stdout', daemon=True)
        self._stderr_thread = threading.Thread(target=self._read_stderr,
            name='collection-capture-stderr', daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()

    def alive(self):
        """False once this client can no longer serve a request (closed,
        poisoned, process exited or unsolicited output queued)."""
        if self._closed or self._poisoned:
            return False
        if self.process is None:
            return True
        return self.process.poll() is None and self._responses.empty()

    def start(self):
        """Start the hidden service early (its OCR helpers warm up); no request is sent."""
        if not self._lock.acquire(blocking=False):
            raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_CONCURRENT_REQUEST')
        try:
            if self._closed or self._poisoned:
                raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_CLOSED')
            self._start()
        finally:
            self._lock.release()

    def capture(self, command, timeout):
        if type(timeout) not in (float, int) or not math.isfinite(timeout) or timeout <= 0:
            raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_TIMEOUT_OPTION')
        if (not isinstance(command, (tuple, list)) or len(command) < 2
                or not all(isinstance(value, str) for value in command)
                or Path(command[0]).resolve() != self.executable):
            raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_EXECUTABLE')
        if not self._lock.acquire(blocking=False):
            raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_CONCURRENT_REQUEST')
        try:
            if self._closed or self._poisoned:
                raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_CLOSED')
            request_id = self._request_count + 1
            raw = json.dumps(dict(id=request_id, arguments=list(command[1:])),
                ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode('utf-8') + b'\n'
            if len(raw) > self.MAX_REQUEST_BYTES:
                raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_REQUEST_LIMIT')
            started = time.monotonic()
            try:
                self._start()
                if self.process.poll() is not None or not self._responses.empty():
                    raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_UNEXPECTED_OUTPUT')
                self._request_count = request_id
                written = queue.Queue(maxsize=1)
                def write_request():
                    try:
                        self.process.stdin.write(raw)
                        self.process.stdin.flush()
                        written.put(None)
                    except (OSError, ValueError) as error:
                        written.put(error)
                # A wedged worker must not block the coordinator in a pipe
                # write, even if a maximum-size request exceeds pipe capacity.
                self._writer_thread = threading.Thread(target=write_request,
                    name='collection-capture-stdin', daemon=True)
                self._writer_thread.start()
                remaining = timeout - (time.monotonic() - started)
                if remaining <= 0:
                    raise queue.Empty()
                write_error = written.get(timeout=remaining)
                if write_error is not None:
                    raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_WRITE')
                remaining = timeout - (time.monotonic() - started)
                if remaining <= 0:
                    raise queue.Empty()
                kind, payload = self._responses.get(timeout=remaining)
                if kind != 'response':
                    raise CaptureWorkerError(payload)
                envelope = json.loads(payload)
                if (self._poisoned or not isinstance(envelope, dict)
                        or set(envelope) != {'id', 'exit_status', 'result'}
                        or type(envelope.get('id')) is not int
                        or envelope['id'] != request_id
                        or type(envelope.get('exit_status')) is not int
                        or envelope['exit_status'] not in (0, 1, 2)
                        or not isinstance(envelope.get('result'), dict)):
                    raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_PROTOCOL')
                self._last_roundtrip_ms = (time.monotonic() - started) * 1000
                self._completed_count += 1
                # The packet is unchanged. The session applies the same age,
                # frame, page, input-ownership and journal checks as one-shot.
                return subprocess.CompletedProcess(command, envelope['exit_status'],
                    json.dumps(envelope['result'], ensure_ascii=False, allow_nan=False).encode('utf-8'),
                    bytes(self._stderr))
            except queue.Empty as error:
                self._poisoned = True
                self.close(force=True)
                # No payload in this exception: it can be serialized safely.
                raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_TIMEOUT') from error
            except BaseException as error:
                self._poisoned = True
                self.close(force=True)
                if isinstance(error, (CaptureWorkerError, KeyboardInterrupt, SystemExit)):
                    raise
                raise CaptureWorkerError('COLLECTION_CAPTURE_WORKER_PROTOCOL') from error
        finally:
            self._lock.release()

    def close(self, *, force=False):
        if self._closed:
            return
        self._closed = True
        process = self.process
        if process is None:
            return
        try:
            if force and process.poll() is None:
                process.terminate()
            if process.stdin is not None:
                try:
                    process.stdin.close()
                except OSError:
                    pass
            try:
                process.wait(timeout=self.close_timeout)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=self.close_timeout)
            self._exit_status = process.returncode
        finally:
            for reader in (self._writer_thread, self._stdout_thread, self._stderr_thread):
                if reader is not None:
                    reader.join(timeout=self.close_timeout)
            for pipe in (process.stdout, process.stderr):
                if pipe is not None:
                    pipe.close()

    def metadata(self):
        return dict(mode='persistent_jsonl', worker_start_count=self._start_count,
                    worker_pid=self._pid, request_count=self._request_count,
                    completed_count=self._completed_count, closed=self._closed,
                    poisoned=self._poisoned, exit_status=self._exit_status,
                    last_roundtrip_ms=self._last_roundtrip_ms,
                    timeout_policy='terminal_no_restart_no_replay', image_file_writes=0)
