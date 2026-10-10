"""Bounded evidence for the long-running F2 cycle.

A collection run writes 66-131 MB of session records (live 2026-10-09,
hotkey_runs/20261009-191652 and -195907) and the disk had 28 GB free: a
night of cycling would fill it. Finished runs of the cycle are packed: each
session.json with its session.steps/ goes into session.zip next to them
(92 MB -> 10 MB, about 1 s), verified before the originals are removed;
nothing is dropped. The newest runs stay unpacked for debugging. Below
MIN_FREE_BYTES the cycle stops instead of writing on.
"""
from pathlib import Path
import shutil
import zipfile

KEEP_UNPACKED = 2
MIN_FREE_BYTES = 3 * 1024 ** 3


def pack_sessions(run_dir):
    """Pack every session record under run_dir; returns bytes saved."""
    saved = 0
    for record in sorted(Path(run_dir).rglob('session.json')):
        folder = record.parent
        steps = folder / 'session.steps'
        target = folder / 'session.zip'
        if target.exists():
            continue
        members = [record] + (sorted(p for p in steps.rglob('*') if p.is_file()) if steps.is_dir() else [])
        before = sum(m.stat().st_size for m in members)
        part = folder / 'session.zip.part'
        with zipfile.ZipFile(part, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for member in members:
                archive.write(member, member.relative_to(folder).as_posix())
        with zipfile.ZipFile(part) as archive:
            intact = archive.testzip() is None and len(archive.namelist()) == len(members)
            sizes_match = all(archive.getinfo(m.relative_to(folder).as_posix()).file_size == m.stat().st_size
                              for m in members)
        if not (intact and sizes_match):
            part.unlink()
            raise RuntimeError('EVIDENCE_PACK_UNVERIFIED:' + str(folder))
        part.replace(target)
        record.unlink()
        if steps.is_dir():
            shutil.rmtree(steps)
        saved += before - target.stat().st_size
    return saved


class EvidenceKeeper:
    """Finished run directories of one cycle, packed once newer ones exist."""

    def __init__(self, keep=KEEP_UNPACKED, pack=pack_sessions):
        self.keep, self.pack = keep, pack
        self.finished = {}
        self.saved = 0
        self.errors = []

    def finished_run(self, kind, directory):
        if directory:
            self.finished.setdefault(kind, []).append(Path(directory))

    def pack_older(self, keep=None):
        keep = self.keep if keep is None else keep
        for kind, folders in self.finished.items():
            while len(folders) > keep:
                folder = folders.pop(0)
                try:
                    self.saved += self.pack(folder)
                except Exception as error:
                    self.errors.append('%s: %s' % (folder, str(error) or type(error).__name__))

    def pending(self):
        return sum(max(0, len(folders) - self.keep) for folders in self.finished.values())


def free_bytes(path):
    return shutil.disk_usage(Path(path)).free
