#!/usr/bin/env python3
"""What dead runs leave in tmpfs, removed only when no process can still use it (scripts/wt-tmpfs.sh prune runs it):

  python3 -I scripts/tmpfs-reap.py --builds DIR --perf DIR --shm DIR [--build-ttl MIN] [--dry-run] [--verbose]
  python3 -I scripts/tmpfs-reap.py --selftest

  builds   a directory of DIR (one checkout's builds) whose checkout is gone or where nothing changed for --build-ttl minutes (60), that no process has as its
           working directory, names on its command line, holds open or maps (a loaded .so)
  perf     the runs of the perf gates that died and their templates unused for an hour (scripts/perf/gate.py clean_stale)
  shm      a PostgreSQL.<handle> segment of the user, unchanged for 5 minutes, that no process maps or holds open and whose handle is in no live postmaster's
           control segment (the one that starts with the magic 0x9a503d32); when a postgres process of the user cannot be read, no segment is removed
"""
import argparse
import contextlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE / 'perf'))
import gate  # noqa: E402

DSM_MAGIC = struct.pack('<I', 0x9a503d32)
DSM_MIN_AGE = 300
PERF_TTL = 3600


class Procs:
    """The paths every readable process uses: working directory, command line, open files, mappings. blind: a postgres process of the user could not be read."""

    def __init__(self):
        self.cwds, self.args, self.files, self.blind = set(), [], set(), False
        uid, pids = os.getuid(), [p for p in Path('/proc').iterdir() if p.name.isdigit()]
        self.count = len(pids)
        for p in pids:
            self.read(p, uid)

    def read(self, p, uid):
        try:
            mine = p.stat().st_uid == uid
            self.cwds.add(os.readlink(p / 'cwd'))
            self.args += [a.decode(errors='replace') for a in (p / 'cmdline').read_bytes().split(b'\0') if a]
            self.files.update(line.split(None, 5)[5].strip() for line in (p / 'maps').read_text(errors='replace').splitlines() if line.count(' ') >= 5 and '/' in line)
            self.files.update(os.readlink(f) for f in (p / 'fd').iterdir())
        except (FileNotFoundError, ProcessLookupError):
            pass
        except PermissionError:
            if mine and self.postgres(p):
                self.blind = True

    @staticmethod
    def postgres(p):
        try:
            return (p / 'comm').read_text().startswith(('postgres', 'postmaster'))
        except OSError:
            return True

    def uses(self, d):
        d = str(d)
        under = lambda x: x == d or x.startswith(d + '/')  # noqa: E731
        return any(map(under, self.cwds)) or any(d in a for a in self.args) or any(map(under, self.files))


def recent(d, cutoff):
    """A file or directory under d changed after cutoff."""
    for root, dirs, files in os.walk(d):
        for n in [root] + [os.path.join(root, f) for f in files]:
            try:
                if os.lstat(n).st_mtime > cutoff:
                    return True
            except FileNotFoundError:
                pass
    return False


def orphan(d):
    """The checkout the builds were made from (.repo, scripts/pgtest.sh install) is gone."""
    repo = d / '.repo'
    return repo.is_file() and not Path(repo.read_text()).is_dir()


def builds(root, ttl_min, procs):
    cutoff = time.time() - ttl_min * 60
    return [d for d in sorted(Path(root).glob('*')) if d.is_dir() and (orphan(d) or not recent(d, cutoff)) and not procs.uses(d)]


def registered(controls):
    """Every aligned 32-bit value of the live control segments: more than the handles, never fewer."""
    return {v for c in controls for (v,) in struct.iter_unpack('<I', c[:len(c) // 4 * 4])}


def segments(shm, procs, min_age=DSM_MIN_AGE):
    if procs.blind:
        return []
    uid, cutoff, cands, controls = os.getuid(), time.time() - min_age, [], []
    for f in sorted(Path(shm).glob('PostgreSQL.*')):
        try:
            st = f.stat()
            if st.st_uid != uid or not f.name.split('.', 1)[1].isdigit():
                continue
            if str(f) in procs.files:
                data = f.read_bytes()
                if data[:4] == DSM_MAGIC:
                    controls.append(data)
            elif st.st_mtime < cutoff:
                cands.append(f)
        except FileNotFoundError:
            pass
    known = registered(controls)
    return [f for f in cands if int(f.name.split('.', 1)[1]) not in known]


def remove(paths, dry, why):
    for p in paths:
        print(f'reap: {"would remove" if dry else "remove"} {p} ({why})')
        if not dry:
            shutil.rmtree(p, ignore_errors=True) if p.is_dir() else p.unlink(missing_ok=True)


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--builds')
    p.add_argument('--perf')
    p.add_argument('--shm')
    p.add_argument('--build-ttl', type=int, default=int(os.environ.get('SIFT_BUILD_TTL_MIN', 60)))
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--verbose', action='store_true')
    p.add_argument('--selftest', action='store_true')
    a = p.parse_args(argv)
    if a.selftest:
        return 0 if unittest.main(argv=[sys.argv[0]], exit=False, verbosity=1).result.wasSuccessful() else 1
    procs = Procs()
    if a.builds and Path(a.builds).is_dir():
        remove(builds(a.builds, a.build_ttl, procs), a.dry_run, f'checkout gone or idle {a.build_ttl} min, used by no process')
    if a.perf and Path(a.perf).is_dir() and not a.dry_run:
        gate.clean_stale(Path(a.perf), PERF_TTL)
    if a.shm:
        if procs.blind:
            print('reap: a postgres process cannot be read: no shared memory segment is removed')
        remove(segments(a.shm, procs), a.dry_run, 'unmapped, unregistered, 5 min old')
    if a.verbose:
        print(f'reap: {procs.count} processes read, {len(procs.files)} paths in use{", postgres unreadable" if procs.blind else ""}')
    return 0


class Selftest(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(dir=os.environ.get('TMPDIR', '/tmp')))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.kids = []

    def tearDown(self):
        for k in self.kids:
            k.kill()
            k.wait()
        shutil.rmtree(self.t)

    def hold(self, code, *args, cwd=None):
        """A child that runs code (with argv) and prints ready: its uses are in the /proc snapshot taken after."""
        k = subprocess.Popen([sys.executable, '-I', '-c', code + '\nprint("ready", flush=True)\nimport time; time.sleep(60)', *map(str, args)],
                             cwd=cwd, stdout=subprocess.PIPE, text=True)
        self.kids.append(k)
        self.assertEqual(k.stdout.readline().strip(), 'ready')

    def make(self, path, data=b'x', age=7200):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        for q in (path, path.parent):
            os.utime(q, (time.time() - age, time.time() - age))

    def test_builds(self):
        b = self.t / 'build'
        for n in ('idle', 'cwd', 'arg', 'mapped', 'open', 'fresh', 'gone', 'gone-used'):
            self.make(b / n / 'flavor' / 'pg_sift.so', age=0 if n in ('fresh', 'gone', 'gone-used') else 7200)
            os.utime(b / n, (0, 0))
        for n in ('gone', 'gone-used'):
            (b / n / '.repo').write_text(str(self.t / 'no-checkout'))
        (b / 'fresh' / '.repo').write_text(str(self.t))
        self.hold('', cwd=b / 'gone-used')
        self.hold('', cwd=b / 'cwd' / 'flavor')
        self.hold('', b / 'arg' / 'flavor')
        self.hold('import mmap, sys; f = open(sys.argv[1], "rb"); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ); f.close()', b / 'mapped' / 'flavor' / 'pg_sift.so')
        self.hold('import sys; f = open(sys.argv[1], "rb")', b / 'open' / 'flavor' / 'pg_sift.so')
        self.assertEqual([d.name for d in builds(b, 60, Procs())], ['gone', 'idle'])
        self.assertEqual(main(['--builds', str(b)]), 0)
        self.assertEqual(sorted(d.name for d in b.iterdir()), ['arg', 'cwd', 'fresh', 'gone-used', 'mapped', 'open'])

    def test_segments(self):
        s = self.t / 'shm'
        control = DSM_MAGIC + struct.pack('<III', 2, 64, 0) + struct.pack('<II', 4, 2) + bytes(32)
        for n, data in ((1, b'x'), (2, b'x'), (3, control), (4, b'x'), (6, b'x')):
            self.make(s / f'PostgreSQL.{n}', data)
        self.make(s / 'PostgreSQL.5', age=0)
        self.make(s / 'PostgreSQL.7', DSM_MAGIC + struct.pack('<I', 6))
        mapper = 'import mmap, sys\nm = []\nfor a in sys.argv[1:]:\n    f = open(a, "r+b"); m.append(mmap.mmap(f.fileno(), 0)); f.close()'
        self.hold(mapper, s / 'PostgreSQL.2', s / 'PostgreSQL.3')
        procs = Procs()
        self.assertEqual([f.name for f in segments(s, procs)], ['PostgreSQL.1', 'PostgreSQL.6', 'PostgreSQL.7'])
        procs.blind = True
        self.assertEqual(segments(s, procs), [])
        self.assertEqual(main(['--shm', str(s), '--dry-run']), 0)
        self.assertEqual(len(list(s.iterdir())), 7)
        self.assertEqual(main(['--shm', str(s)]), 0)
        self.assertEqual(sorted(f.name for f in s.iterdir()), ['PostgreSQL.2', 'PostgreSQL.3', 'PostgreSQL.4', 'PostgreSQL.5'])

    def test_perf(self):
        p = self.t / 'perf'
        (p / 'tpl-old').mkdir(parents=True)
        (p / 'tpl-new').mkdir()
        os.utime(p / 'tpl-old', (0, 0))
        self.assertEqual(main(['--perf', str(p), '--dry-run']), 0)
        self.assertEqual(len(list(p.iterdir())), 2)
        self.assertEqual(main(['--perf', str(p)]), 0)
        self.assertEqual([d.name for d in p.iterdir()], ['tpl-new'])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
