"""What a server spent on a measurement: its CPU and, for Postgres, the buffers its queries touched.

    python3 -I meter.py pid PGDATA|PID      print the pid of a server (the postmaster of a data directory, or the pid given)

The CPU is every process of the server (the postmaster, its backends and workers; the threads of a Java process): the postmaster's own time and that of the
children it has waited for (exited backends), plus the time of the children still running, so a backend that ends inside the window is counted once.
The buffers are the hits and reads of pg_stat_database of the database, 8 KiB each: the data the queries touched, not what the device was asked for.
A search cell divides both by the queries it ran: CPU milliseconds and MB per query.
"""
import os, subprocess, sys

BLOCK_MB = 8192 / 2**20
TICK = os.sysconf('SC_CLK_TCK')


def stat(pid):
    """(ppid, own ticks, ticks of the children it waited for) of a process, None when it is gone"""
    try:
        with open(f'/proc/{pid}/stat') as f:
            rest = f.read().rsplit(')', 1)[1].split()   # after the command name, which may hold spaces
    except (FileNotFoundError, ProcessLookupError):
        return None
    return int(rest[1]), int(rest[11]) + int(rest[12]), int(rest[13]) + int(rest[14])


def children():
    """{pid: ppid} of every process"""
    out = {}
    for name in os.listdir('/proc'):
        if name.isdigit() and (s := stat(int(name))):
            out[int(name)] = s[0]
    return out


def cpu_ms(pid):
    """CPU milliseconds the server of `pid` has used so far, its running children included"""
    s = stat(pid)
    if not s:
        return 0.0
    ppid = children()
    kids, todo = [], [pid]
    while todo:
        p = todo.pop()
        for c, parent in ppid.items():
            if parent == p:
                kids.append(c)
                todo.append(c)
    ticks = s[1] + s[2] + sum(k[1] for c in kids if (k := stat(c)))
    return ticks * 1000 / TICK


def postmaster(pgdata):
    with open(os.path.join(pgdata, 'postmaster.pid')) as f:
        return int(f.readline())


def blocks(psql, dbname):
    """hits plus reads of the database so far; the backends that ran the queries have ended or flushed (a session that is still open runs pg_stat_force_next_flush() first)"""
    out = subprocess.run([*psql, '-X', '-qAt', '-d', dbname, '-c', "SELECT pg_stat_force_next_flush()", '-c',
                          "SELECT coalesce(blks_hit + blks_read, 0) FROM pg_stat_database WHERE datname = current_database()"],
                         capture_output=True, text=True, check=True).stdout.split()
    return int(out[-1])


class Meter:
    """CPU of a server and, with a database, the buffers it touched, between start() and stop(); per(n) is (CPU ms, MB) per query"""

    def __init__(self, pid, psql=None, dbname=None):
        self.pid, self.psql, self.dbname = pid, psql or ['psql'], dbname

    def start(self):
        self.cpu0, self.blocks0 = cpu_ms(self.pid), blocks(self.psql, self.dbname) if self.dbname else None
        return self

    def stop(self):
        self.cpu, self.blocks = cpu_ms(self.pid) - self.cpu0, blocks(self.psql, self.dbname) - self.blocks0 if self.dbname else None

    def per(self, queries):
        """(CPU ms, MB touched) per query; the MB is None for a server that has no buffer counter"""
        if not queries:
            return None, None
        return self.cpu / queries, None if self.blocks is None else self.blocks * BLOCK_MB / queries


def main(argv):
    if len(argv) == 2 and argv[0] == 'pid':
        print(int(argv[1]) if argv[1].isdigit() else postmaster(argv[1]))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
