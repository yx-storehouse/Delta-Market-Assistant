"""Durable, independently reserved collection attempts with immutable history.

An unfinished attempt is never replayed. A separately observed current white
star can create a new attempt after old confirmed attempts, when explicitly
requested by the caller. Historical record bytes and raw identity keys stay
unchanged; decimal wear normalization is only used when comparing identities.
"""
from contextlib import contextmanager
from copy import deepcopy
from decimal import Decimal, InvalidOperation
from functools import lru_cache
import hashlib
import json
import math
import os
from pathlib import Path
import re
import uuid


def candidate_key(candidate):
    # Keep this serialization stable: existing journal filenames use its hash.
    # Normalized history matching is separate and does not rewrite old keys.
    identity = {k: candidate[k] for k in ('product', 'condition', 'wear')}
    return hashlib.sha256(json.dumps(identity, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def validate_record_identity(record):
    """Return the durable attempt key; reject mismatched legacy/v2 identities."""
    try:
        if not isinstance(record, dict) or not isinstance(record.get('candidate'), dict):
            raise ValueError
        identity = candidate_key(record['candidate'])
        key = record.get('key')
        if record.get('schema') == 'collection-attempt-v2':
            attempt = record.get('attempt_id')
            valid = (record.get('identity_key') == identity and isinstance(attempt, str)
                     and re.fullmatch(r'[0-9a-f]{32}', attempt) is not None
                     and key == identity + '.' + attempt)
        else:
            valid = (record.get('schema') in (None, 'collection-attempt-v1')
                     and 'identity_key' not in record and 'attempt_id' not in record
                     and key == identity)
        if not valid:
            raise ValueError
        return key
    except (KeyError, TypeError, ValueError, OverflowError) as error:
        raise ValueError('COLLECTION_JOURNAL_IDENTITY') from error


def _normalized_identity(candidate):
    try:
        product, condition, wear = (candidate[name] for name in ('product', 'condition', 'wear'))
        if any(not isinstance(value, str) or not value for value in (product, condition, wear)):
            raise ValueError
        number = Decimal(wear)
        if not number.is_finite() or number < 0:
            raise ValueError
        return product, condition, number
    except (KeyError, TypeError, ValueError, InvalidOperation) as error:
        raise ValueError('COLLECTION_JOURNAL_IDENTITY') from error


def _validate_current_white(candidate):
    star = candidate.get('favorite_before', {})
    age = candidate.get('age_at_action_ms')
    warm = star.get('favorite_warm_fraction') if isinstance(star, dict) else None
    bright = star.get('favorite_bright_fraction') if isinstance(star, dict) else None
    frame_id, digest = candidate.get('source_frame_id'), candidate.get('source_frame_sha256')
    finite = lambda value: type(value) in (int, float) and math.isfinite(value)
    if (not isinstance(star, dict) or star.get('selected') is not True
            or not finite(warm) or warm != 0
            or not finite(bright) or not .04 <= bright <= 1
            or candidate.get('eligible') is not True
            or not isinstance(frame_id, str) or not frame_id.strip()
            or not isinstance(digest, str) or re.fullmatch(r'[0-9a-f]{64}', digest) is None
            or not finite(age) or not 0 <= age <= 5000):
        raise ValueError('COLLECTION_JOURNAL_CURRENT_WHITE_REQUIRED')


def _historical_frame_ids(record):
    """Reject reused acquisition IDs, not independently reacquired equal pixels.

    A static screen may hash identically at two valid capture times. Completed
    history is not a content-hash deny list: the current white-star/eligibility
    and five-second checks remain mandatory, as does receipt hash transition.
    """
    frame_ids = set()
    pending = [record.get(name) for name in ('candidate', 'receipt', 'reconciliation')]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            for name, item in value.items():
                if name in ('source_frame_id', 'frame_id') and isinstance(item, str) and item:
                    frame_ids.add(item)
                elif isinstance(item, (dict, list)):
                    pending.append(item)
        elif isinstance(value, list):
            pending.extend(value)
    return frame_ids


@lru_cache(maxsize=1)
def _windows_metadata_api():
    """Read-only file metadata; initialization failure disables cache hits."""
    import ctypes
    from ctypes import wintypes

    class BasicInfo(ctypes.Structure):
        _fields_ = [('CreationTime', ctypes.c_longlong), ('LastAccessTime', ctypes.c_longlong),
                    ('LastWriteTime', ctypes.c_longlong), ('ChangeTime', ctypes.c_longlong),
                    ('FileAttributes', wintypes.DWORD)]

    class HandleInfo(ctypes.Structure):
        _fields_ = [('FileAttributes', wintypes.DWORD), ('CreationTime', wintypes.FILETIME),
                    ('LastAccessTime', wintypes.FILETIME), ('LastWriteTime', wintypes.FILETIME),
                    ('VolumeSerialNumber', wintypes.DWORD), ('FileSizeHigh', wintypes.DWORD),
                    ('FileSizeLow', wintypes.DWORD), ('NumberOfLinks', wintypes.DWORD),
                    ('FileIndexHigh', wintypes.DWORD), ('FileIndexLow', wintypes.DWORD)]

    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    create = kernel32.CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    basic = kernel32.GetFileInformationByHandleEx
    basic.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    basic.restype = wintypes.BOOL
    identity = kernel32.GetFileInformationByHandle
    identity.argtypes = [wintypes.HANDLE, ctypes.POINTER(HandleInfo)]
    identity.restype = wintypes.BOOL
    close = kernel32.CloseHandle
    close.argtypes = [wintypes.HANDLE]
    close.restype = wintypes.BOOL
    return ctypes, BasicInfo, HandleInfo, create, basic, identity, close


def _record_signature(path):
    """Identity + real change time, never Windows creation-time-as-ctime.

    A failed metadata read returns None: the caller rereads the document and
    does not cache it. In particular size/mtime alone never authorize a hit.
    """
    if os.name == 'nt':
        handle = None
        close = None
        try:
            ctypes, BasicInfo, HandleInfo, create, basic, identity, close = _windows_metadata_api()
            # FILE_READ_ATTRIBUTES, all sharing modes, OPEN_EXISTING. This
            # opens no program and changes no file or directory contents.
            handle = create(str(path), 0x80, 0x7, None, 3, 0, None)
            if handle in (None, ctypes.c_void_p(-1).value):
                handle = None
                return None
            times, info = BasicInfo(), HandleInfo()
            if (not basic(handle, 0, ctypes.byref(times), ctypes.sizeof(times))
                    or not identity(handle, ctypes.byref(info))):
                return None
            if times.ChangeTime <= 0 or info.FileAttributes & 0x10:
                return None
            return (info.VolumeSerialNumber, info.FileIndexHigh, info.FileIndexLow,
                    info.FileSizeHigh, info.FileSizeLow, times.LastWriteTime,
                    times.ChangeTime, times.CreationTime, info.FileAttributes)
        except (OSError, AttributeError, TypeError, ValueError):
            return None
        finally:
            if handle is not None and close is not None:
                close(handle)
    try:
        stat = path.stat()
        return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
    except OSError:
        return None


@lru_cache(maxsize=1)
def _windows_directory_api():
    """Read-only directory enumeration with each entry's file ID and ChangeTime."""
    import ctypes
    from ctypes import wintypes

    class DirectoryInfo(ctypes.Structure):
        # FILE_ID_BOTH_DIR_INFO; FileName follows FileId.
        _fields_ = [('NextEntryOffset', wintypes.DWORD), ('FileIndex', wintypes.DWORD),
                    ('CreationTime', ctypes.c_longlong), ('LastAccessTime', ctypes.c_longlong),
                    ('LastWriteTime', ctypes.c_longlong), ('ChangeTime', ctypes.c_longlong),
                    ('EndOfFile', ctypes.c_longlong), ('AllocationSize', ctypes.c_longlong),
                    ('FileAttributes', wintypes.DWORD), ('FileNameLength', wintypes.DWORD),
                    ('EaSize', wintypes.DWORD), ('ShortNameLength', ctypes.c_byte),
                    ('ShortName', wintypes.WCHAR * 12), ('FileId', ctypes.c_longlong)]

    class HandleInfo(ctypes.Structure):
        _fields_ = [('FileAttributes', wintypes.DWORD), ('CreationTime', wintypes.FILETIME),
                    ('LastAccessTime', wintypes.FILETIME), ('LastWriteTime', wintypes.FILETIME),
                    ('VolumeSerialNumber', wintypes.DWORD), ('FileSizeHigh', wintypes.DWORD),
                    ('FileSizeLow', wintypes.DWORD), ('NumberOfLinks', wintypes.DWORD),
                    ('FileIndexHigh', wintypes.DWORD), ('FileIndexLow', wintypes.DWORD)]

    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    create = kernel32.CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    listing = kernel32.GetFileInformationByHandleEx
    listing.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    listing.restype = wintypes.BOOL
    identity = kernel32.GetFileInformationByHandle
    identity.argtypes = [wintypes.HANDLE, ctypes.POINTER(HandleInfo)]
    identity.restype = wintypes.BOOL
    close = kernel32.CloseHandle
    close.argtypes = [wintypes.HANDLE]
    close.restype = wintypes.BOOL
    return ctypes, DirectoryInfo, HandleInfo, create, listing, identity, close


def _directory_signatures(directory):
    """One-pass {name: signature} in _record_signature's tuple shape, or None.

    NTFS keeps each entry's file ID, size, write/change/creation time in the
    directory index and refreshes them when a writer closes the file. Comparing
    the listing captured at the last strong read therefore detects in-place
    rewrites (including restored size/mtime, which still moves ChangeTime) and
    atomic replacement (new file ID) for 450+ records in one enumeration rather
    than one CreateFile per record. Any failure disables these cache hits.
    """
    if os.name != 'nt':
        return None
    handle = None
    close = None
    try:
        ctypes, DirectoryInfo, HandleInfo, create, listing, identity, close = _windows_directory_api()
        # FILE_LIST_DIRECTORY, all sharing, OPEN_EXISTING, BACKUP_SEMANTICS.
        handle = create(str(directory), 0x1, 0x7, None, 3, 0x02000000, None)
        if handle in (None, ctypes.c_void_p(-1).value):
            handle = None
            return None
        volume = HandleInfo()
        if not identity(handle, ctypes.byref(volume)):
            return None
        buffer = ctypes.create_string_buffer(256 * 1024)
        base = ctypes.addressof(buffer)
        name_offset = DirectoryInfo.FileId.offset + ctypes.sizeof(ctypes.c_longlong)
        signatures, information_class = {}, 11  # FileIdBothDirectoryRestartInfo.
        while True:
            if not listing(handle, information_class, buffer, len(buffer)):
                if ctypes.get_last_error() == 18:  # ERROR_NO_MORE_FILES.
                    return signatures
                return None
            information_class, offset = 10, 0  # FileIdBothDirectoryInfo.
            while True:
                if offset + ctypes.sizeof(DirectoryInfo) > len(buffer):
                    return None
                info = DirectoryInfo.from_buffer_copy(buffer, offset)
                if offset + name_offset + info.FileNameLength > len(buffer):
                    return None
                name = ctypes.wstring_at(base + offset + name_offset, info.FileNameLength // 2)
                file_id, size = info.FileId & 0xffffffffffffffff, info.EndOfFile
                signatures[name] = (None if info.ChangeTime <= 0 or info.FileAttributes & 0x410 else
                    (volume.VolumeSerialNumber, file_id >> 32, file_id & 0xffffffff, size >> 32,
                     size & 0xffffffff, info.LastWriteTime, info.ChangeTime, info.CreationTime,
                     info.FileAttributes))
                if not info.NextEntryOffset:
                    break
                offset += info.NextEntryOffset
    except (OSError, AttributeError, TypeError, ValueError):
        return None
    finally:
        if handle is not None and close is not None:
            close(handle)


@contextmanager
def _reservation_lock(directory):
    """OS-owned global lock: concurrent processes serialize; crashes release it.

    The persistent one-byte lock file is not a journal and is never deleted.
    File existence alone is not a lock and cannot strand a crashed session.
    """
    descriptor = os.open(str(directory / '.collection-reservation.lock'), os.O_RDWR | os.O_CREAT, 0o600)
    with os.fdopen(descriptor, 'r+b') as lock:
        if os.fstat(lock.fileno()).st_size == 0:
            lock.write(b'\0')
            lock.flush()
        lock.seek(0)
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def receipt_matches(before, after, packet):
    original=before.get('favorite_before',{})
    transition_proven=original.get('favorite_warm_fraction')==0 and original.get('favorite_bright_fraction',0)>=.04
    return (
        all(before.get(k) == after.get(k) for k in ('product', 'condition', 'price', 'wear', 'row_index'))
        and before['source_frame_sha256'] != after['source_frame_sha256']
        and (packet.get('startup_page', {}).get('anchor_checks', {}).get('collection.added') is True or transition_proven)
        and packet.get('collection_selected_card', {}).get('favorite_warm_fraction', 0) >= .05
        and packet.get('collection_selected_card', {}).get('favorite_bright_fraction', 1) <= .02
    )


class CollectionJournal:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        # Only independently read, identity-validated completed documents live
        # here. Pending reservations and weak/unavailable metadata never cache.
        self._confirmed_cache = {}

    def _read_record(self, path, before):
        try:
            record = json.loads(path.read_text(encoding='utf-8'))
        except (UnicodeError, ValueError) as error:
            raise ValueError('COLLECTION_JOURNAL_DOCUMENT') from error
        if path.stem != validate_record_identity(record):
            raise ValueError('COLLECTION_JOURNAL_IDENTITY')
        after = _record_signature(path)
        if before is not None and after is not None and before != after:
            raise ValueError('COLLECTION_JOURNAL_CHANGED_DURING_READ')
        stable = before if before is not None and before == after else None
        return record, stable

    def _records_locked(self):
        # Always enumerate under the reservation lock, including when every
        # completed file was cached. New/changed/pending files must be read and
        # deleted entries must disappear before a new reservation is allowed.
        listed = _directory_signatures(self.directory)
        if listed is None:
            with os.scandir(self.directory) as entries:
                names = [entry.name for entry in entries]
        else:
            names = list(listed)
        names = sorted(name for name in names
                       if (name.lower() if os.name == 'nt' else name).endswith('.json'))
        cache, records = {}, []
        for name in names:
            path = self.directory / name
            cached = self._confirmed_cache.get(name)
            entry = listed.get(name) if listed is not None else None
            if entry is not None and cached is not None and cached[2] == entry:
                # Directory entry unchanged since this record's strong read.
                signature, record = cached[0], cached[1]
            else:
                signature = _record_signature(path)
                if signature is not None and cached is not None and cached[0] == signature:
                    record = cached[1]
                else:
                    record, signature = self._read_record(path, signature)
            if signature is not None and record.get('status') in ('confirmed', 'confirmed_reconciliation'):
                cache[name] = (signature, record, entry)
            records.append(record)
        self._confirmed_cache = cache
        return records

    def records(self):
        """Fresh directory membership and pending state, copied history bytes."""
        with _reservation_lock(self.directory):
            return deepcopy(self._records_locked())

    def decision_records(self):
        """Fresh, detached identity/status projection for current decisions.

        Keep the same locked enumeration, strong change-time checks, pending
        re-reads and identity validation. The hot path has no use for hundreds
        of old nested OCR/evidence trees; copying those costs more than reading
        the identity metadata. Full records remain available to audit/UI and
        prepare still independently checks reservations and historical frames.
        """
        with _reservation_lock(self.directory):
            result = []
            for record in self._records_locked():
                item = {key: record[key] for key in ('schema', 'key', 'status', 'identity_key', 'attempt_id')
                        if key in record}
                candidate = record['candidate']
                item['candidate'] = {key: candidate[key] for key in ('product', 'condition', 'wear')}
                result.append(item)
            # Only a bounded small identity projection is detached; mutable or
            # malformed legacy values must never escape into the private cache.
            return deepcopy(result)

    def _refresh_written_record(self, path):
        # Read back our one changed file, rather than trusting a document that
        # another writer might have replaced between our write and metadata.
        self._confirmed_cache.pop(path.name, None)
        record, signature = self._read_record(path, _record_signature(path))
        if signature is not None and record.get('status') in ('confirmed', 'confirmed_reconciliation'):
            self._confirmed_cache[path.name] = (signature, record, None)

    def prepare(self, candidate, evidence, *, allow_confirmed_history=False):
        if type(allow_confirmed_history) is not bool:
            raise ValueError('COLLECTION_JOURNAL_HISTORY_POLICY')
        candidate, evidence = deepcopy(candidate), deepcopy(evidence)
        key = identity_key = candidate_key(candidate)
        if allow_confirmed_history:
            _validate_current_white(candidate)
            identity = _normalized_identity(candidate)
        with _reservation_lock(self.directory):
            matches = []
            if allow_confirmed_history:
                for record in self._records_locked():
                    # An unresolved action anywhere prevents a new input.
                    if record.get('status') not in ('confirmed', 'confirmed_reconciliation'):
                        raise FileExistsError('COLLECTION_PENDING_RECONCILIATION')
                    if _normalized_identity(record['candidate']) == identity:
                        matches.append(record)
                for record in matches:
                    if candidate['source_frame_id'] in _historical_frame_ids(record):
                        raise ValueError('COLLECTION_JOURNAL_STALE_FRAME')
                if matches:
                    key = identity_key + '.' + uuid.uuid4().hex
            document = dict(key=key, status='prepared', candidate=candidate, evidence=evidence)
            if key != identity_key:
                document.update(schema='collection-attempt-v2', identity_key=identity_key,
                                attempt_id=key.rsplit('.', 1)[1])
            payload = json.dumps(document, ensure_ascii=False, indent=2, allow_nan=False)
            # The reservation exists durably before input, including a crash
            # after SendInput and before receipt. New attempts never replace it.
            with (self.directory / (key + '.json')).open('x', encoding='utf-8') as file:
                file.write(payload)
                file.flush()
                os.fsync(file.fileno())
            self._confirmed_cache.pop(key + '.json', None)
            return key

    def release_unsent(self, key, *, archive_directory, evidence):
        """Retire OUR prepared reservation whose input was provably never sent.

        Only a ``prepared`` record (never ``dispatched``/``input_uncertain``)
        qualifies; the caller asserts that SendInput was not reached. The exact
        record plus the release evidence is written to ``archive_directory``
        (outside the scanned journal) before the reservation file is removed.
        """
        if not isinstance(key, str) or re.fullmatch(r'[0-9a-f]{64}(?:\.[0-9a-f]{32})?', key) is None:
            raise ValueError('COLLECTION_JOURNAL_IDENTITY')
        archive = Path(archive_directory)
        if archive.resolve() == self.directory.resolve():
            raise ValueError('COLLECTION_JOURNAL_RELEASE_ARCHIVE')
        archive.mkdir(parents=True, exist_ok=True)
        with _reservation_lock(self.directory):
            path = self.directory / (key + '.json')
            raw = path.read_bytes()
            document = json.loads(raw.decode('utf-8'))
            if (validate_record_identity(document) != key or document.get('status') != 'prepared'
                    or 'sent' in document):
                raise ValueError('COLLECTION_JOURNAL_RELEASE_REQUIRES_UNSENT_PREPARED')
            released = dict(schema='collection-released-unsent-reservation-v1', key=key,
                            original_record_sha256=hashlib.sha256(raw).hexdigest(),
                            original_record=document, evidence=deepcopy(evidence), input_actions=0)
            target = archive / (key + '.' + uuid.uuid4().hex + '.json')
            with target.open('x', encoding='utf-8') as file:
                file.write(json.dumps(released, ensure_ascii=False, indent=2, allow_nan=False))
                file.flush()
                os.fsync(file.fileno())
            os.remove(path)
            self._confirmed_cache.pop(path.name, None)
            return target

    def update(self, key, status, **details):
        if status not in ('dispatched', 'input_uncertain', 'confirmed', 'confirmed_reconciliation'):
            raise ValueError('COLLECTION_JOURNAL_STATUS')
        if not isinstance(key, str) or re.fullmatch(r'[0-9a-f]{64}(?:\.[0-9a-f]{32})?', key) is None:
            raise ValueError('COLLECTION_JOURNAL_IDENTITY')
        if any(name in details for name in ('schema', 'key', 'candidate', 'evidence', 'identity_key', 'attempt_id')):
            raise ValueError('COLLECTION_JOURNAL_RESERVED_FIELD')
        with _reservation_lock(self.directory):
            path = self.directory / (key + '.json')
            document = json.loads(path.read_text(encoding='utf-8'))
            if validate_record_identity(document) != key:
                raise ValueError('COLLECTION_JOURNAL_IDENTITY')
            allowed = {
                'prepared': {'dispatched', 'input_uncertain', 'confirmed_reconciliation'},
                'dispatched': {'confirmed', 'confirmed_reconciliation'},
                'input_uncertain': {'confirmed_reconciliation'},
            }
            if status not in allowed.get(document['status'], set()):
                raise ValueError('COLLECTION_JOURNAL_TRANSITION')
            document.update(status=status, **details)
            payload = json.dumps(document, ensure_ascii=False, indent=2, allow_nan=False)
            temporary = self.directory / (key + '.' + uuid.uuid4().hex + '.tmp')
            with temporary.open('x', encoding='utf-8') as file:
                file.write(payload)
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, path)
            if status in ('confirmed', 'confirmed_reconciliation'):
                self._refresh_written_record(path)
            else:
                self._confirmed_cache.pop(path.name, None)
            return document
