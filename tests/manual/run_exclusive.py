"""One game run at a time on this desktop session.

Every collection or F2 cycle holds a named mutex while it runs; a second
runner (a duplicate standby, a single run started next to the program's F2,
an agent rehearsal) refuses to start instead of leasing the same game and
pressing in the same dialog (review 2026-10-09). The mutex is released when
the run ends or its process dies (an abandoned mutex is taken over).
Re-entrant within one thread: the cycle's collection runs inside the cycle.
"""
import contextlib
import ctypes
import threading

MUTEX_NAME = 'Local\\RelinkStudioGameRun'
ERROR = 'RUN_ALREADY_ACTIVE'
_WAIT_OBJECT_0, _WAIT_ABANDONED = 0x0, 0x80
_held = threading.local()


class WindowsMutex:
    def __init__(self, name=MUTEX_NAME):
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.CreateMutexW.restype = ctypes.c_void_p
        k.CreateMutexW.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p)
        k.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        k.WaitForSingleObject.restype = ctypes.c_uint32
        k.ReleaseMutex.argtypes = k.CloseHandle.argtypes = (ctypes.c_void_p,)
        self.k, self.name, self.handle = k, name, None

    def acquire(self):
        handle = self.k.CreateMutexW(None, False, self.name)
        if not handle:
            raise RuntimeError('RUN_LOCK_UNAVAILABLE:%d' % ctypes.get_last_error())
        if self.k.WaitForSingleObject(handle, 0) not in (_WAIT_OBJECT_0, _WAIT_ABANDONED):
            self.k.CloseHandle(handle)
            return False
        self.handle = handle
        return True

    def release(self):
        if self.handle:
            self.k.ReleaseMutex(self.handle)
            self.k.CloseHandle(self.handle)
            self.handle = None


@contextlib.contextmanager
def exclusive_run(factory=None):
    """Hold the game for this run; RuntimeError(RUN_ALREADY_ACTIVE) when
    another runner holds it."""
    depth = getattr(_held, 'depth', 0)
    if depth:
        _held.depth = depth + 1
        try:
            yield
        finally:
            _held.depth -= 1
        return
    lock = (factory or WindowsMutex)()
    if not lock.acquire():
        raise RuntimeError(ERROR)
    _held.depth = 1
    try:
        yield
    finally:
        _held.depth = 0
        lock.release()
