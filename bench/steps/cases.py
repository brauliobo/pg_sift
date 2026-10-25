#!/usr/bin/env python3
"""The benchmark cases of the optimizations (bench/cases.tsv, bench/cases/*.sql; bench/bench cases, steps/cases.sh).

    python3 -I cases.py run   [--cases ID,..] [--engines LIST] [--rows N] [--rounds R] [--runs N] [--duck-port P] [--out FILE] [--docs]
    python3 -I cases.py list
    python3 -I cases.py plan  [--cases ..] [--rows N] [--scales L]      (what a run needs, no server)

A case is a row of the registry (the feature, the CHANGELOG sentence it measures, the dataset and index, the plan the scan must show, the
baselines, the README block it feeds) and a SQL file in the layout of bench/sql/store/queries (`--@ pg` the statement every lane runs, `--@ sift`
or `--@ duck` replace it for that lane, `--@ setup` DDL after the index, `-- set: a = 1; b = 2` settings of every lane, `--@ prepare` / `--@ load` for a case that times a load). `run` builds
each case's table in a schema of its own on the server named by PGHOST / PGPORT (steps/cases.sh starts it), asserts the plan of the sift lane from
EXPLAIN (a case that falls back to the normal plan fails), then times the lanes interleaved in rounds and compares every answer with the native one:

    sift     the planner's own choice with pg_sift
    native   the same table and statement with sift.enable_agg = off (PostgreSQL's plan on the same rows)
    <name>   a variant of the registry: the sift lane with settings (`scan:sift.agg_preagg=off`)
    pg_duckdb  a pg_duckdb server (--duck-port) holding a plain copy of the rows, duckdb.force_execution = on; NA when DuckDB does not run it

A wrong answer or a lost plan is a failure (exit 1); a case where sift is slower than native is reported as a loss, not hidden.
"""
import argparse
import datetime
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from duckdb import Session, same, wait_quiet, busy  # noqa: E402  (a persistent psql session, the comparison of rows, the quiet gate)
from store_lib import sections  # noqa: E402  (`--@ name` sections)

BENCH = os.path.dirname(HERE)
REPO = os.path.dirname(BENCH)
GEN = os.path.join(BENCH, 'sql', 'store', 'gen.sql')
EVENTS_GEN = os.path.join(REPO, 'test', 'plan', 'gen.sql')   # the events dataset of the plan-choice gate: one schema, c_events, for every case of the dataset
DATASETS = {'events': ('c_events', EVENTS_GEN), 'keys': ('c_keys', os.path.join(BENCH, 'sql', 'keys', 'gen.sql')),
            'tpch': ('c_tpch', os.path.join(BENCH, 'sql', 'tpch', 'gen.sql')),
            'hits': ('c_hits', os.path.join(BENCH, 'sql', 'hits', 'gen.sql')),
            'tseries': ('c_tseries', os.path.join(BENCH, 'sql', 'tseries', 'gen.sql'))}   # the datasets a case shares: schema and generator
BUILT = set()
COLUMNS = ['id', 'customer_id', 'country', 'category', 'status', 'amount', 'quantity', 'created_at', 'title']
TYPES = ['bigint', 'int', 'text', 'text', 'text', 'numeric(10,2)', 'smallint', 'timestamp', 'text']
DERIVED = ['segment_id', 'order_date']   # int with 6,313 values and the date of the order: the keys of the key-presence and calendar cases
INDEX_COLUMNS = ', '.join(COLUMNS[1:8] + DERIVED)
DB = 'cases'
FIELDS = ['id', 'changelog', 'dataset', 'index', 'plan', 'baselines', 'variants', 'metric', 'block', 'headline']
TIMEOUT = 60
LANE_BUDGET = 20.0   # seconds of timed runs after which a lane stops repeating (it has MIN_RUNS or more); --lane-budget
MIN_RUNS = 3
SCAN = re.compile(r'Custom Scan \(Sift')
FORCED = ('agg_forced', 'sift.agg_force = on')   # the lane of a fallback case: what the scan can do where the planner declines it
TIMES = {}   # case id -> [build seconds, run seconds] of the last point


def die(msg):
    sys.exit('cases: ' + msg)


def log(msg):
    print('[%s cases] %s' % (time.strftime('%T'), msg), file=sys.stderr, flush=True)


# --- the registry

class Case:
    def __init__(self, row, text):
        self.__dict__.update(zip(FIELDS, row))
        self.sec = sections(text)
        head = self.sec.pop('').split('\n')
        self.title = head[0][3:]
        self.label = next((m[10:] for m in head if m.startswith('-- label: ')), self.id)
        self.opt = dict(m[3:].split(': ', 1) for m in head if re.match(r'-- (ordered|vacuum|set): ', m))
        self.load = 'load' in self.sec
        self.baselines = [b for b in self.baselines.split(',') if b != '-']
        self.variants = [v.split(':', 1) for v in self.variants.split(',') if v != '-']
        self.fallback = self.plan.startswith('fallback:')   # the planner is expected to keep PostgreSQL's plan: `plan` is that plan, and a lane with the scan forced is measured beside it
        if self.fallback:
            self.plan = self.plan[len('fallback:'):]
            self.variants = [v for v in self.variants if v[0] != FORCED[0]] + [list(FORCED)]
        self.metrics = self.metric.split(',')
        self.headline = self.headline == 'yes'
        self.share = None   # read_registry: the name of the table that cases with the same index and setup read together

    def statement(self, lane):
        if lane == 'pg_duckdb':
            return self.sec.get('duck', self.sec['pg'])
        return self.sec['pg'] if lane == 'native' else self.sec.get('sift', self.sec['pg'])

    def schema(self):
        return DATASETS[self.dataset][0] if self.dataset in DATASETS else 'c_' + (self.share or self.id)

    def table_index(self):
        m = re.match(r'default((?:\+\w+)*)(.*)$', self.index)
        return '(%s)%s' % (', '.join([INDEX_COLUMNS] + [c[1:] for c in re.findall(r'\+\w+', m.group(1))]), m.group(2)) if m else self.index


def read_registry(root=REPO):
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(root, 'bench', 'cases.tsv')) if l.strip() and not l.startswith('#')]
    cases = []
    for r in rows:
        if len(r) != len(FIELDS):
            die('bench/cases.tsv: %s has %d columns, %d expected (%s)' % (r[0], len(r), len(FIELDS), ' '.join(FIELDS)))
        path = os.path.join(root, 'bench', 'cases', r[0] + '.sql')
        if not os.path.exists(path):
            die('bench/cases.tsv: %s has no file bench/cases/%s.sql' % (r[0], r[0]))
        cases.append(Case(r, open(path).read()))
    share_builds(cases)
    return cases


READ_ONLY = re.compile(r'\b(insert|update|delete|alter|create|drop|truncate|copy)\b', re.I)


def share_builds(cases):
    """the cases on `orders` that only read it and have the same index and setup are one build: `orders` is made, indexed and vacuumed once (the build is most of the time of a case at 1M
    rows), in a schema named after the first case of the group; a case that writes, loads or has its own dataset keeps a table of its own"""
    groups = {}
    for c in cases:
        if c.dataset == 'orders' and not c.load and not any(READ_ONLY.search(c.sec.get(k, '')) for k in ('pg', 'sift', 'duck')):
            groups.setdefault((c.index, c.sec.get('setup', ''), c.opt.get('vacuum', 'yes')), []).append(c)
    for g in groups.values():
        if len(g) > 1:
            for c in g:
                c.share = 's_' + g[0].id


# --- the server

ENV = {**os.environ, 'LC_ALL': 'C'}


def psql(sql, db=DB, port=None, *args):
    cmd = ['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres', '-d', db] + (['-h', '127.0.0.1', '-p', str(port)] if port else []) + list(args)
    r = subprocess.run(cmd, input=sql, capture_output=True, text=True, env={**ENV, 'PGOPTIONS': '-c statement_timeout=%ds -c client_min_messages=warning' % TIMEOUT})
    if r.returncode:
        die('psql: %s' % r.stderr.strip()[:500])
    return r.stdout.strip()


def server_load():
    return open('/proc/loadavg').read().split()[0]


def dataset(rows):
    """orders_src (bench/sql/store/gen.sql) with ROWS rows, and its CSV for the server-side COPY of the load cases; unchanged when it is there"""
    if psql("SELECT to_regclass('public.orders_src') IS NULL") == 't' or int(psql('SELECT count(*) FROM public.orders_src')) != rows:
        log('generating %s orders' % format(rows, ','))
        subprocess.run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres', '-d', DB, '-v', 'n=%d' % rows, '-f', GEN], check=True, capture_output=True, env=ENV)
        psql('ANALYZE orders_src')
    csv = os.path.join(os.environ.get('CASES_TMP', '/var/tmp'), 'orders-%d.csv' % rows)
    if not os.path.exists(csv):
        psql("COPY (SELECT %s FROM orders_src ORDER BY id) TO '%s' CSV" % (', '.join(COLUMNS), csv))
    return csv


def build(case, rows):
    """the case's schema: `orders` from orders_src (+ the derived columns) with the btrees of the store's PostgreSQL lane (bench/sql/store/pg.sql, without the GIN) and
    the sift_col index, the case's DDL, VACUUM (FREEZE, ANALYZE) unless the case says no"""
    if case.load:
        psql('DROP SCHEMA IF EXISTS c_%s CASCADE; CREATE SCHEMA c_%s' % (case.id, case.id))
        return
    if case.dataset in DATASETS:
        schema, path = DATASETS[case.dataset]
        if (case.dataset, rows) not in BUILT:
            psql('DROP SCHEMA IF EXISTS %s CASCADE; CREATE SCHEMA %s' % (schema, schema))
            gen = open(path).read()
            if case.dataset == 'events':   # the gate's sizes are 20,000 to 50,000 rows: int8 products keep 200,000 and 1,000,000 in range, and the partitions it indexes are those of its own schema
                gen = gen.replace('(i * 7919)', '(i::bigint * 7919)').replace('(i * 104729)', '(i::bigint * 104729)').replace('JOIN pg_class c ON c.oid = i.inhrelid', "JOIN pg_class c ON c.oid = i.inhrelid AND c.relnamespace = 'c_events'::regnamespace")
            r = subprocess.run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres', '-d', DB, '-v', 'n=%d' % rows, '-f', '-'], input=gen, capture_output=True, text=True,
                               env={**ENV, 'PGOPTIONS': '-c search_path=%s,public -c client_min_messages=warning' % schema})
            if r.returncode:
                die('%s: %s' % (os.path.relpath(path, REPO), r.stderr.strip()[:500]))
            BUILT.add((case.dataset, rows))
        if case.sec.get('setup'):
            psql('SET search_path = %s, public;\n' % schema + case.sec['setup'] + ';\n')
        return
    if case.share and (case.schema(), rows) in BUILT:
        return
    ddl = ['DROP SCHEMA IF EXISTS {0} CASCADE', 'CREATE SCHEMA {0}', 'SET search_path = {0}, public',
           'CREATE TABLE orders AS SELECT o.*, (customer_id % 6313)::int AS segment_id, created_at::date AS order_date FROM public.orders_src o ORDER BY id',
           'ALTER TABLE orders ADD PRIMARY KEY (id)', 'CREATE INDEX orders_created ON orders (created_at)', 'CREATE INDEX orders_country_status ON orders (country, status)',
           'CREATE INDEX orders_col ON orders USING sift_col %s' % case.table_index()]
    sql = ';\n'.join(d.format(case.schema()) for d in ddl) + ';\n' + case.sec.get('setup', '').replace('{cols}', INDEX_COLUMNS) + ';\n'
    if case.opt.get('vacuum', 'yes') == 'yes':
        sql += 'VACUUM (FREEZE, ANALYZE) orders;\n'
    psql(sql)
    psql('CHECKPOINT')
    BUILT.add((case.schema(), rows))


def server_session(timeout):
    return Session(os.environ['PGPORT'], timeout, None, db=DB, host=os.environ['PGHOST'])


def lane_sessions(case, engines, duck_port):
    """lane -> Session: sift and native always, then the variants of the registry and the engines the run asked for (the registry lists the ones a case may use)"""
    # a case that turns the access-method scan on (-- set: sift.enable_col_scan = on) compares it with the plan PostgreSQL makes without it
    lanes = {'sift': ['SET sift.enable_agg = on'], 'native': ['SET sift.enable_agg = off'] + (['SET sift.enable_col_scan = off'] if 'sift.enable_col_scan' in case.opt.get('set', '') else [])}
    for name, settings in case.variants:
        lanes[name] = ['SET %s' % s for s in settings.split(';')]
    out = {}
    wm = ['SET work_mem = %s' % os.environ['CASES_WORK_MEM']] if os.environ.get('CASES_WORK_MEM') else []   # a large work_mem is 4GB; the cluster default is 64MB
    for lane, sets in lanes.items():
        s = server_session(TIMEOUT)
        for st in ['SET search_path = %s, public' % case.schema(), 'SET max_parallel_workers_per_gather = %s' % os.environ.get('CASES_WORKERS', '2')] + wm + ['SET %s' % x for x in case.opt.get('set', '').split(';') if x] + sets:
            s.run(st)
        out[lane] = s
    if 'pg_duckdb' in engines and 'pg_duckdb' in case.baselines and not case.load and duck_port:
        s = Session(duck_port, TIMEOUT, None)
        s.run('SET duckdb.force_execution = on')
        out['pg_duckdb'] = s
    return out


def plan_text(session, sql, analyze=True):
    """EXPLAIN of the statement; ANALYZE (one more run) shows what the scan did: units answered from pre-aggregates, rows scanned"""
    return '\n'.join(session.run('EXPLAIN (%s) %s' % ('ANALYZE, COSTS OFF, TIMING OFF' if analyze else 'COSTS OFF', sql.rstrip(';')))[1])


def load_duck(port, csv):
    """the orders as a plain heap in the pg_duckdb server, with the derived columns and the btrees of the store's PostgreSQL lane"""
    psql('CREATE EXTENSION IF NOT EXISTS pg_duckdb; DROP TABLE IF EXISTS orders, orders_raw; CREATE TABLE orders_raw (%s)' % ', '.join('%s %s' % ct for ct in zip(COLUMNS, TYPES)), 'bench', port)
    with open(csv) as f:
        subprocess.run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-h', '127.0.0.1', '-p', str(port), '-U', 'postgres', '-d', 'bench', '-c', 'COPY orders_raw FROM STDIN CSV'], stdin=f, check=True, env=ENV)
    psql('CREATE TABLE orders AS SELECT *, (customer_id % 6313)::int AS segment_id, created_at::date AS order_date FROM orders_raw ORDER BY id; DROP TABLE orders_raw;'
         ' ALTER TABLE orders ADD PRIMARY KEY (id); CREATE INDEX orders_created ON orders (created_at); CREATE INDEX orders_country_status ON orders (country, status);'
         ' VACUUM (FREEZE, ANALYZE) orders', 'bench', port)


def plan_note(text):
    """the plan the row says it used: the scan node and the lines that name what it does (or the first scan of PostgreSQL's plan)"""
    sift = [re.sub(r' \(actual[^)]*\)', '', l.strip()) for l in text.split('\n') if re.match(r'\s*(Custom Scan \(Sift|Top-N:|Group Keys:|Pushed Aggregates:)', l)]
    if sift:
        return '; '.join(sift)[:140]
    scans = re.findall(r'(?:Parallel )?(?:Seq Scan|Index Only Scan|Index Scan|Bitmap Heap Scan)(?: using \w+)?(?: on \w+)?', text)
    return scans[0] if scans else 'DuckDBScan' if 'DuckDBScan' in text else 'unknown'


def plan_shape(text):
    """the nodes of a plan without their numbers (costs, rows, times, counters): two plans of the same shape are the same plan"""
    return [re.sub(r'\s*[(:].*', '', l.strip()) for l in text.split('\n') if l.strip() and not l.strip().startswith(('Planning', 'Execution', 'Buffers', 'JIT', 'Memory', 'Heap Fetches', 'Rows Removed', 'Settings'))]


def equal(a, b, ordered):
    """the same rows (numbers to a relative 1e-9), in the same order when the statement orders them"""
    return len(a) == len(b) and (all(same([x], [y]) for x, y in zip(a, b)) if ordered else same(a, b))


IDS = {}


def expand(sql, case):
    """`{ids:TABLE:N}` is the list of the ids of the rows of TABLE with n = N, in the statement: a literal IN list of 50,000 or 500,000 values is not a thing to write in a file"""
    def ids(m):
        key = (case.schema(), m.group(1), m.group(2))
        if key not in IDS:
            IDS[key] = psql("SELECT string_agg(id::text, ',' ORDER BY id) FROM %s.%s WHERE n = %s" % key)
        return IDS[key]
    return re.sub(r'\{ids:(\w+):(\d+)\}', ids, sql)


def measure_query(case, lanes, rounds, runs, keep_plan=True):
    """rows of results: per lane the timed runs of the interleaved rounds, the answer of the warm-up compared with the native one; a plan other than the case's is an error
    unless keep_plan is false (the scale points: the planner's choice at each size is what the curve shows)"""
    ordered = case.opt.get('ordered', 'no') == 'yes'
    sql = {lane: expand(case.statement(lane), case) for lane in lanes}
    notes, answers, times, errors, texts = {}, {}, {lane: [] for lane in lanes}, {}, {}
    for lane, s in lanes.items():
        text = texts[lane] = plan_text(s, sql[lane], lane != 'pg_duckdb')
        notes[lane] = plan_note(text)
        if lane == 'pg_duckdb' and 'DuckDBScan' not in text:
            errors[lane] = 'not supported: it runs on the PostgreSQL executor'
        ms, answers[lane], err, _ = s.run(sql[lane])   # the warm-up, whose answer is checked
        if err:
            errors.setdefault(lane, err[:110])
        if lane == 'sift':
            # the plan of the runs that are timed is the one after the first: in a backend that has not loaded the library (it needs no shared_preload_libraries) the
            # first statement is planned without it, and the note says so
            again = plan_text(s, sql[lane])
            if plan_shape(again) != plan_shape(text):
                notes[lane] = '%s (the first plan of a backend was %s)' % (plan_note(again), notes[lane])
                text = texts[lane] = again
            if keep_plan and not re.search(case.plan, text, re.S):
                errors[lane] = 'PLAN LOST: expected /%s/, the plan is %s' % (case.plan, notes[lane])
            elif keep_plan and case.fallback and SCAN.search(text):
                errors[lane] = 'PLAN CHANGED: the planner chooses the scan now (%s): drop the fallback: of the plan column' % notes[lane]
            if case.fallback:
                notes['why'] = why(s, sql[lane])
    if FORCED[0] in texts and not SCAN.search(texts[FORCED[0]]) and FORCED[0] not in errors:
        notes[FORCED[0]] = 'no scan: ' + why(lanes[FORCED[0]], sql[FORCED[0]])   # the shape the scan cannot run: a feature gap, not a cost
    if 'native' in texts and 'sift' in texts and notes['sift'] == notes['native'] and plan_shape(texts['sift']) != plan_shape(texts['native']):
        notes['sift'] += ' (a different plan below it)'   # the first scan is the same: a block must not call two different plans "the same plan"
    ref = answers.get('native')
    for lane in lanes:
        if lane not in errors and lane != 'native' and ref is not None and not equal(ref, answers[lane], ordered):
            errors[lane] = 'RESULT DIFFERS from native (%d rows against %d)' % (len(answers[lane]), len(ref))
    for r in range(rounds):
        order = [l for l in lanes if l not in errors]
        order = order[r % len(order):] + order[:r % len(order)] if order else order
        for lane in order:
            for _ in range(runs):
                if sum(times[lane]) / 1000 > LANE_BUDGET and len(times[lane]) >= MIN_RUNS:   # a lane of seconds a run: the median of a few runs, not of fifteen
                    break
                times[lane].append(lanes[lane].run(sql[lane])[0])
    return times, notes, errors


def why(session, sql):
    """sift.explain_why of the statement (planned, not run): the first reason each arm of the aggregate pushdown gave up"""
    out = session.run("SELECT arm || ' ' || slug || ': ' || reason FROM sift.explain_why($q$%s$q$) WHERE NOT used" % sql.rstrip(';'))[1]
    return '; '.join(out)[:300] or 'sift.explain_why names no reason'


def stats(ms):
    return dict(value='%.3f' % statistics.median(ms), min='%.3f' % min(ms), max='%.3f' % max(ms), n=len(ms))


def wal_lsn(s):
    return int(s.run("SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')::bigint")[1][0])


def cpu_ns(pid):
    with open('/proc/%s/schedstat' % pid) as f:
        return int(f.read().split()[0])


def measure_load(case, csv, rounds, rows):
    """a load case: per lane and round one fresh table (`--@ prepare`, untimed, with {am} and {csv}), the statements of `--@ load` timed with its WAL and the backend's CPU; metrics wal_bytes (per row), ms, cpu_ms"""
    lanes = {'sift': ('sift_table', []), 'native': ('heap', [])}
    for name, settings in case.variants:
        lanes[name] = ('sift_table', ['SET %s' % s for s in settings.split(';')])
    out, errors, plans = {lane: {'wal_bytes': [], 'ms': [], 'cpu_ms': []} for lane in lanes}, {}, {}
    for r in range(rounds):
        names = list(lanes)
        for lane in names[r % len(names):] + names[:r % len(names)]:
            am, sets = lanes[lane]
            s = server_session(600)
            for st in ['SET search_path = c_%s, public' % case.id] + sets:
                s.run(st)
            for stmt in re.split(r';[ \t]*\n', case.sec['prepare'].replace('{am}', am).replace('{csv}', csv)):   # the statements of the preparation, untimed (a VACUUM among them cannot share a transaction)
                err = s.run(stmt)[2]
                if err:
                    die('%s: the preparation failed on %s: %s' % (case.id, lane, err))
            pid = s.run('SELECT pg_backend_pid()')[1][0]
            s.run('CHECKPOINT')
            lsn, cpu = wal_lsn(s), cpu_ns(pid)
            ms = 0
            for stmt in re.split(r';[ \t]*\n', case.sec.get('load_sift', case.sec['load']) if am == 'sift_table' else case.sec['load']):   # the statements of the load, their times added
                took, _, err, _ = s.run(stmt.replace('{csv}', csv))
                if err:
                    die('%s: the load failed on %s: %s' % (case.id, lane, err))
                ms += took
            cpu, lsn = cpu_ns(pid) - cpu, wal_lsn(s) - lsn
            checked = '\n'.join(s.run(case.sec['check'])[1]) if lane == 'sift' else ''
            plans[lane] = checked.replace('\n', ', ')
            if lane == 'sift' and not re.search(case.plan, checked, re.M):
                errors[lane] = 'PLAN LOST: the load did not write what /%s/ describes' % case.plan
            out[lane]['wal_bytes'].append(lsn / rows)
            out[lane]['ms'].append(ms)
            out[lane]['cpu_ms'].append(cpu / 1e6)
            s.close()
            build(case, rows)   # an empty schema again
    return out, plans, errors


def host():
    cpu = next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name')), '?')
    mem = int(next(l.split()[1] for l in open('/proc/meminfo') if l.startswith('MemTotal'))) // 1048576
    return '%s, %d threads, %d GB memory' % (cpu, os.cpu_count(), mem)


def summarize(results, cases):
    """the table printed after a run and the loss lines: where sift is slower than native, plainly"""
    lines, losses = ['%-26s %-10s %10s %10s %8s  %s' % ('case', 'metric', 'native', 'sift', 'speedup', 'plan')], []
    for c, m in [(c, m) for c in cases for m in c.metrics]:
        cell = lambda arm: next((r for r in results if r['case'] == c.id and r['arm'] == arm and r['metric'] == m), None)  # noqa: E731
        n, s = cell('native'), cell('sift')
        if not (n and s and 'NA' not in (n['value'], s['value']) and 'error' not in (n['value'], s['value'])):
            lines.append('%-26s %-10s %10s %10s %8s  %s' % (c.id, m, n['value'] if n else '-', s['value'] if s else '-', '-', (s or {}).get('note', '')[:60]))
            continue
        if c.fallback:   # PostgreSQL's plan against itself has no ratio: the lane with the scan forced is the comparison
            f = cell(FORCED[0])
            ok = f and f['value'] not in ('NA', 'error', '')
            lines.append('%-26s %-10s %10s %10s %8s  %s' % (c.id, m, n['value'], f['value'] if ok else '-', '%.1fx' % (float(n['value']) / float(f['value'])) if ok and float(f['value']) else '-', 'scan forced: ' + f['note'][:46] if f else ''))
            continue
        speed = float(n['value']) / float(s['value']) if float(s['value']) else float('inf')
        lines.append('%-26s %-10s %10s %10s %7.1fx  %s' % (c.id, m, n['value'], s['value'], speed, s['note'][:60]))
        if speed < 0.95:
            losses.append('%s: sift %s %s against native %s (%.1fx slower)' % (c.id, s['value'], m, n['value'], 1 / speed))
    if losses:
        lines += ['', 'slower than native on %d of %d numbers:' % (len(losses), sum(len(c.metrics) for c in cases))] + ['  ' + l for l in losses]
    return '\n'.join(lines)


def run_point(args, cases, rows, engines, results, failed):
    """every case at ROWS orders: the rows of `results` (a case id with its size when the run has scales), the failures in `failed`; the scale points keep the planner's plan"""
    scaled = bool(args.scales)
    tag = '@%d' % rows if scaled else ''
    t0 = time.time()
    csv = dataset(rows)
    TIMES['(dataset)' + tag] = [round(time.time() - t0, 1), 0]
    if args.duck_port:
        t0 = time.time()
        load_duck(args.duck_port, csv)
        TIMES['(pg_duckdb load)' + tag] = [round(time.time() - t0, 1), 0]
    point = []
    left = {}   # the cases still to read each shared table: it is dropped after the last
    for c in cases:
        if c.share:
            left[c.schema()] = left.get(c.schema(), 0) + 1
    for c in cases:
        log('%s%s: %s' % (c.id, ' at %s rows' % format(rows, ',') if scaled else '', c.label))
        t0 = time.time()
        build(c, rows)
        built = time.time() - t0
        if not args.no_wait:
            wait_quiet(args.wait)
        noisy = busy()
        load = server_load()
        if c.load:
            t1 = time.time()
            out, plans, errors = measure_load(c, csv, args.rounds, rows)
            TIMES[c.id + tag] = [round(built, 1), round(time.time() - t1, 1)]
            failed += ['%s%s on %s: %s' % (c.id, tag, lane, e) for lane, e in errors.items()]
            for lane, ms in out.items():
                for metric, xs in ms.items():
                    point.append(dict(case=c.id, arm=lane, metric=metric, note=(('sift_table, segments written: %s' if 'USING {am}' in c.sec['prepare'] else 'heap with a sift_col index, rows: %s') % plans[lane] if lane == 'sift' else 'heap' if lane == 'native' else 'sift_table, %s' % lane), load=load, **stats(xs)))
            continue
        lanes = lane_sessions(c, engines, args.duck_port)
        t1 = time.time()
        times, notes, errors = measure_query(c, lanes, args.rounds, args.runs, keep_plan=not (scaled or args.any_plan))
        TIMES[c.id + tag] = [round(built, 1), round(time.time() - t1, 1)]
        for lane in lanes:
            if lane in errors:
                if lane in ('sift', 'native') or errors[lane].startswith(('PLAN', 'RESULT')):
                    failed.append('%s%s on %s: %s' % (c.id, tag, lane, errors[lane]))
                point.append(dict(case=c.id, arm=lane, metric='ms', value='NA' if errors[lane].startswith('not supported') else 'error', note=errors[lane], load=load))
            else:
                point.append(dict(case=c.id, arm=lane, metric='ms', note=notes[lane] + (' NOISY' if noisy else ''), load=load, **stats(times[lane])))
            lanes[lane].close()
        if 'why' in notes:
            point.append(dict(case=c.id, arm='why', metric='ms', value='', note=notes['why'], load=load))
        if c.share:
            left[c.schema()] -= 1
        if c.dataset not in DATASETS and (c.share is None and scaled or c.share and left[c.schema()] == 0):   # a copy of the table for each of the 30 cases at 20 million rows is not room the host has
            psql('DROP SCHEMA IF EXISTS %s CASCADE' % c.schema())
            BUILT.discard((c.schema(), rows))
    print(('rows %s\n' % format(rows, ',') if scaled else '') + summarize(point, cases))
    results += [dict(r, case=r['case'] + tag) for r in point]


def cmd_run(args):
    global TIMEOUT, LANE_BUDGET
    LANE_BUDGET = args.lane_budget
    cases = [c for c in read_registry() if not args.cases or c.id in args.cases]
    if not cases:
        die('no case matches %s' % ','.join(args.cases))
    if args.no_share:
        for c in cases:
            c.share = None
    TIMEOUT = args.timeout
    engines = args.engines.split(',')
    load_before = host_load()
    results, failed = [], []
    for scale in [int(x) for x in args.scales.split(',')] if args.scales else [1]:
        run_point(args, cases, args.rows * scale, engines, results, failed)
    log('time: %.0f s of builds (the dataset and the copy in pg_duckdb included), %.0f s of timed runs, %d cases, %d tables built for them' % (
        sum(b for b, _ in TIMES.values()), sum(r for _, r in TIMES.values()), len(cases), len({c.schema() for c in cases if c.dataset not in DATASETS})))
    for f in failed:
        print('FAILED ' + f, file=sys.stderr)
    write_results(args, cases, results, load_before)
    if args.docs and not failed:   # the blocks that quote bench/results/cases.tsv and cases-scale.tsv
        import cases_render
        cases_render.render(os.path.join(REPO, 'bench', 'results'), os.path.join(REPO, 'README.md'), os.path.join(REPO, 'docs', 'BENCHMARKS.md'))
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'highlights.py')], check=True, stdout=subprocess.DEVNULL)
    return 1 if failed else 0


def cmd_plan(args):
    """what a run would do, no server: the points, the builds, the disk and the time it needs (estimates fitted to the 1,000,000-row run of 2026-10-24, 86 cases in 17 min with shared tables: linear in the rows, a floor on a loaded host)"""
    cases = [c for c in read_registry() if not args.cases or c.id in args.cases]
    scales = [int(x) for x in args.scales.split(',')] if args.scales else [1]
    events = sum(c.dataset in DATASETS for c in cases)
    tmp = os.environ.get('CASES_TMP', '/dev/shm/sift-cases')
    probe = tmp
    while not os.path.exists(probe):
        probe = os.path.dirname(probe)
    free = os.statvfs(probe).f_bavail * os.statvfs(probe).f_frsize / 2**30
    seconds, peak = 0.0, 0.0
    print('cases plan: %d cases (%d on orders, %d on shared datasets) at %d point%s of %s orders, %d rounds of %d timed runs, statement timeout %d s' % (
        len(cases), len(cases) - events, events, len(scales), 's' * (len(scales) > 1), ' x '.join([format(args.rows, ','), str(scales)]), args.rounds, args.runs, args.timeout))
    for sc in scales:
        n = args.rows * sc
        t = n * 25e-6 + (n * 50e-6 if events else 0) + len(cases) * (n * 8e-6 + 4.0 * sc)
        gb = (n * (120 + 85 + 350) + (n * 1000 if events else 0)) / 2**30 + 4
        seconds, peak = seconds + t, max(peak, gb)
        print('  %14s orders (%dx): generate%s, %d builds and measures, about %4.0f min, up to %4.0f GB of scratch' % (format(n, ','), sc, ' orders and events' if events else ' orders', len(cases), t / 60, gb))
    print('  total about %.1f h; the largest point needs about %.0f GB under CASES_TMP=%s (%.0f GB free there); memory cap CASES_MEM=%s' % (seconds / 3600, peak, tmp, free, os.environ.get('CASES_MEM', '8G')))
    print('  the schema of each orders case is dropped after its measure; the events schema (one for the dataset) stays until the next size')
    return 0 if free > peak else 1


def host_load():
    pressure = ''
    try:
        pressure = ' cpu_pressure %s%%' % re.search(r'some avg10=([0-9.]+)', open('/proc/pressure/cpu').read()).group(1)
    except (OSError, AttributeError):
        pass
    return 'load %s%s' % (' '.join(open('/proc/loadavg').read().split()[:3]), pressure)


def write_results(args, cases, results, load_before):
    import extras
    version = re.search(r"default_version\s*=\s*'([^']*)'", open(os.path.join(REPO, 'pg_sift.control')).read()).group(1)
    sha = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip()
    meta = {'orders': args.rows, 'postgresql': psql('SHOW server_version', 'postgres'), 'pg_sift': version, 'build': sha, 'date': datetime.date.today().isoformat(),
            'cpu': host(), 'method': 'client-side milliseconds of one psql session per lane, interleaved: %d rounds of 1 untimed and %d timed runs, median of all; the sift lane is the '
            'planner\'s own choice, native is sift.enable_agg = off on the same table; %s parallel workers; every answer compared with native' % (args.rounds, args.runs, os.environ.get('CASES_WORKERS', '2')),
            'host_load_before': load_before, 'host_load_after': host_load(), 'work_mem': os.environ.get('CASES_WORK_MEM', '64MB')}
    for r in results:
        r['version'] = version
    path = args.out
    if args.docs:
        path = os.path.join(REPO, 'bench', 'results', 'cases-scale.tsv' if args.scales else 'cases.tsv')
        if os.path.exists(path):   # a run of some cases replaces only their rows, at the size of the others
            old_meta, old = extras.read(path)
            ran = {r['case'] for r in results}
            if not args.scales and str(args.rows) != old_meta.get('orders') and {c.id for c in cases} != {c.id for c in read_registry()}:
                die('bench/results/cases.tsv holds %s orders: run every case at --rows %d, or these at --rows %s' % (old_meta.get('orders'), args.rows, old_meta.get('orders')))
            if args.scales and str(args.rows) != old_meta.get('orders'):
                die('bench/results/cases-scale.tsv has its 1x at %s orders: run with --rows %s' % (old_meta.get('orders'), old_meta.get('orders')))
            results = [r for r in old if r['case'] not in ran and str(args.rows) == old_meta.get('orders')] + results   # a run of every case at another size replaces the file
    if args.scales:
        meta['scales'] = args.scales
    extras.write(path, 'Benchmark cases (bench/cases.tsv)%s: pg_sift against PostgreSQL and the other engines on the orders of the orders benchmark.' % (' at 1x, 10x and 100x of --rows (the case id carries the number of orders: ID@ROWS)' if args.scales else ''), meta, results)
    log('wrote %s' % path)
    if args.docs or args.times:
        write_times(args, cases)


def write_times(args, cases):
    """bench/results/cases-time.tsv: the seconds each case took (its build: the table, indexes and setup, zero for a case that reads a table another one built; its timed runs on every lane),
    which the budget table of bench/README.md is made from; the rows of the cases that ran replace theirs"""
    path = args.times or os.path.join(REPO, 'bench', 'results', 'cases-time-scale.tsv' if args.scales else 'cases-time.tsv')
    old = {}
    if os.path.exists(path):
        for l in open(path):
            f = l.rstrip('\n').split('\t')
            if not l.startswith('#') and f[0] != 'case' and len(f) == 4:
                old[f[0]] = f
    for k, (b, r) in TIMES.items():
        old[k] = [k, str(args.rows), str(b), str(r)]
    lines = ['# Seconds of each case of bench/cases.tsv (steps/cases.py, bench/bench cases --docs): build (the table, its indexes and setup; 0 for a case that reads the table of an earlier one), run (the timed runs of every lane,',
             '# %d rounds of %d runs, a lane stops repeating after %g s), at the orders of the row. (dataset): generating the orders; (pg_duckdb load): the copy in the container.' % (args.rounds, args.runs, args.lane_budget),
             '# measured %s on %s, host load before %s.' % (datetime.date.today().isoformat(), host(), host_load()),
             'case\torders\tbuild_s\trun_s'] + ['\t'.join(v) for v in old.values()]
    open(path, 'w').write('\n'.join(lines) + '\n')
    log('wrote %s' % path)


def cmd_list(_):
    for c in read_registry():
        print('%-26s %-9s %-30s %s' % (c.id, c.dataset, ','.join(['sift', 'native'] + c.baselines[1:] if c.baselines[:1] == ['native'] else c.baselines), c.label))


# --- the coverage check: no measurement

CLAIM = re.compile(r'\d[\d,.]* ?(?:ms|us|µs|microseconds?|s)? (?:against|->|to|before|was|vs\.?) \d|\d[\d.]*x (?:faster|less|more|smaller|slower)|instructions|mispredict')


def changelog_lines(root):
    """the bullets of `## Unreleased`"""
    out, on = [], False
    for line in open(os.path.join(root, 'CHANGELOG.md')):
        if line.startswith('## '):
            on = line.startswith('## Unreleased')
        elif on and line.startswith('- '):
            out.append(line.rstrip('\n'))
    return out


def claims(line):
    """a bullet that gives a before and after, a ratio or a count of instructions: a measured speed"""
    kind = line[2:].split(':', 1)[0]
    return kind in ('Added', 'Changed', 'Fixed') and bool(CLAIM.search(line))


def allow_list(root):
    path = os.path.join(root, 'scripts', 'bench-cases.allow')
    out = {}
    if os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.startswith('#'):
                anchor, _, reason = line.rstrip('\n').partition('\t')
                out[anchor] = reason.strip()
    return out


def cmd_check(args):
    root = os.path.abspath(args.root)
    errors, notes = [], []
    cases = read_registry(root)
    ids = [c.id for c in cases]
    changelog = open(os.path.join(root, 'CHANGELOG.md')).read()
    docs = ''.join(open(os.path.join(root, f)).read() for f in ('README.md', os.path.join('docs', 'BENCHMARKS.md')) if os.path.exists(os.path.join(root, f)))
    results = os.path.join(root, 'bench', 'results', 'cases.tsv')
    rows = [l.split('\t') for l in open(results) if l.strip() and not l.startswith('#')][1:] if os.path.exists(results) else []
    measured = {(r[1], r[2], r[3]) for r in rows if len(r) > 4 and r[4] not in ('NA', 'error', '')}
    for f in sorted(os.listdir(os.path.join(root, 'bench', 'cases'))):
        if f.endswith('.sql') and f[:-4] not in ids:
            errors.append('bench/cases/%s is in no row of bench/cases.tsv' % f)
    for c in cases:
        if ids.count(c.id) > 1:
            errors.append('%s: the id is in bench/cases.tsv twice' % c.id)
        if c.plan in ('', '.') or (c.fallback and c.load):
            errors.append('%s: the plan column must name the plan (a case that compares pg_sift has its scan in it; fallback:REGEX names the plan PostgreSQL keeps), not %r' % (c.id, c.plan))
        try:
            re.compile(c.plan)
        except re.error as e:
            errors.append('%s: the plan /%s/ is not a regular expression (%s)' % (c.id, c.plan, e))
        if c.changelog not in changelog:
            errors.append('%s: its CHANGELOG anchor "%s" is in no line of CHANGELOG.md (the sentence moved: update the registry)' % (c.id, c.changelog))
        if '<!-- bench:%s:begin' % c.block not in docs:
            errors.append('%s: it feeds the block "%s", which README.md and docs/BENCHMARKS.md do not have (<!-- bench:%s:begin ... -->)' % (c.id, c.block, c.block))
        for arm, m in [(a, m) for a in ('native', 'sift') for m in c.metrics]:
            if (c.id, arm, m) not in measured:
                errors.append('%s: bench/results/cases.tsv has no %s row of %s (bench/bench cases --cases %s --docs)' % (c.id, arm, m, c.id))
    allowed = allow_list(root)
    lines = changelog_lines(root)
    changed = {l.rstrip('\n') for l in open(args.changed)} if args.changed else None
    unmeasured = 0
    used = set()
    for line in lines:
        if not claims(line):
            continue
        covered = next((c.id for c in cases if c.changelog in line), None)
        waived = next((a for a in allowed if a in line), None)
        used.add(waived)
        if covered or waived:
            continue
        unmeasured += 1
        if changed is None or line in changed:
            errors.append('CHANGELOG claims a speed with no case: "%s ..." (add bench/cases/ID.sql and a row in bench/cases.tsv whose changelog column is a phrase of the line, or a reasoned line in scripts/bench-cases.allow)' % line[:110])
    for a in allowed:
        if a not in used:
            errors.append('scripts/bench-cases.allow: "%s" matches no speed claim of the Unreleased section (remove it)' % a)
        elif not allowed[a]:
            errors.append('scripts/bench-cases.allow: "%s" has no reason' % a)
    total = sum(1 for l in lines if claims(l))
    if changed is not None and unmeasured:
        notes.append('%d of the %d speed claims of Unreleased have no case yet (only the lines this change adds fail; --strict fails on all)' % (unmeasured, total))
    for e in errors:
        print('bench-cases: ' + e)
    for n in notes:
        print('bench-cases: ' + n)
    if errors:
        return 1
    print('bench-cases: %d cases, each with an anchor in CHANGELOG.md, a block and result rows; %d of %d speed claims of Unreleased covered' % (len(cases), total - unmeasured, total))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['run', 'list', 'check', 'plan'])
    ap.add_argument('--cases', type=lambda s: s.split(','), default=[])
    ap.add_argument('--engines', default='sift,native')
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--duck-port', type=int, default=0)
    ap.add_argument('--out', default=os.path.join(BENCH, 'out', 'cases-%s.tsv' % time.strftime('%Y%m%dT%H%M%S')))
    ap.add_argument('--scales', help='run every case at ROWS times each of these (1,10,100): the curve of bench/results/cases-scale.tsv, the planner\'s plan at each size')
    ap.add_argument('--timeout', type=int, default=TIMEOUT, help='seconds a statement may run (the large points need more)')
    ap.add_argument('--work-mem', help='work_mem of every lane (the cluster runs 64MB; a large setting is 4GB): written to the file as the work_mem meta')
    ap.add_argument('--lane-budget', type=float, default=LANE_BUDGET, help='seconds of timed runs after which a lane stops repeating, with at least %d runs (0: always the full rounds x runs); a case whose statement takes seconds is not run fifteen times' % MIN_RUNS)
    ap.add_argument('--any-plan', action='store_true', help='the planner\'s plan at this size is what the rows show, not a failure (a smoke run: at a few thousand rows it keeps PostgreSQL\'s plan)')
    ap.add_argument('--no-share', action='store_true', help='one table for every case, as before (the build of a case does not depend on the others that ran)')
    ap.add_argument('--times', help='write the seconds of every case to this file (bench/results/cases-time.tsv with --docs), without the results')
    ap.add_argument('--docs', action='store_true')
    ap.add_argument('--no-wait', action='store_true')
    ap.add_argument('--wait', type=int, default=300)
    ap.add_argument('--root', default=REPO)
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--changed')
    args = ap.parse_args()
    if args.work_mem:
        os.environ['CASES_WORK_MEM'] = args.work_mem
    if args.strict:
        args.changed = None
    sys.exit({'run': cmd_run, 'list': cmd_list, 'check': cmd_check, 'plan': cmd_plan}[args.cmd](args))


if __name__ == '__main__':
    main()
