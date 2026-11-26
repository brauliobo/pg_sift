"""What the suites share beyond collect.py: the ceilings of Spec 107 (Limits: the Ceilings of cases.py and the counts the suites add), the files and the peak memory of a
backend from /proc, a heap twin compared with a sift table in one snapshot, and the open prepared transactions of the cluster.
"""
from __future__ import annotations

import os
import sys
import threading
from dataclasses import dataclass

sys.dont_write_bytecode = True

from cases import Ceilings  # noqa: E402
from collect import rss_kb  # noqa: E402

TWIN = ('SELECT grp, count(*), sum(v) FROM {t} GROUP BY grp ORDER BY grp', 'SELECT id, v FROM {t} ORDER BY v DESC, id LIMIT 10')


@dataclass(frozen=True)
class Limits(Ceilings):
    prepared_left: int | None = 0
    write_skew: int | None = 0
    unpriv_waits: int | None = 0
    standby_mismatch: int | None = 0
    promoted_errors: int | None = 0
    fault_unseen: int | None = 0
    wedged: int | None = 0
    fd_delta: int | None = None
    par_workers_peak: int | None = None


def fds(pid: int, under: str = '/') -> int | None:
    """the files of a server process open under the directory UNDER (Linux), None where /proc has none"""
    try:
        return sum(os.readlink(f'/proc/{pid}/fd/{f}').startswith(under) for f in os.listdir(f'/proc/{pid}/fd'))
    except OSError:
        return None


def fd_delta(before: list, after: list) -> int | None:
    return None if None in before + after else max(b - a for a, b in zip(before, after))


def anon_peak_mb(ctx, w, sql: str) -> float | None:
    """the peak growth of the anonymous memory of W's backend while it runs SQL (sampled every 5 ms; shared buffers are not anonymous), None without /proc or with --no-rss;
    also with assertions, where a caller compares it with the same growth of another statement"""
    pid = w.pid
    base = None if ctx.a.no_rss else rss_kb(pid)
    peak, done = [base or 0], threading.Event()

    def sample():
        while not done.wait(0.005):
            peak[0] = max(peak[0], rss_kb(pid) or 0)

    t = threading.Thread(target=sample, daemon=True)
    t.start()
    try:
        w.run(sql)
    finally:
        done.set()
        t.join()
    return None if base is None else round((max(peak[0], rss_kb(pid) or 0) - base) / 1024, 1)


def twin_mismatch(db, a: str, b: str, sqls: tuple = TWIN) -> int:
    """1 when sift table A and its heap twin B answer differently in one REPEATABLE READ snapshot"""
    body = '; '.join(q.format(t=t) for t in (a, b) for q in sqls)
    out = db.run(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {body}; COMMIT')
    return int(out[:len(out) // 2] != out[len(out) // 2:])


def twin_do(w, a: str, b: str, allow: tuple = ()) -> int:
    """twin_mismatch in a worker: an expected error (ALLOW) is no mismatch"""
    body = '; '.join(q.format(t=t) for t in (a, b) for q in TWIN)
    out = w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {body}; COMMIT', allow)
    return 0 if out is None else int(out[:len(out) // 2] != out[len(out) // 2:])


def prepared_left(db) -> int:
    """the prepared transactions of the load still open; they are rolled back, so the next case starts without them"""
    gids = db.run("SELECT gid FROM pg_prepared_xacts WHERE gid LIKE 'load\\_%'")
    for g in gids:
        db.run(f"ROLLBACK PREPARED '{g}'")
    return len(gids)


def selftest(expect):
    lim = Limits()
    expect(lim.prepared_left == 0 and lim.lost_rows == 0 and lim.fd_delta is None, 'Limits keeps the hard ceilings of Ceilings')
    expect(fd_delta([3, 4], [5, 4]) == 2 and fd_delta([None], [1]) is None, 'fd_delta')
    expect(fds(-1) is None, 'no process, no files')
