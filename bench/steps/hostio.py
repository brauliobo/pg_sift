#!/usr/bin/env python3
"""The IO operations of a measured statement, whatever the engine (bench/README.md, "IO operations"): the harness reads the host, not the engine's own counters.

    python3 -I bench/steps/hostio.py snap [--pid PID,..] [--pgdata DIR] [--datadir DIR] > A.json     the counters now
    python3 -I bench/steps/hostio.py delta A.json [same options]                                     the figures since A.json, as `name<TAB>value` lines

Three sources, each `None` where the host cannot say (the figure is then absent from a results file and '-' in a table):

    device    /proc/diskstats of the block device that holds the data directory: reads completed (iops), sectors read, merged reads, writes, flushes. A data directory on tmpfs or
              another file system with no device (overlay, ZFS) has none; on ZFS the pool's /proc/spl/kstat/zfs/POOL/io is read instead (reads, bytes)
    process   /proc/PID/io of the engine's processes and their children (rchar, syscr, read_bytes, write_bytes); a process that starts or ends inside the run is not in it
              (a PostgreSQL parallel worker), so the cgroup is the complete count where there is one
    cgroup    io.stat of the cgroup of the first process (rios, rbytes, wios, wbytes of every device): a container or a systemd scope keeps what its exited processes did

`io_avg_kb_per_read` is the device's (sectors * 0.5 KB / reads), else the cgroup's, else the process's read_bytes / syscr (every read(2): sockets and pipes included, so only a
rough figure). Cold runs are made cold by the caller (iolib.cool, or the drop of the page cache); `cache_dropped` is theirs to record. The `root` argument is the file system the
files are read from ('/' except in bench/test/test_hostio.py, which builds a fake /proc and /sys).
"""
import argparse
import json
import os
import sys

MB = 2 ** 20
SECTOR = 512


def read(root, path):
    try:
        with open(os.path.join(root, path.lstrip('/'))) as f:
            return f.read()
    except OSError:
        return None


def device_of(path, root='/'):
    """('disk', name in /proc/diskstats), ('zfs', pool) or None for the file system PATH is on"""
    try:
        dev = os.stat(path).st_dev
    except OSError:
        return None
    major, minor = os.major(dev), os.minor(dev)
    if major:
        try:
            return 'disk', os.path.basename(os.readlink(os.path.join(root, 'sys/dev/block/%d:%d' % (major, minor))))
        except OSError:
            return None
    best = None
    for line in (read(root, '/proc/mounts') or '').splitlines():
        src, mnt, typ = line.split()[:3]
        if typ == 'zfs' and (path == mnt or path.startswith(mnt.rstrip('/') + '/')) and (best is None or len(mnt) >= len(best[0])):
            best = (mnt, src.split('/')[0])
    return ('zfs', best[1]) if best else None


def disk_counters(source, root='/'):
    """{reads, merged, read_bytes, writes, write_bytes, flushes} of the device since boot; None when it has no line"""
    kind, name = source
    if kind == 'zfs':
        lines = (read(root, '/proc/spl/kstat/zfs/%s/io' % name) or '').splitlines()
        if len(lines) < 3:
            return None
        v = dict(zip(lines[1].split(), (int(x) for x in lines[2].split())))
        return dict(reads=v['reads'], merged=None, read_bytes=v['nread'], writes=v['writes'], write_bytes=v['nwritten'], flushes=None)
    for line in (read(root, '/proc/diskstats') or '').splitlines():
        f = line.split()
        if len(f) > 13 and f[2] == name:
            n = [int(x) for x in f[3:]]
            return dict(reads=n[0], merged=n[1], read_bytes=n[2] * SECTOR, writes=n[4], write_bytes=n[6] * SECTOR, flushes=n[15] if len(n) > 15 else None)
    return None


def ppid(pid, root='/'):
    stat = read(root, '/proc/%s/stat' % pid)
    return int(stat.rsplit(')', 1)[1].split()[1]) if stat else None


def tree(pid, root='/'):
    """PID and every descendant (the postmaster and its backends, a JVM and its threads' processes)"""
    kids, out = {}, [pid]
    try:
        names = os.listdir(os.path.join(root, 'proc'))
    except OSError:   # no /proc (macOS): the process has no counters, and the figure is absent
        names = []
    for p in names:
        if p.isdigit() and ppid(p, root) is not None:
            kids.setdefault(ppid(p, root), []).append(int(p))
    for p in out:
        out += kids.get(p, [])
    return out


def proc_counters(pids, root='/'):
    """{rchar, syscr, read_bytes, write_bytes} summed over PIDS; None when none can be read (not Linux, another user)"""
    total, seen = dict(rchar=0, syscr=0, read_bytes=0, write_bytes=0), 0
    for pid in pids:
        text = read(root, '/proc/%s/io' % pid)
        if text is None:
            continue
        seen += 1
        for line in text.splitlines():
            k, _, v = line.partition(':')
            if k in total:
                total[k] += int(v)
    return total if seen else None


def cgroup_counters(pid, root='/'):
    """{rios, rbytes, wios, wbytes} of the cgroup (v2) the process is in, every device; None without one"""
    for line in (read(root, '/proc/%s/cgroup' % pid) or '').splitlines():
        if line.startswith('0::'):
            text = read(root, '/sys/fs/cgroup%s/io.stat' % line[3:])
            break
    else:
        return None
    if text is None:
        return None
    total = dict(rios=0, rbytes=0, wios=0, wbytes=0)
    for line in text.splitlines():
        for f in line.split()[1:]:
            k, _, v = f.partition('=')
            if k in total:
                total[k] += int(v)
    return total


def snapshot(pids=(), datadir=None, root='/', children=False):
    """the counters now; pids are the engine's processes (their descendants are added)"""
    pids = list(dict.fromkeys(p for q in pids for p in tree(q, root)))
    source = device_of(datadir, root) if datadir else None
    return dict(disk=disk_counters(source, root) if source else None, proc=proc_counters(pids, root) if pids else None,
                cgroup=cgroup_counters(pids[0], root) if pids else None, source=source and ':'.join(source))


def sub(a, b):
    return None if a is None or b is None else {k: None if a[k] is None else b[k] - a[k] for k in a}


def figures(a, b):
    """{name: value} between two snapshots; names are those of the results files (io_<cache>_ is added by `metrics`), None where no source says"""
    d, p, c = sub(a['disk'], b['disk']), sub(a['proc'], b['proc']), sub(a['cgroup'], b['cgroup'])
    out = dict(dev_read_iops=d and d['reads'], dev_read_mb=d and d['read_bytes'] / MB, dev_merged=d and d['merged'], dev_write_iops=d and d['writes'], dev_write_mb=d and d['write_bytes'] / MB,
               dev_flushes=d and d['flushes'], proc_read_ops=p and p['syscr'], proc_read_mb=p and p['read_bytes'] / MB, proc_write_mb=p and p['write_bytes'] / MB,
               cg_read_ios=c and c['rios'], cg_read_mb=c and c['rbytes'] / MB)
    for ops, mb in ((d and d['reads'], d and d['read_bytes'] / MB), (c and c['rios'], c and c['rbytes'] / MB), (p and p['syscr'], p and p['read_bytes'] / MB)):
        if ops and mb:   # a process that read from the page cache or tmpfs asked the device for nothing: no average
            out['avg_kb_per_read'] = mb * 1024 / ops
            break
    else:
        out['avg_kb_per_read'] = None
    return out


class Meter:
    """`start()` before the run, `stop()` after (the figures); PIDS may be a function (the processes alive now)"""

    def __init__(self, pids=(), datadir=None, root='/'):
        self.pids, self.datadir, self.root, self.figures = pids, datadir, root, {}

    def snap(self):
        return snapshot(self.pids() if callable(self.pids) else self.pids, self.datadir, self.root)

    def start(self):
        self.a = self.snap()

    def stop(self):
        self.figures = figures(self.a, self.snap())
        return self.figures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['snap', 'delta'])
    ap.add_argument('before', nargs='?', help='delta: the file snap wrote')
    ap.add_argument('--pid', default='', help='PID,..: the engine\'s processes')
    ap.add_argument('--pgdata', help='a PostgreSQL data directory: its postmaster is the process and the directory the device')
    ap.add_argument('--datadir')
    a = ap.parse_args()
    pids = [int(p) for p in a.pid.split(',') if p]
    if a.pgdata:
        pids.append(int(open(os.path.join(a.pgdata, 'postmaster.pid')).readline()))
    now = snapshot(pids, a.datadir or a.pgdata)
    if a.cmd == 'snap':
        print(json.dumps(now))
    else:
        for k, v in figures(json.load(open(a.before)), now).items():
            print('%s\t%s' % (k, '-' if v is None else '%.2f' % v))


if __name__ == '__main__':
    sys.exit(main())
