"""The observations of the load gate (scripts/load/run.py): a sampler of the sessions under load, the memory of a backend, the hygiene of the cluster after a case.

The sampler reads pg_stat_activity every 50 ms for the sessions named load_w*: how many of them were active, waited for a heavyweight lock or for an LWLock (by name), the peak
of the coarse (relation) row locks in sift.locks, the pacer of the maintenance (sift.activity) and an optional peak of a case. Nothing is timed: a share of samples and a peak are
the same on a fast host and a slow one. The hygiene after a case is what no load may leave: problems of sift.check, quarantined or failing relations, temporary files, the crashes
and leaks of the server log. The ledger helpers check that the rows a worker committed are the rows a table holds, and the oracle that an index path answers what the heap does.
"""
from __future__ import annotations

import codecs
import math
import os
import random
import re
import sys
import threading
import time

sys.dont_write_bytecode = True

SAMPLE_S = 0.05
N = 8192
META = 'XX002'
AGG = 'SELECT grp, count(*), sum(v) FROM {t} GROUP BY grp ORDER BY grp'
BM = "SELECT count(*), sum(id) FROM {t} WHERE body @@> 'wire'"
WAITS = """SELECT count(*), count(*) FILTER (WHERE wait_event_type = 'Lock'), count(*) FILTER (WHERE wait_event_type = 'LWLock'),
  coalesce(string_agg(wait_event, ',') FILTER (WHERE wait_event_type = 'LWLock'), '')
  FROM pg_stat_activity WHERE application_name LIKE 'load_w%' AND state = 'active' AND pid <> pg_backend_pid()"""
COARSE = "SELECT count(*) FROM sift.locks WHERE kind IN ('row lock', 'row key') AND detail = 'relation' AND granted"
PACER = "SELECT workers_active, pacer_mode, pacer_delay_ms FROM sift.activity() WHERE kind = 'summary'"
CTX_KB = "SELECT coalesce(sum(total_bytes), 0) / 1024 FROM pg_backend_memory_contexts WHERE name LIKE 'sift%'"
IN_CASE = "IN (SELECT c.oid FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname LIKE 'load_case%')"
SIFT_RELS = f"""SELECT c.oid FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col', 'sift_table')
  AND c.relkind IN ('r', 'i') AND c.oid {IN_CASE} ORDER BY 1"""
LEAKCHECK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'leakcheck.sh')


def leak_rules(path: str = LEAKCHECK) -> list:
    """(regex, sample line) of every row of LEAK_RULES in scripts/leakcheck.sh: the load gate finds what the leak gate finds"""
    with open(path) as f:
        rows = re.search(r'^LEAK_RULES=\(\n(.*?)^\)', f.read(), re.M | re.S).group(1)
    return [codecs.decode(r, 'unicode_escape').split('\t') for r in re.findall(r"^\s*\$'((?:[^'\\]|\\.)*)'$", rows, re.M)]


LEAKS = re.compile('|'.join([r for r, _ in leak_rules()] + ['cache lookup failed', 'could not find block containing chunk', r'\[XX000\]']))
SEP = "SELECT '__SIFT_SEP__'"
TIER_TMP = """SELECT count(*) FROM (SELECT d FROM pg_ls_dir('base/pgsql_tmp', true, false) d WHERE d LIKE 'pgsql_tmp_sift_tier.%') t,
  LATERAL pg_ls_dir('base/pgsql_tmp/' || t.d, true, false) f WHERE f LIKE '%.tmp.%'"""


class Observer:
    """samples the sessions under load in a thread of its own until stop(), which raises the error a sample met on a server that still runs; `peak` is SQL of one
    number whose peak the case wants"""

    def __init__(self, db, peak: str = '', pacer: bool = False):
        self.db, self.peak_sql, self.pacer = db, peak, pacer
        self.samples = self.active = self.lock = self.lwlock = self.esc = self.peak = self.throttled = self.busy = 0
        self.events: dict[str, int] = {}
        self.error = ''
        self.done = threading.Event()
        self.t = threading.Thread(target=self.loop, daemon=True)

    def start(self) -> Observer:
        self.t.start()
        return self

    def loop(self):
        while not self.done.is_set():
            try:
                self.sample()
            except Exception as e:
                self.error = '' if self.db.dead else f'{type(e).__name__}: {e}'
                return
            self.done.wait(SAMPLE_S)

    def sample(self):
        n, lock, lw, names = self.db.row(WAITS)
        self.samples, self.active = self.samples + 1, self.active + int(n)
        self.lock, self.lwlock = self.lock + int(lock), self.lwlock + int(lw)
        for e in filter(None, names.split(',')):
            self.events[e] = self.events.get(e, 0) + 1
        self.esc = max(self.esc, int(self.db.val(COARSE)))
        if self.peak_sql:
            self.peak = max(self.peak, int(float(self.db.val(self.peak_sql) or 0)))
        if self.pacer:
            workers, mode, delay = self.db.row(PACER)
            self.busy += int(workers) > 0
            self.throttled += int(workers) > 0 and (mode != 'full' or int(delay) > 0)

    def stop(self) -> Observer:
        self.done.set()
        self.t.join()
        if self.error:
            raise RuntimeError(f'the sampler: {self.error}')
        return self

    def metrics(self) -> dict:
        pct = lambda k: round(100.0 * k / self.active, 1) if self.active else 0.0
        return {'lock_wait_pct': pct(self.lock), 'lwlock_wait_pct': pct(self.lwlock), 'coarse_escalations': self.esc}

    def top_waits(self) -> str:
        return ','.join(f'{k}:{v}' for k, v in sorted(self.events.items(), key=lambda kv: -kv[1])[:3])


def rss_kb(pid: int) -> int | None:
    """the private anonymous memory of a server process (Linux), None where /proc has none"""
    try:
        with open(f'/proc/{pid}/smaps_rollup') as f:
            return next(int(l.split()[1]) for l in f if l.startswith('Anonymous:'))
    except (OSError, StopIteration):
        return None


def ctx_kb(db) -> int:
    return int(float(db.val(CTX_KB)))


def p99(lat: list) -> float:
    if not lat:
        return 0.0
    s = sorted(lat)
    return s[min(len(s) - 1, (len(s) * 99 + 99) // 100 - 1)]


def p99_ratio(quiet: list, busy: list, floor_ms: float = 2.0) -> float:
    """the p99 of BUSY over the p99 of QUIET (at least FLOOR_MS); no statement under load is no measure, and fails the ceiling"""
    if not busy:
        return math.inf
    return round(p99(busy) / max(p99(quiet), floor_ms), 2)


def snap(a: str, b: str, pre: str = '') -> str:
    """A and B in one REPEATABLE READ snapshot (after PRE), the line of SEP between their answers"""
    return f'BEGIN ISOLATION LEVEL REPEATABLE READ; {pre}{a}; {SEP}; {b}; COMMIT'


def gucs(sql: str, guc: str) -> tuple:
    return f'SET LOCAL {guc} = on; {sql}', f'SET LOCAL {guc} = off; {sql}'


def halves_differ(out: list | None) -> int:
    """1 when the lines of OUT before the line of SEP differ from those after it, 0 for a statement that failed (None)"""
    if out is None:
        return 0
    i = out.index('__SIFT_SEP__')
    return int(out[:i] != out[i + 1:])


def oracle(db, sql: str, guc: str) -> int:
    """1 when SQL answers differently with GUC on and off in one REPEATABLE READ snapshot (the index path against the heap)"""
    return halves_differ(db.run(snap(*gucs(sql, guc))))


def sift_rels(db) -> list:
    return db.run(SIFT_RELS)


def checks(db, damaged: tuple = ()) -> dict:
    """what a case may not leave in the database of DB: problems found by sift.check, quarantines and errors of sift.health outside DAMAGED (oids), failed maintenance"""
    skip = ','.join(damaged) or '0'
    problems = sum(int(db.val(f"SELECT count(*) FROM sift.check({r}::regclass, true)")) for r in sift_rels(db) if r not in damaged)
    q = int(db.val(f"SELECT count(*) FROM sift.quarantine WHERE index::oid {IN_CASE} AND index::oid NOT IN ({skip})"))
    health = int(db.val(f"SELECT count(*) FROM sift.health() WHERE severity = 'error' AND relation::oid {IN_CASE} AND relation::oid NOT IN ({skip})"))
    failures = int(db.val(f"SELECT coalesce(sum(maint_failures), 0) FROM sift.status() WHERE relation::oid {IN_CASE} AND relation::oid NOT IN ({skip})"))
    return {'verify_errors': problems, 'false_quarantines': q, 'health_errors': health, 'maint_failures': failures}


def hygiene(db, damaged: tuple = ()) -> dict:
    """checks() and the temporary files of the cluster, the half-written chunks of the tier caches among them: counts that add up"""
    return dict(checks(db, damaged), tmp_files_left=int(db.val('SELECT count(*) FROM pg_ls_tmpdir()')) + int(db.val(TIER_TMP)))


class LogScan:
    """the crash and leak lines the server logs PATHS gained since mark(); a log made anew since (a standby cloned again) is read from its start"""

    def __init__(self, paths: list):
        self.paths, self.at = paths, {}

    def mark(self):
        self.at = {p: (st.st_ino, st.st_size) for p in self.paths if (st := stat(p))}

    def start(self, path: str, st: os.stat_result) -> int:
        ino, size = self.at.get(path, (None, 0))
        return size if st.st_ino == ino and st.st_size >= size else 0

    def findings(self) -> list:
        out = []
        for p in self.paths:
            if st := stat(p):
                with open(p, errors='replace') as f:
                    f.seek(self.start(p, st))
                    out += [l.strip()[:200] for l in f if LEAKS.search(l)]
        return out


def stat(path: str) -> os.stat_result | None:
    try:
        return os.stat(path)
    except FileNotFoundError:
        return None


# The workload and the ledger the cases share: the tables of the three engines from load_base_fact, a writer that keeps what it committed, and the checks of a ledger.

def nxt(w) -> int:
    """the next id of worker W: its own sequence, which goes on across phases"""
    w.seq = getattr(w, 'seq', 0) + 1
    return w.seed * 10 ** 6 + w.seq


def writers(cores: int) -> int:
    return min(8, max(2, cores // 2))


def zipf(seed: int, n: int, s: float) -> int:
    u = random.Random(seed).random()
    return min(n, int(((n ** (1 - s) - 1) * u + 1) ** (1 / (1 - s))))


def src(rows: int, fixture: bool = True) -> str:
    """the first ROWS of load_base_fact, or the same shape from generate_series in a database without the fixture"""
    if fixture:
        return f'SELECT * FROM load_base_fact WHERE id <= {rows}'
    return f"SELECT g, g % 64, (hashint4(g) & 1023)::int8, 'wire tail' || g % 97 FROM generate_series(1, {rows}) g"


def tbl(t: str, rows: int = N, fixture: bool = True) -> str:
    return f'CREATE TABLE {t} (id int8 PRIMARY KEY, grp int, v int8, body text) USING sift_table; INSERT INTO {t} {src(rows, fixture)}'


def col(t: str, rows: int = N, fixture: bool = True) -> str:
    return f'CREATE TABLE {t} (id int8, grp int, v int8, body text); INSERT INTO {t} {src(rows, fixture)}; ' \
           f'CREATE INDEX {t}_ix ON {t} USING sift_col (id, grp, v); ANALYZE {t}'


def bm(t: str, rows: int = N) -> str:
    return f'CREATE TABLE {t} (id int8, grp int, v int8, body text); INSERT INTO {t} SELECT * FROM load_base_fact WHERE id <= {rows}; ' \
           f'CREATE INDEX {t}_ix ON {t} USING sift (body); ANALYZE {t}'


def row3(t: str, i: int) -> str:
    return f"INSERT INTO {t} VALUES ({i}, {i % 64}, {i % 1000}, 'wire tail{i % 97}')"


def kept(db, t: str, ids: set, lo: int, hi: int = 10 ** 8) -> dict:
    """lost_rows and dup_rows of T against the ids in [LO, HI) a ledger says it holds"""
    n, d, s = (int(x or 0) for x in db.row(f'SELECT count(*), count(DISTINCT id), sum(id) FROM {t} WHERE id >= {lo} AND id < {hi}'))
    return {'lost_rows': abs(len(ids) - d) + (s != sum(ids) and d == len(ids)), 'dup_rows': n - d}


def add(*dicts) -> dict:
    out = {}
    for d in dicts:
        for k, v in d.items():
            out[k] = out[k] + v if k in out and isinstance(v, (int, float, list)) else v
    return out


def base(ctx, roles: dict, first: str, **kw) -> dict:
    """the ops of the role FIRST and the waits of the phase ROLES; run.py adds the unexpected errors of every worker of the case, in every phase"""
    return {'ops': roles[first].ops, **ctx.obs.metrics(), **kw}


def until(db, sql: str, cap_s: float) -> int:
    """0 once SQL answers 0 within CAP_S, else 1 (the wait is a liveness cap, never a measured time)"""
    end = time.monotonic() + cap_s
    while time.monotonic() < end:
        if int(db.val(sql) or 0) == 0:
            return 0
        time.sleep(0.05)
    return 1


def mem(ctx, ws: list):
    for w in ws:
        w.run('DISCARD ALL')
    return [ctx_kb(w) for w in ws], ctx.rss(ws)


def mem_delta(ctx, before, after) -> dict:
    return {'ctx_kb_delta': max(b - a for a, b in zip(before[0], after[0])), 'rss_mb_delta': ctx.rss_mb_delta(before[1], after[1])}


def ledger_writes(t: str, kinds: tuple = ('tbl', 'col', 'bm'), churn: bool = True):
    """a writer of T_tbl, T_col and T_bm in one transaction: an insert and, with CHURN, every third an update and every fourth a delete of its row before;
    w.ids is what it committed"""
    def write(w, i):
        if not hasattr(w, 'ids'):
            w.ids = set()
        k = nxt(w)
        old = k - 2
        dele = churn and k % 4 == 0 and old in w.ids
        stmts = [row3(f'{t}_{e}', k) for e in kinds] + [f'UPDATE {t}_{e} SET v = v + 1 WHERE id = {old}' for e in kinds if churn and k % 3 == 0]
        stmts += [f'DELETE FROM {t}_{e} WHERE id = {old}' for e in kinds if dele]
        sql = 'BEGIN; ' + '; '.join(stmts) + '; COMMIT'
        if w.do(sql, getattr(w, 'allow', ())) is not None:
            w.ids = (w.ids | {k}) - ({old} if dele else set())
    return write


def multi_kept(db, t: str, ws: list, kinds: tuple = ('tbl', 'col', 'bm')) -> dict:
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws))
    return add(*(kept(db, f'{t}_{e}', ids, 10 ** 6) for e in kinds))


def engines(db, t: str = 'e', rows: int = N, kinds: tuple = ('tbl', 'col', 'bm')):
    db.run('; '.join({'tbl': tbl, 'col': col, 'bm': bm}[e](f'{t}_{e}', rows) for e in kinds))


def oracles(db, t: str = 'e') -> int:
    return oracle(db, AGG.format(t=f'{t}_col'), 'sift.enable_agg') + oracle(db, BM.format(t=f'{t}_bm'), 'sift.enable_scan')


def oracle_heap(db, t: str) -> str:
    return db.run(f'SET sift.enable_scan = off; {BM.format(t=t)}; RESET sift.enable_scan')[0]


def selftest(expect):
    expect(p99([]) == 0.0 and p99(list(range(1, 101))) == 99 and p99([5]) == 5, 'p99')
    expect(p99_ratio([1.0] * 10, [5.0] * 10) == 2.5 and p99_ratio([4.0], [8.0]) == 2.0, 'p99_ratio floors the quiet p99 at 2 ms')
    expect(LEAKS.search('TRAP: failed Assert') and LEAKS.search('server process (PID 1) was terminated by signal 9') and not LEAKS.search('LOG:  checkpoint'), 'leak rules')
    o = Observer(None)
    o.active, o.lock, o.lwlock, o.events = 10, 1, 3, {'SiftRowLock': 2, 'WALInsert': 1}
    expect(o.metrics() == {'lock_wait_pct': 10.0, 'lwlock_wait_pct': 30.0, 'coarse_escalations': 0}, o.metrics())
    expect(o.top_waits() == 'SiftRowLock:2,WALInsert:1', o.top_waits())
    expect(rss_kb(-1) is None, 'no process, no rss')
