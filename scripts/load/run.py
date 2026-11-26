#!/usr/bin/env python3
"""The load gate: sift under concurrent writers, readers, pooled sessions, maintenance and faults, judged by counts that do not depend on the host (cases.py, fixtures.sql).

Every case runs in the schema load_case of one cluster: worker sessions (persistent psql processes, application_name load_w*) released together by a barrier, a sampler of their
waits (collect.py), and then the checks no load may break: the rows the workers committed are the rows the tables hold (lost_rows, dup_rows), the index paths answer what the heap
answers in one snapshot (oracle_mismatch), sift.check finds nothing, nothing is quarantined, no temporary file and no crash or leak line is left. The contention ceilings are shares
of samples and ratios of a p99 under load to the p99 of the same statements alone: never a time or a rate. A case with `open` (LC-NN of Spec 106, S107-NN of Spec 107) names the
defect it waits for: it must fail a ceiling until the fix (OPEN=ID, a TODO) and the run fails once it meets them all, until `open` is dropped. --suite picks the catalog: core
(cases.py, one cluster), workloads and repl (suites/: a cluster sized for many sessions and prepared transactions, a primary with a hot standby and wal_level logical), or all.

  scripts/load/run.py [--suite S] [--only RE] [--class C]   one LOAD line a case: LOAD id [f2] measurements ... PASS | FAIL | OPEN=ID | SKIP
  scripts/load/run.py --determinism             the cases forward, again and in reverse: every verdict agrees
  scripts/load/run.py --calibrate               forward and reverse, and the ceilings the ratios propose (the worst run, 50% above)
  scripts/load/run.py --list                    the cases it would run (for the format of the cluster, --format 1|2 for another)
  scripts/load/run.py --selftest                the logic without a server

Reads PGHOST, PGPORT and PGUSER like psql; --log is the server log (the crash and leak lines of each case), --no-rss skips the anonymous memory of the backends (no /proc),
LOAD_CRASH and LOAD_RESTART are the shell commands that kill -9 the server and start it again (the last case, and a case that crashed it); LOAD_STANDBY_HOST and LOAD_STANDBY_PORT
name the hot standby of the repl suite, LOAD_PROMOTE and LOAD_RESTANDBY promote it and clone a new one. --format 1|2 runs the cases on that format (the testing library: sift.test_create_format1).
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field, fields, replace

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cases as catalog  # noqa: E402
import collect  # noqa: E402
import suites  # noqa: E402
from cases import Ceilings, LoadCase  # noqa: E402

ERR = re.compile(r'^(ERROR|FATAL|PANIC): +([0-9A-Z]{5}): (.*)')
LOST = ('57P01', '08006')
OPEN_ID = re.compile(r'(LC-(0[1-9]|[1-4]\d)|S107-(0[1-9]|[1-3]\d))(,(LC-(0[1-9]|[1-4]\d)|S107-(0[1-9]|[1-3]\d)))*')
FLOORS = ('scale_ratio',)
SKIP_FORMAT, SKIP_TESTING = 'format 2.0 only', 'needs testing library'
GONE = "SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE 'load_w%'"


class Psql:
    """one psql process: statements in, a marker line out; run() raises the first error, q() returns it with the SQLSTATE"""

    def __init__(self, init: str = '', app: str = 'load_ctl', db: str = 'postgres', conn: dict | None = None):
        self.n, self.init, self.app, self.dbname, self.conn, self.p = 0, init, app, db, conn or {}, None
        self.open()

    def open(self):
        env = dict(os.environ, PGOPTIONS=os.environ.get('LOAD_PGOPTIONS', '') + ' -c client_min_messages=error', LC_ALL='C',
                   PGAPPNAME=self.app, PGDATABASE=self.dbname, **self.conn)
        self.p = subprocess.Popen(['psql', '-X', '-At', '-q', '-v', 'ON_ERROR_STOP=0', '-v', 'VERBOSITY=verbose'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self._pid, self.dead = None, False
        if self.init:
            _, err, _ = self.q(self.init)
            if err and not self.dead:
                raise RuntimeError(f'{err[0]} {err[1]} in the start of a session: {self.init[:120]}')

    def q(self, sql: str):
        """(lines, (sqlstate, message) or None, ms); a session the server ended is opened again by the next statement"""
        if self.dead:
            self.open()
        if self.dead:
            return [], ('08006', 'no server'), 0.0
        self.n += 1
        mark, out, t0 = f'LOAD_DONE_{self.n}', [], time.monotonic()
        try:
            self.p.stdin.write(f'{sql}\n;\n\\echo {mark}\n')
            self.p.stdin.flush()
            for line in iter(self.p.stdout.readline, ''):
                if line.strip() == mark:
                    break
                out.append(line.rstrip('\n'))
            else:
                raise BrokenPipeError
        except (BrokenPipeError, OSError):
            err = next((m.groups()[1:] for l in out if (m := ERR.match(l))), ('08006', 'the server closed the session'))
            self.p.wait()
            self.dead = True
            return [], err, (time.monotonic() - t0) * 1000
        err = next((m.groups()[1:] for l in out if (m := ERR.match(l))), None)
        return out, err, (time.monotonic() - t0) * 1000

    def run(self, sql: str) -> list:
        out, err, _ = self.q(sql)
        if err:
            raise RuntimeError(f'{err[0]} {err[1]} in: {sql.strip()[:120]}')
        return out

    def row(self, sql: str) -> list:
        out = self.run(sql)
        if not out:
            raise RuntimeError(f'no row in: {sql.strip()[:100]}')
        return out[0].split('|')

    def val(self, sql: str) -> str:
        return self.row(sql)[0]

    @property
    def pid(self) -> int:
        self._pid = self._pid or int(self.val('SELECT pg_backend_pid()'))
        return self._pid

    def close(self):
        if self.p.poll() is None:
            try:
                self.p.stdin.write('\\q\n')
                self.p.stdin.close()
            except BrokenPipeError:
                pass
        self.p.wait()


class Worker(Psql):
    """a session under load: do() times a statement, counts its errors (the SQLSTATEs in allow are expected) and keeps the unexpected ones"""

    def __init__(self, init: str, app: str, db: str = 'postgres', seed: int = 0, conn: dict | None = None):
        self.seed, self.lat, self.ok, self.expected, self.bad, self.ledger, self.last = seed, [], 0, 0, [], 0, None
        super().__init__(init, app, db, conn)

    def do(self, sql: str, allow: tuple = ()):
        out, err, ms = self.q(sql)
        self.last = err
        if err is None:
            self.ok += 1
            self.lat.append(ms)
            return out
        if err[0] in allow or (err[0] in LOST and set(LOST) & set(allow)):
            self.expected += 1
        else:
            self.bad.append(f'{err[0]} {err[1][:100]}')
        return None


@dataclass
class Role:
    ops: int = 0
    expected: int = 0
    lat: list = field(default_factory=list)
    bad: list = field(default_factory=list)


class Ctx:
    """what a case body uses: the control session (db), worker sessions, phases under the sampler and the facts of the cluster"""

    def __init__(self, a, db: Psql):
        self.a, self.db, self.open_sessions, self.obs, self.obs_db = a, db, [], None, Psql(app='load_obs')
        self.testing = db.val("SELECT count(*) FROM pg_settings WHERE name = 'sift.test_create_format1'") == '1'
        self.fmt = 1 if self.testing and db.val('SHOW sift.test_create_format1') == 'on' else 2
        self.assert_build = db.val('SHOW debug_assertions') == 'on'
        self.cores = os.cpu_count() or 1
        self.no_rss = a.no_rss or self.assert_build or not os.path.exists(f'/proc/{db.pid}/smaps_rollup')
        self.cache_kb = int(db.val("SELECT setting FROM pg_settings WHERE name = 'sift.backend_cache_kb'"))

    def sessions(self, n: int, init: str = '', db: str = 'postgres', path: str = 'load_case, public', conn: dict | None = None) -> list:
        ws = [Worker(f'SET search_path = {path}; {init}', f'load_w{i}', db, seed=i + 1, conn=conn) for i in range(n)]
        self.open_sessions += ws
        return ws

    def phase(self, roles: dict, secs: float, peak: str = '', pacer: bool = False, lead: str = '') -> dict:
        """runs every (workers, fn) of ROLES together for SECS, or until the workers of the role LEAD end (a fn that returns False ends its worker): fn(worker, i) does statement i"""
        stop, pairs = threading.Event(), [(name, w, fn) for name, (ws, fn) in roles.items() for w in ws]
        bar = threading.Barrier(len(pairs) + 1)
        for _, w, _ in pairs:
            w.lat, w.ok, w.expected, w.bad = [], 0, 0, []

        def loop(w, fn):
            bar.wait()
            i = 0
            while not stop.is_set() and fn(w, i) is not False:
                i += 1

        threads = [threading.Thread(target=loop, args=(w, fn), daemon=True) for _, w, fn in pairs]
        for t in threads:
            t.start()
        self.obs = collect.Observer(self.obs_db, peak, pacer)
        bar.wait()
        self.obs.start()
        end = time.monotonic() + secs * self.a.scale
        for t, (name, _, _) in zip(threads, pairs):
            if not lead or name == lead:
                t.join(max(0.0, end - time.monotonic()))
        stop.set()
        for t in threads:
            t.join()
        self.obs.stop()
        out = {name: Role() for name in roles}
        for name, w, _ in pairs:
            r = out[name]
            r.ops, r.expected, r.lat, r.bad = r.ops + w.ok, r.expected + w.expected, r.lat + w.lat, r.bad + w.bad
        return out

    def rss(self, ws: list):
        return None if self.no_rss else [collect.rss_kb(w.pid) for w in ws]

    def rss_mb_delta(self, before, after):
        if before is None or after is None or None in before + after:
            return None
        return round(max(b - a for a, b in zip(before, after)) / 1024, 1)

    def hook(self, var: str):
        cmd = os.environ.get(var)
        if not cmd:
            raise RuntimeError(f'the case needs {var}: the shell command of the test')
        subprocess.run(['bash', '-c', cmd], check=True, stdout=subprocess.DEVNULL)

    def restart(self, *hooks: str):
        """runs the shell HOOKS (LOAD_CRASH, LOAD_RESTART) and opens the control and sampler sessions again on the server they leave"""
        for h in hooks:
            self.hook(h)
        for db in (self.db, self.obs_db):
            db.close()
            db.dead = True
        self.db.run('SET search_path = load_case, public')

    def close_sessions(self):
        """ends the worker sessions and waits until their backends have exited (their memory and their temporary files are given back at exit)"""
        for w in self.open_sessions:
            w.close()
        self.open_sessions = []
        end = time.monotonic() + 30
        while time.monotonic() < end:
            out, err, _ = self.db.q(GONE)
            if err or out == ['0']:
                return
            time.sleep(0.01)


def runs_on(case: LoadCase, ctx: Ctx):
    if ctx.fmt not in case.formats:
        return SKIP_FORMAT
    return SKIP_TESTING if case.testing and not ctx.testing else ''


def measure(ctx: Ctx, case: LoadCase, log: collect.LogScan):
    skip = runs_on(case, ctx)
    if skip:
        return None, [skip]
    db = ctx.db
    try:
        db.run('DROP SCHEMA IF EXISTS load_case CASCADE; CREATE SCHEMA load_case; SET search_path = load_case, public')
        busy = catalog.until(db, catalog.DRAINED, 60.0 * ctx.a.scale)
        db.run('SELECT pg_stat_force_next_flush()')
        dl0 = int(db.val('SELECT deadlocks FROM pg_stat_database WHERE datname = current_database()'))
        log.mark()
        m = case.body(ctx)
        ctx.close_sessions()
        db.run('SELECT pg_stat_force_next_flush()')
        m['deadlocks'] = int(db.val('SELECT deadlocks FROM pg_stat_database WHERE datname = current_database()')) - dl0 - m.pop('deadlocks_meant', 0)
        m.update({k: v for k, v in collect.hygiene(db, m.pop('damaged', ())).items() if k not in m})
        found = log.findings()
        m['traps_and_leaks'] = len(found)
        bad = m.pop('notes', []) + found[:3]
        m['shown_notes'] = [b for b in bad if b.startswith('note:')]
        m['shown_notes'] += ['note: the maintenance of the case before did not drain in 60 s'] if busy else []
        db.run('DROP SCHEMA load_case CASCADE; RESET ALL; SET search_path = load_case, public')
        return m, bad
    except (RuntimeError, OSError) as e:
        ctx.close_sessions()
        db.close()
        ctx.db = reconnect(ctx)
        return None, [str(e)[:300]]


def reconnect(ctx: Ctx) -> Psql:
    """a control session on a server that runs: a case that crashed it (a backend killed by a signal) gets a fresh start from LOAD_RESTART, as a supervisor gives one"""
    ctx.db = Psql()
    if ctx.db.q('SET search_path = load_case, public')[1]:
        ctx.restart('LOAD_RESTART')
    return ctx.db


def effective(case: LoadCase, ctx: Ctx | None) -> Ceilings:
    """the scale floor holds only where it can be met: 4 cores or more and no assertions; it is then at most half the writers"""
    lim = case.limits
    if lim.scale_ratio is None or ctx is None:
        return lim
    if ctx.cores < 4 or ctx.assert_build:
        return replace(lim, scale_ratio=None)
    return replace(lim, scale_ratio=min(lim.scale_ratio, 0.45 * catalog.writers(ctx.cores)))


def violations(lim: Ceilings, m: dict) -> list:
    out = []
    for f in fields(lim):
        c, v = getattr(lim, f.name), m.get(f.name)
        if c is None or v is None:
            continue
        if (v < c - 1e-9) if f.name in FLOORS else (v > c + 1e-9):
            out.append(f'{f.name} {v:g}{"<" if f.name in FLOORS else ">"}{c:g}')
    return out


def judge(case: LoadCase, m: dict | None, bad: list, lim: Ceilings | None = None):
    if m is None:
        skipped = bool(bad) and bad[0] in (SKIP_FORMAT, SKIP_TESTING)
        return ('SKIP' if skipped else 'OPEN=' + case.open if case.open else 'FAIL'), bad
    over = violations(lim or case.limits, m) + [b for b in bad if not b.startswith('note:')]
    if case.open and not over:
        return ('OPEN=' + case.open, ['note: not met in this run']) if case.intermittent else ('FAIL', [f'meets every ceiling: drop open={case.open}'])
    return ('OPEN=' + case.open if case.open and over else 'FAIL' if over else 'PASS'), over


SHOWN = (('ops', 'ops', 'd'), ('lost_rows', 'lost', 'd'), ('dup_rows', 'dup', 'd'), ('oracle_mismatch', 'orc', 'd'), ('unexpected_errors', 'err', 'd'),
         ('verify_errors', 'chk', 'd'), ('false_quarantines', 'q', 'd'), ('lock_wait_pct', 'lock', '.1f'), ('lwlock_wait_pct', 'lw', '.1f'),
         ('coarse_escalations', 'esc', 'd'), ('p99_ratio', 'rp99', '.2f'), ('scale_ratio', 'scale', '.2f'), ('ctx_kb_delta', 'ctx', 'd'),
         ('rss_mb_delta', 'rss', '.1f'), ('shm_used_after', 'shm', 'd'), ('tmp_files_left', 'tmp', 'd'), ('deadlocks', 'dl', 'd'),
         ('peak', 'peak', 'd'), ('l0_per_kop', 'l0k', '.1f'), ('pending_after', 'pend', 'd'), ('quarantine_missed', 'qmiss', 'd'), ('not_restarted', 'norestart', 'd'), ('meta_corrupt', 'meta', 'd'), ('unconverged', 'unconv', 'd'), ('maint_failures', 'mf', 'd'), ('traps_and_leaks', 'trap', 'd'))


def line(case: LoadCase, ctx: Ctx, m: dict | None, verdict: str, over: list, secs: float = 0.0) -> str:
    tag = f'[f{ctx.fmt}]'
    if m is None:
        return f'LOAD {case.id} {tag} {"; ".join(over)} {verdict}'
    lim, cells = effective(case, ctx), []
    for key, short, fmt in SHOWN:
        if m.get(key) is None:
            continue
        c = getattr(lim, key, None)
        cells.append(f'{short}={m[key]:{fmt}}' + ('' if c is None else f'/{c:g}'))
    extra = [f'{k}={v}' for k, v in m.items() if k not in {s[0] for s in SHOWN} and isinstance(v, (int, float, str))]
    notes = [f'[{"; ".join(over)}]'] if over else []
    notes += [f'({"; ".join(n[6:] for n in m.get("shown_notes", []))})'] if m.get('shown_notes') else []
    return ' '.join([f'LOAD {case.id} {tag} t={secs:.1f}s'] + cells + extra + notes + [verdict])


def sweep(ctx: Ctx, selected: list, log: collect.LogScan, reverse: bool = False) -> dict:
    order = list(reversed(selected)) if reverse else selected
    order = [c for c in order if not c.last] + [c for c in order if c.last]
    out = {}
    for c in order:
        t0 = time.monotonic()
        m, bad = measure(ctx, c, log)
        out[c.id] = (m, bad, time.monotonic() - t0)
    return out


def catalog_problems(cases, classes: tuple = catalog.CLASSES) -> list:
    out, seen = [], set()
    for c in cases:
        rules = [(c.id not in seen, 'duplicate id'), (c.cls in classes, f'class {c.cls}'), (c.id.startswith(c.cls + '_'), 'id does not start with its class'),
                 (not c.open or OPEN_ID.fullmatch(c.open), f'open={c.open!r} is not LC-NN or S107-NN'), (set(c.formats) <= {1, 2} and c.formats, 'formats'),
                 (all(getattr(c.limits, k) == 0 for k in catalog.HARD), 'a correctness ceiling above 0'), (c.open or not c.intermittent, 'intermittent needs open')]
        out += [f'{c.id}: {msg}' for ok, msg in rules if not ok]
        seen.add(c.id)
    return out


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def selftest():
    expect(not catalog_problems(catalog.CASES), catalog_problems(catalog.CASES))
    expect(len(catalog.CASES) == 17 and {c.cls for c in catalog.CASES} == set(catalog.CLASSES), 'sixteen cases in every class')
    expect(sum(c.last for c in catalog.CASES) == 1 and catalog.CASES[-1].last, 'the crash case runs last')
    c0 = LoadCase('conc_x', 'conc', lambda ctx: {}, Ceilings(lock_wait_pct=10))
    bad = [replace(c0, id='nope_x'), replace(c0, id='conc_y', open='LC-1'), replace(c0, id='conc_z', limits=Ceilings(lost_rows=1)), replace(c0, id='conc_w', formats=())]
    expect(len(catalog_problems(bad)) == 4, catalog_problems(bad))
    expect(catalog_problems([c0, c0]) == ['conc_x: duplicate id'], 'duplicate ids')
    for name, (cases, classes) in suites.SUITES.items():
        expect(not catalog_problems(cases, classes) and {c.cls for c in cases} == set(classes), f'suite {name}: {catalog_problems(cases, classes)}')
    every = suites.cases('all')
    expect(not catalog_problems(every, suites.CLASSES) and len(every) == 27, 'the suites share no id: 17 core, 8 workloads, 2 repl')
    suites.selftest(expect)
    m = {'lost_rows': 0, 'lock_wait_pct': 5.0, 'scale_ratio': 3.0}
    expect(judge(c0, m, []) == ('PASS', []), judge(c0, m, []))
    expect(judge(c0, dict(m, lost_rows=1), [])[0] == 'FAIL' and judge(c0, dict(m, lock_wait_pct=11), [])[0] == 'FAIL', 'a ceiling is a hard limit')
    expect(judge(c0, m, ['note: x'])[0] == 'PASS' and judge(c0, m, ['42P01 boom'])[0] == 'FAIL', 'a note is no failure, an error is')
    debt = replace(c0, open='LC-04')
    expect(judge(debt, dict(m, lock_wait_pct=50), [])[0] == 'OPEN=LC-04', 'an open case that fails its ceilings is the debt')
    expect(judge(debt, m, []) == ('FAIL', ['meets every ceiling: drop open=LC-04']), 'a fixed open case fails')
    expect(judge(replace(debt, intermittent=True), m, [])[0] == 'OPEN=LC-04' and catalog_problems([replace(c0, id='conc_i', intermittent=True)]), 'an intermittent case stays open')
    expect(judge(c0, None, [SKIP_FORMAT])[0] == 'SKIP' and judge(c0, None, ['boom'])[0] == 'FAIL', 'a skipped case is not a failed one')
    expect(judge(debt, None, ['08006 the server closed the session'])[0] == 'OPEN=LC-04', 'an open case that crashes is still the debt')
    expect(OPEN_ID.fullmatch('LC-19,LC-21,LC-42') and not OPEN_ID.fullmatch('LC-50') and not OPEN_ID.fullmatch('LC-00'), 'LC-01..LC-49')
    expect(OPEN_ID.fullmatch('S107-01,LC-43,S107-39') and not OPEN_ID.fullmatch('S107-40') and not OPEN_ID.fullmatch('S107-1'), 'S107-01..S107-39')
    floor = Ceilings(scale_ratio=2.5)
    expect(violations(floor, {'scale_ratio': 2.0}) == ['scale_ratio 2<2.5'] and not violations(floor, {'scale_ratio': 2.6}), 'scale is a floor')
    expect(not violations(Ceilings(), {'rss_mb_delta': None, 'ctx_kb_delta': None}), 'n/a is not judged')
    fake = type('C', (), {'cores': 2, 'assert_build': False})()
    expect(effective(replace(c0, limits=floor), fake).scale_ratio is None, 'no scale floor on 2 cores')
    fake.cores = 32
    expect(effective(replace(c0, limits=floor), fake).scale_ratio == min(2.5, 0.45 * catalog.writers(32)), 'core-aware floor')
    fake.assert_build = True
    expect(effective(replace(c0, limits=floor), fake).scale_ratio is None, 'no scale floor with assertions')
    fake.fmt = 1
    expect(runs_on(replace(c0, formats=(2,)), fake) == SKIP_FORMAT, 'an F2 case skips on Format 1')
    expect(ERR.match('ERROR:  57014: canceling statement due to statement timeout').groups()[1] == '57014', 'sqlstate parse')
    expect(catalog.zipf(1, 100, 1.2) == catalog.zipf(1, 100, 1.2) and 1 <= catalog.zipf(7, 100, 1.2) <= 100, 'deterministic zipf')
    collect.selftest(expect)
    print(f'selftest ok: {len(catalog.CASES)} core cases, ' + ', '.join(f'{len(c)} {n}' for n, (c, _) in suites.SUITES.items()))


def report(a, ctx: Ctx, selected: list, runs: list) -> int:
    failed = 0
    for c in selected:
        m, bad, secs = runs[0][c.id]
        verdict, over = judge(c, m, bad, effective(c, ctx))
        failed += verdict == 'FAIL'
        if a.determinism or a.calibrate:
            verdicts = [judge(c, r[c.id][0], r[c.id][1], effective(c, ctx))[0] for r in runs]
            vary = len(set(verdicts)) > 1
            failed += vary
            print(f'LOAD_SPREAD {c.id} {" ".join(verdicts)} ' + ('VARY' if vary else 'OK'))
            for n, r in enumerate(runs if vary else []):
                print(f'LOAD_RUN{n} ' + line(c, ctx, r[c.id][0], *judge(c, r[c.id][0], r[c.id][1], effective(c, ctx)), r[c.id][2])[5:])
        if a.calibrate and m:
            worst = {k: max(r[c.id][0].get(k) or 0 for r in runs if r[c.id][0]) for k in ('lock_wait_pct', 'lwlock_wait_pct', 'p99_ratio', 'ctx_kb_delta', 'peak')}
            print(f'LOAD_CAL {c.id} ' + ' '.join(f'{k}={v * 1.5:.1f}' for k, v in worst.items() if k in m))
        print(line(c, ctx, m, verdict, over, secs))
    print(f'LOAD_OPEN {sum(bool(c.open) for c in selected)} ' + ','.join(c.open for c in selected if c.open))
    return int(failed > 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--suite', choices=('core', *suites.SUITES, 'all'), default='core')
    ap.add_argument('--only', default='')
    ap.add_argument('--class', dest='cls', default='')
    ap.add_argument('--format', type=int, default=0)
    ap.add_argument('--log', default='')
    ap.add_argument('--scale', type=float, default=float(os.environ.get('LOAD_SCALE', '1')))
    for flag in ('list', 'calibrate', 'determinism', 'selftest', 'no-rss'):
        ap.add_argument('--' + flag, action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest() or 0
    selected = [c for c in suites.cases(a.suite) if re.search(a.only, c.id) and c.cls.startswith(a.cls)]
    if a.list and a.format:
        return print(*(c.id for c in selected if a.format in c.formats), sep='\n') or 0
    if a.format:
        os.environ['LOAD_PGOPTIONS'] = f'-c sift.test_create_format1={"on" if a.format == 1 else "off"}'
    db = Psql()
    db.run('CREATE EXTENSION IF NOT EXISTS pg_sift')
    ctx = Ctx(a, db)
    if a.list:
        return print(*(c.id for c in selected if ctx.fmt in c.formats), sep='\n') or 0
    if ctx.testing:
        db.run('CREATE EXTENSION IF NOT EXISTS pg_sift_test')
    else:
        print('LOAD_INFO release build: the sub-checks of the testing library are skipped')
    ctx.db.run(f'\\set rows {catalog.N}\n\\i {HERE}/fixtures.sql')
    log = collect.LogScan(a.log)
    try:
        runs = [sweep(ctx, selected, log)]
        if a.determinism:
            runs.append(sweep(ctx, selected, log))
        if a.determinism or a.calibrate:
            runs.append(sweep(ctx, selected, log, reverse=True))
        return report(a, ctx, selected, runs)
    finally:
        ctx.close_sessions()
        ctx.db.close()
        ctx.obs_db.close()


if __name__ == '__main__':
    sys.exit(main())
