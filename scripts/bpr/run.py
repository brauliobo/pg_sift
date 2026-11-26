#!/usr/bin/env python3
"""The bytes-per-row gate: the disk and the WAL a path of sift writes for each row (cases.py, fixtures.sql).

A case is measured in windows: CHECKPOINT, the statements, then the WAL records of the window (pg_walinspect, without the
Standby and CHECKPOINT records of the background), the temp bytes and the size of the relation. The records that only touch
catalog relations (the pg_class rows of a CREATE INDEX: their full page images follow the churn of the catalog, not the rows)
are not counted. A window starts after a checkpoint, so every page a statement touches costs the same first full page image
in every run. The cluster must run with full_page_writes on, wal_compression, autovacuum and sift.async_maintenance off and
sift preloaded (test/t/core/storage_bpr.sh): the bytes do not depend on the platform or the PostgreSQL major. A case with
`dual_format` runs again with sift.test_create_format1 on (the testing library only) and F2 may not cost more than F1.

  scripts/bpr/run.py [--only RE]       one BPR line a case: BPR id [f2] measurements ... PASS | FAIL | OPEN=W-NN
  scripts/bpr/run.py --determinism     the cases forward, again and in reverse: the bytes agree within 0.5%
  scripts/bpr/run.py --calibrate       forward and reverse, and the ceilings the measurements propose (15% above)
  scripts/bpr/run.py --check-headroom  a clean case whose ceiling is more than 15% above what it measures fails
  scripts/bpr/run.py --list            the cases it would run
  scripts/bpr/run.py --selftest        the logic without a server

Reads PGHOST, PGPORT, PGUSER and PGDATABASE like psql; --dir is where the COPY files go (server and runner share it).
"""
from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, fields, replace

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cases as catalog  # noqa: E402
from cases import BprCase, Ceilings  # noqa: E402

PAGE = 8192
HEADROOM, NOISE_BPR, SPREAD = 0.15, 1.0, 0.005
REQUIRED = {'full_page_writes': 'on', 'wal_compression': 'off', 'autovacuum': 'off', 'sift.async_maintenance': 'off'}
SIFT_RMGRS = "('Generic', 'sift_table', 'sift_table_legacy', 'pg_sift_table_vm')"
WAL_SQL = f"""WITH r AS (SELECT '{{l0}}'::pg_lsn AS a, '{{l1}}'::pg_lsn AS b WHERE '{{l1}}'::pg_lsn > '{{l0}}'::pg_lsn),
  catalog AS (SELECT start_lsn FROM r, LATERAL pg_get_wal_block_info(r.a, r.b)
              GROUP BY start_lsn HAVING bool_and(relfilenode < 16384)),
  w AS (SELECT i.* FROM r, LATERAL pg_get_wal_records_info(r.a, r.b) i WHERE resource_manager <> 'Standby'
    AND NOT (resource_manager = 'XLOG' AND record_type LIKE 'CHECKPOINT%') AND start_lsn NOT IN (SELECT start_lsn FROM catalog))
SELECT coalesce(sum(record_length), 0), coalesce(sum(record_length) FILTER (WHERE resource_manager IN {SIFT_RMGRS}
    OR (resource_manager = 'XLOG' AND record_type LIKE 'FPI%')), 0), coalesce(sum(fpi_length), 0),
  coalesce(sum(record_length) FILTER (WHERE resource_manager IN ('Heap', 'Heap2')), 0), count(*) FROM w"""
TEMP_SQL = 'SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database()'
HOLD = re.compile(r'^-- @hold_snapshot: (.*)$', re.M)
OPEN_ID = re.compile(r'(W|HIST)-\d\d(,(W|HIST)-\d\d)*')
DISK_WAL = ('disk_bpr', 'total_disk_bpr', 'wal_bpr', 'sift_wal_bpr', 'fpi_bpr', 'idle_wal_bytes')


class Psql:
    """one psql process for the whole run: statements in, a marker line out, the first error raised"""

    def __init__(self, init: str = ''):
        env = dict(os.environ, PGOPTIONS='-c client_min_messages=error', LC_ALL='C')
        self.p = subprocess.Popen(['psql', '-X', '-At', '-q', '-v', 'ON_ERROR_STOP=0'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self.n, self.has_f1 = 0, False
        self.run(init)

    def run(self, sql: str) -> list:
        self.n += 1
        mark = f'BPR_DONE_{self.n}'
        self.p.stdin.write(f'{sql}\n;\n\\echo {mark}\n')
        self.p.stdin.flush()
        out = []
        for line in iter(self.p.stdout.readline, ''):
            if line.strip() == mark:
                break
            out.append(line.rstrip('\n'))
        else:
            raise RuntimeError('psql ended: ' + ' '.join(out[-3:]))
        err = next((l for l in out if re.match(r'(ERROR|FATAL|PANIC):', l)), None)
        if err:
            raise RuntimeError(f'{err} in: {sql.strip()[:100]}')
        return out

    def row(self, sql: str) -> list:
        return self.run(sql)[0].split('|')

    def val(self, sql: str) -> str:
        return self.row(sql)[0]

    def close(self):
        self.p.stdin.write('\\q\n')
        self.p.stdin.close()
        self.p.wait()


@dataclass(frozen=True)
class Win:
    wal: int
    sift: int
    fpi: int
    heap: int
    recs: int
    temp: int


@dataclass(frozen=True)
class Foot:
    table: int
    total: int
    used: int | None = None
    payload: int | None = None
    parts: int | None = None


@dataclass(frozen=True)
class BprMetrics:
    disk_bpr: float
    total_disk_bpr: float
    used_bpr: float | None
    payload_bpr: float | None
    alloc_overhead: float | None
    parts: int | None
    wal_bpr: float
    sift_wal_bpr: float
    fpi_bpr: float
    heap_wal_bpr: float
    wal_recs_per_row: float
    temp_bpr: float
    vs_base_disk: float | None = None
    vs_base_wal: float | None = None
    vs_f1_disk: float | None = None
    vs_f1_wal: float | None = None
    post_maint_disk_bpr: float | None = None
    maint_wal_bpr: float | None = None
    idle_wal_bytes: int | None = None


def ratio(a: float, b: float) -> float:
    return 1.0 if a == b else (a / b if b else math.inf)


def build(n: int, w: Win, f: Foot, total: bool = False, maint=None, maint_foot=None, idle=None) -> BprMetrics:
    sift = f.used is not None
    disk = lambda foot: (foot.total if total else foot.table) / n
    return BprMetrics(
        disk_bpr=disk(f), total_disk_bpr=f.total / n, used_bpr=f.used * PAGE / n if sift else None,
        payload_bpr=f.payload / n if sift else None, alloc_overhead=f.table / max(f.used * PAGE, 1) if sift else None,
        parts=f.parts, wal_bpr=w.wal / n, sift_wal_bpr=w.sift / n, fpi_bpr=w.fpi / n, heap_wal_bpr=w.heap / n,
        wal_recs_per_row=w.recs / n, temp_bpr=w.temp / n, post_maint_disk_bpr=disk(maint_foot) if maint_foot else None,
        maint_wal_bpr=maint.wal / n if maint else None, idle_wal_bytes=idle.wal if idle else None)


def window(db: Psql, sql: str) -> Win:
    db.run('CHECKPOINT; SELECT pg_stat_force_next_flush()')
    t0 = int(db.val(TEMP_SQL))
    l0 = db.val('SELECT pg_current_wal_insert_lsn()')
    db.run(sql)
    db.run('SELECT pg_stat_force_next_flush()')
    l1 = db.val('SELECT pg_current_wal_insert_lsn()')
    t1 = int(db.val(TEMP_SQL))
    db.run("SELECT pg_logical_emit_message(false, 'bpr', '', true)")
    if db.val(f"SELECT pg_current_wal_flush_lsn() >= '{l1}'") != 't':
        raise RuntimeError(f'WAL not flushed to {l1}')
    return Win(*map(int, db.row(WAL_SQL.format(l0=l0, l1=l1))), temp=t1 - t0)


def footprint(db: Psql, rel: str, sift: bool) -> Foot:
    table, total = map(int, db.row(f"SELECT pg_table_size('{rel}'), pg_total_relation_size('{rel}')"))
    if not sift:
        return Foot(table, total)
    return Foot(table, total, *map(int, db.row(f"SELECT used_blocks, bytes, parts FROM sift.space('{rel}')")))


def split_hold(setup: str):
    return HOLD.sub('', setup), HOLD.findall(setup)


def run_pass(db: Psql, case: BprCase, fmt: str, tmp: str):
    sub = lambda q: q.replace('{dir}', tmp)
    db.run('DROP SCHEMA IF EXISTS bpr_case CASCADE; CREATE SCHEMA bpr_case; SET search_path = bpr_case, public')
    if db.has_f1:
        db.run(f"SET sift.test_create_format1 = {'on' if fmt == 'f1' else 'off'}")
    db.run(case.settings)
    setup, holds = split_hold(sub(case.setup))
    db.run(setup)
    snaps = [Psql(f'SET search_path = bpr_case, public; BEGIN ISOLATION LEVEL REPEATABLE READ; {h}') for h in holds]
    win = window(db, sub(case.op))
    foot = footprint(db, case.rel, True)
    for s in snaps:
        s.close()
    bad = []
    if db.has_f1:
        major = int(db.val(f"SELECT format_major FROM sift.status('{case.rel}')"))
        if major != (1 if fmt == 'f1' else 2):
            bad.append(f'format_major {major} is not the format of the {fmt} pass')
    maint = window(db, sub(case.maint)) if case.maint else None
    maint_foot = footprint(db, case.rel, True) if case.maint else None
    idle = window(db, sub(case.idle)) if case.idle else None
    problems = int(db.val(f"SELECT count(*) FROM sift.check('{case.rel}', true)")) if case.verify else 0
    if problems:
        bad.append(f'sift.check found {problems} problems')
    m = build(case.rows, win, foot, case.include_indexes, maint, maint_foot, idle)
    if fmt == 'f2' and case.base_op:
        db.run(sub(case.base_setup))
        base = build(case.rows, window(db, sub(case.base_op)), footprint(db, case.base_rel, False))
        m = replace(m, vs_base_disk=ratio(m.disk_bpr, base.disk_bpr), vs_base_wal=ratio(m.wal_bpr, base.wal_bpr))
    db.run('DROP SCHEMA bpr_case CASCADE; RESET ALL')
    return m, bad


def measure(db: Psql, case: BprCase, tmp: str):
    m, bad = run_pass(db, case, 'f2', tmp)
    if case.dual_format and db.has_f1:
        f1, _ = run_pass(db, case, 'f1', tmp)
        m = replace(m, vs_f1_disk=ratio(m.disk_bpr, f1.disk_bpr), vs_f1_wal=ratio(m.wal_bpr, f1.wal_bpr))
    return m, bad


def effective(case: BprCase) -> Ceilings:
    parity = ('vs_f1_disk', 'vs_f1_wal') if case.dual_format else ()
    return replace(case.limits, **{k: 1.0 for k in parity if getattr(case.limits, k) is None})


def violations(lim: Ceilings, m: BprMetrics) -> list:
    return [f'{f.name} {v:.2f}>{c:g}' for f in fields(Ceilings) if (c := getattr(lim, f.name)) is not None
            and (v := getattr(m, f.name)) is not None and v > c + 1e-9]


def judge(case: BprCase, m: BprMetrics, bad: list):
    over = violations(effective(case), m) + bad
    unjudged = case.dual_format and m.vs_f1_disk is None
    if case.open and not over and not unjudged:
        return 'FAIL', [f'meets every ceiling: drop open={case.open}']
    return ('OPEN=' + case.open if case.open else 'FAIL' if over else 'PASS'), over


def cap(name: str, v: float) -> float:
    return max(v * (1 + HEADROOM), v + NOISE_BPR) if name.endswith('_bpr') else v * (1 + HEADROOM)


def headroom_excess(lim: Ceilings, m: BprMetrics) -> list:
    return [f'{f.name} ceiling {c:g} is above {cap(f.name, v):.2f} (measured {v:.2f})' for f in fields(Ceilings)
            if not f.name.startswith('vs_') and (c := getattr(lim, f.name)) and (v := getattr(m, f.name)) is not None
            and c > cap(f.name, v)]


def line(case: BprCase, m: BprMetrics, verdict: str, over: list) -> str:
    lim = effective(case)

    def cell(name: str, fmt: str = '.1f') -> str:
        ceiling = getattr(lim, name)
        return f'{getattr(m, name):{fmt}}' + ('' if ceiling is None else f'/{ceiling:g}')

    parts = [f'BPR {case.id} [f2] disk={cell("disk_bpr")} wal={cell("wal_bpr")} sift={m.sift_wal_bpr:.1f}',
             f'fpi={m.fpi_bpr:.1f} heap={m.heap_wal_bpr:.1f} rec={m.wal_recs_per_row:.3f} tmp={m.temp_bpr:.1f}']
    if m.used_bpr is not None:
        parts.append(f'used={m.used_bpr:.1f}({m.alloc_overhead:.2f}x)')
    for tag, d, w in (('vs_f1', m.vs_f1_disk, m.vs_f1_wal), ('vs_base', m.vs_base_disk, m.vs_base_wal)):
        if d is not None:
            parts.append(f'{tag}={d:.2f}d/{w:.2f}w')
    if m.maint_wal_bpr is not None:
        parts.append(f'maint={cell("maint_wal_bpr")}w>{m.post_maint_disk_bpr:.1f}d')
    if m.idle_wal_bytes is not None:
        parts.append(f'idle={m.idle_wal_bytes}B' + ('' if lim.idle_wal_bytes is None else f'/{lim.idle_wal_bytes:g}B'))
    return ' '.join(parts + ([f'[{"; ".join(over)}]'] if over else []) + [verdict])


def spread(runs: list, case_id: str) -> dict:
    out = {}
    for name in ('disk_bpr', 'wal_bpr', 'fpi_bpr', 'maint_wal_bpr', 'idle_wal_bytes'):
        vals = [v for r in runs if (v := getattr(r[case_id][0], name)) is not None]
        out[name] = (max(vals) - min(vals)) / max(max(vals), 1e-9) if vals else 0.0
    return out


def proposals(m: BprMetrics) -> dict:
    return {f.name: 0 if f.name in ('temp_bpr', 'idle_wal_bytes') else math.ceil(cap(f.name, v) * 100 - 1e-9) / 100
            for f in fields(Ceilings) if not f.name.startswith('vs_') and (v := getattr(m, f.name)) is not None}


def catalog_problems(cases) -> list:
    out, seen = [], set()
    for c in cases:
        lim, parity = c.limits, (c.limits.vs_f1_disk, c.limits.vs_f1_wal)
        sections = {'post_maint_disk_bpr': c.maint, 'maint_wal_bpr': c.maint, 'idle_wal_bytes': c.idle,
                    'vs_base_disk': c.base_op, 'vs_base_wal': c.base_op}
        rules = [(c.id not in seen, 'duplicate id'),
                 (c.engine in catalog.ENGINES, f'engine {c.engine}'),
                 (any(getattr(lim, n) is not None for n in DISK_WAL), 'no disk or WAL ceiling'),
                 (c.engine == 'bm25' or lim.temp_bpr == 0.0, 'a Format 2.0 case must hold temp_bpr at 0.0'),
                 (not c.open or OPEN_ID.fullmatch(c.open), f'open={c.open!r} is not W-NN or HIST-NN'),
                 (c.dual_format or parity == (None, None), 'vs_f1 needs dual_format'),
                 (c.open or all(v is None or v <= 1.05 for v in parity), 'vs_f1 above 1.05 needs open')]
        out += [f'{c.id}: {msg}' for ok, msg in rules if not ok]
        out += [f'{c.id}: {k} needs its SQL section' for k, sql in sections.items() if getattr(lim, k) is not None and not sql]
        seen.add(c.id)
    return out


def sweep(db: Psql, selected: list, tmp: str, reverse: bool = False) -> dict:
    return {c.id: measure(db, c, tmp) for c in (reversed(selected) if reverse else selected)}


def connect(tmp: str) -> Psql:
    db = Psql()
    got = {k: db.val(f"SHOW {k}") for k in REQUIRED}
    if got != REQUIRED or 'pg_sift' not in db.val('SHOW shared_preload_libraries'):
        raise RuntimeError(f'the cluster must run with {REQUIRED} and pg_sift preloaded, it runs with {got}')
    db.run('CREATE EXTENSION IF NOT EXISTS pg_sift; CREATE EXTENSION IF NOT EXISTS pg_walinspect')
    db.has_f1 = db.val("SELECT count(*) FROM pg_settings WHERE name = 'sift.test_create_format1'") == '1'
    db.run(f'\\set rows {catalog.N}\n\\i {HERE}/fixtures.sql')
    db.run(f"COPY bpr_base_wide TO '{tmp}/copy_wide.tsv'")
    return db


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def selftest():
    expect(not catalog_problems(catalog.CASES), catalog_problems(catalog.CASES))
    c0 = catalog.CASES[0]
    bad = [replace(c0, id='x', engine='nope'), replace(c0, id='y', limits=Ceilings()), replace(c0, id='z', open='W-1'),
           replace(c0, id='w', limits=Ceilings(wal_bpr=1, idle_wal_bytes=0)), replace(c0, id='v', limits=Ceilings(wal_bpr=1, temp_bpr=None))]
    expect(len(catalog_problems(bad)) == 5, catalog_problems(bad))
    expect(catalog_problems([c0, c0]) == [f'{c0.id}: duplicate id'], 'duplicate ids')
    expect(split_hold('a\n-- @hold_snapshot: SELECT 1\nb') == ('a\n\nb', ['SELECT 1']), 'hold_snapshot directive')
    expect((ratio(0, 0), ratio(1, 2), ratio(1, 0)) == (1.0, 0.5, math.inf), 'ratio guards')
    w = Win(wal=2048, sift=1024, fpi=512, heap=0, recs=4, temp=0)
    m = build(1024, w, Foot(table=16384, total=32768, used=1, payload=8192, parts=2), maint=w, maint_foot=Foot(8192, 8192), idle=Win(0, 0, 0, 0, 0, 0))
    expect((m.disk_bpr, m.total_disk_bpr, m.used_bpr, m.alloc_overhead) == (16.0, 32.0, 8.0, 2.0), m)
    expect((m.wal_bpr, m.sift_wal_bpr, m.fpi_bpr, m.wal_recs_per_row, m.temp_bpr) == (2.0, 1.0, 0.5, 4 / 1024, 0.0), m)
    expect((m.maint_wal_bpr, m.post_maint_disk_bpr, m.idle_wal_bytes) == (2.0, 8.0, 0), m)
    expect(build(1024, w, Foot(16384, 32768), total=True).disk_bpr == 32.0 and build(1024, w, Foot(1, 1)).used_bpr is None, 'footprint basis')
    case = replace(c0, limits=Ceilings(disk_bpr=16, wal_bpr=2))
    expect(judge(case, m, []) == ('PASS', []), judge(case, m, []))
    expect(judge(case, m, ['x'])[0] == 'FAIL' and judge(replace(case, limits=Ceilings(wal_bpr=1.9)), m, [])[0] == 'FAIL', 'a ceiling is a hard limit')
    debt = replace(case, open='W-01', limits=Ceilings(idle_wal_bytes=0, wal_bpr=1))
    expect(judge(debt, m, [])[0] == 'OPEN=W-01', 'an open case that fails its ceilings is the debt')
    expect(judge(replace(debt, limits=Ceilings(wal_bpr=3)), m, []) == ('FAIL', ['meets every ceiling: drop open=W-01']), 'a fixed open case fails')
    dual = replace(case, dual_format=True)
    expect(judge(replace(dual, open='HIST-34'), m, [])[0] == 'OPEN=HIST-34', 'no F1 pass: the parity debt stays open')
    expect(judge(dual, replace(m, vs_f1_wal=1.2), [])[0] == 'FAIL' and judge(dual, replace(m, vs_f1_wal=0.9), [])[0] == 'PASS', 'F2 <= F1')
    expect(not headroom_excess(Ceilings(disk_bpr=18.4, wal_bpr=3.0), m) and len(headroom_excess(Ceilings(disk_bpr=19, wal_bpr=3.1), m)) == 2, 'headroom')
    expect(not headroom_excess(Ceilings(vs_f1_wal=1.0), replace(m, vs_f1_wal=0.1)), 'ratios have policy ceilings')
    expect(proposals(m)['disk_bpr'] == 18.4 and proposals(m)['temp_bpr'] == 0 and proposals(m)['wal_bpr'] == 3.0, proposals(m))
    out = line(case, m, 'PASS', [])
    expect(out.startswith(f'BPR {c0.id} [f2] disk=16.0/16 wal=2.0/2') and out.endswith(' PASS') and 'idle=0B' in out, out)
    runs = [{'a': (replace(m, wal_bpr=v), [])} for v in (2.0, 2.005, 2.0)]
    expect(spread(runs, 'a')['wal_bpr'] < SPREAD * 0.99 and spread(runs[:2] + [{'a': (replace(m, wal_bpr=2.1), [])}], 'a')['wal_bpr'] > SPREAD, 'spread')
    print(f'selftest ok: {len(catalog.CASES)} cases')


def report(a, selected: list, runs: list) -> int:
    failed = 0
    for c in selected:
        m, bad = runs[0][c.id]
        verdict, over = judge(c, m, bad)
        failed += verdict == 'FAIL'
        if a.determinism or a.calibrate:
            sp = spread(runs, c.id)
            vary = max(sp.values()) > SPREAD
            failed += vary
            print(f'BPR_SPREAD {c.id} ' + ' '.join(f'{k}={v * 100:.2f}%' for k, v in sp.items()) + (' VARY' if vary else ' OK'))
        if a.calibrate:
            print(f'BPR_CAL {c.id} ' + ' '.join(f'{k}={v:g}' for k, v in proposals(m).items()))
        if a.check_headroom:
            excess = [] if c.open else headroom_excess(c.limits, m)
            failed += bool(excess)
            print(f'BPR_HEADROOM {c.id} ' + ('; '.join(excess) or 'ok'))
        print(line(c, m, verdict, over))
    print(f'BPR_OPEN {sum(bool(c.open) for c in selected)} ' + ','.join(c.open for c in selected if c.open))
    return int(failed > 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--only', default='')
    ap.add_argument('--dir', default='')
    for flag in ('list', 'calibrate', 'determinism', 'check-headroom', 'selftest'):
        ap.add_argument('--' + flag, action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest() or 0
    selected = [c for c in catalog.CASES if re.search(a.only, c.id)]
    if a.list:
        return print(*(c.id for c in selected), sep='\n') or 0
    tmp = a.dir or tempfile.mkdtemp(prefix='bpr-')
    os.chmod(tmp, 0o777)
    db = connect(tmp)
    try:
        if not db.has_f1:
            print('BPR_INFO release build: dual-format F1 comparison unavailable')
        runs = [sweep(db, selected, tmp)]
        if a.determinism:
            runs.append(sweep(db, selected, tmp))
        if a.determinism or a.calibrate:
            runs.append(sweep(db, selected, tmp, reverse=True))
        return report(a, selected, runs)
    finally:
        db.close()
        if not a.dir:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
