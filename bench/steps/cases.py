#!/usr/bin/env python3
"""The benchmark cases of the optimizations (bench/cases.tsv, bench/cases/*.sql; bench/bench cases, steps/cases.sh).

    python3 -I cases.py run   [--cases ID,..] [--engines LIST] [--rows N] [--rounds R] [--runs N] [--duck-port P] [--out FILE] [--docs]
    python3 -I cases.py list
    python3 -I cases.py check [--root DIR] [--strict] [--changed FILE]      (scripts/check-bench-cases.sh; no measurement, no server)

A case is a row of the registry (the feature, the CHANGELOG sentence it measures, the dataset and index, the plan the scan must show, the
baselines, the README block it feeds) and a SQL file in the layout of bench/sql/store/queries (`--@ pg` the statement every lane runs, `--@ sift`
or `--@ duck` replace it for that lane, `--@ setup` DDL after the index, `--@ prepare` / `--@ load` for a case that times a load). `run` builds
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
COLUMNS = ['id', 'customer_id', 'country', 'category', 'status', 'amount', 'quantity', 'created_at', 'title']
TYPES = ['bigint', 'int', 'text', 'text', 'text', 'numeric(10,2)', 'smallint', 'timestamp', 'text']
DERIVED = ['segment_id', 'order_date']   # int with 6,313 values and the date of the order: the keys of the key-presence and calendar cases
INDEX_COLUMNS = ', '.join(COLUMNS[1:8] + DERIVED)
DB = 'cases'
FIELDS = ['id', 'changelog', 'dataset', 'index', 'plan', 'baselines', 'variants', 'metric', 'block', 'headline']
TIMEOUT = 60


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
        self.label = next((m[10:] for m in head if m.startswith('-- label: ')), self.id)
        self.opt = dict(m[3:].split(': ', 1) for m in head if re.match(r'-- (ordered|vacuum): ', m))
        self.load = 'load' in self.sec
        self.baselines = [b for b in self.baselines.split(',') if b != '-']
        self.variants = [v.split(':', 1) for v in self.variants.split(',') if v != '-']
        self.metrics = self.metric.split(',')
        self.headline = self.headline == 'yes'

    def statement(self, lane):
        if lane == 'pg_duckdb':
            return self.sec.get('duck', self.sec['pg'])
        return self.sec['pg'] if lane == 'native' else self.sec.get('sift', self.sec['pg'])

    def table_index(self):
        cols = '(%s)' % INDEX_COLUMNS
        return cols + self.index[len('default'):] if self.index.startswith('default') else self.index


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
    return cases


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
    ddl = ['DROP SCHEMA IF EXISTS c_{0} CASCADE', 'CREATE SCHEMA c_{0}', 'SET search_path = c_{0}, public',
           'CREATE TABLE orders AS SELECT o.*, (customer_id % 6313)::int AS segment_id, created_at::date AS order_date FROM public.orders_src o ORDER BY id',
           'ALTER TABLE orders ADD PRIMARY KEY (id)', 'CREATE INDEX orders_created ON orders (created_at)', 'CREATE INDEX orders_country_status ON orders (country, status)',
           'CREATE INDEX orders_col ON orders USING sift_col %s' % case.table_index()]
    sql = ';\n'.join(d.format(case.id) for d in ddl) + ';\n' + case.sec.get('setup', '') + ';\n'
    if case.opt.get('vacuum', 'yes') == 'yes':
        sql += 'VACUUM (FREEZE, ANALYZE) orders;\n'
    psql(sql)
    psql('CHECKPOINT')


def server_session(timeout):
    return Session(os.environ['PGPORT'], timeout, None, db=DB, host=os.environ['PGHOST'])


def lane_sessions(case, engines, duck_port):
    """lane -> Session: sift and native always, then the variants of the registry and the engines the run asked for (the registry lists the ones a case may use)"""
    lanes = {'sift': ['SET sift.enable_agg = on'], 'native': ['SET sift.enable_agg = off']}
    for name, settings in case.variants:
        lanes[name] = ['SET %s' % s for s in settings.split(';')]
    out = {}
    for lane, sets in lanes.items():
        s = server_session(TIMEOUT)
        for st in ['SET search_path = c_%s, public' % case.id, 'SET max_parallel_workers_per_gather = %s' % os.environ.get('CASES_WORKERS', '2')] + sets:
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


def equal(a, b, ordered):
    """the same rows (numbers to a relative 1e-9), in the same order when the statement orders them"""
    return len(a) == len(b) and (all(same([x], [y]) for x, y in zip(a, b)) if ordered else same(a, b))


def measure_query(case, lanes, rounds, runs):
    """rows of results: per lane the timed runs of the interleaved rounds, the answer of the warm-up compared with the native one"""
    ordered = case.opt.get('ordered', 'no') == 'yes'
    sql = {lane: case.statement(lane) for lane in lanes}
    notes, answers, times, errors = {}, {}, {lane: [] for lane in lanes}, {}
    for lane, s in lanes.items():
        text = plan_text(s, sql[lane], lane != 'pg_duckdb')
        notes[lane] = plan_note(text)
        if lane == 'sift' and not re.search(case.plan, text, re.S):
            errors[lane] = 'PLAN LOST: expected /%s/, the plan is %s' % (case.plan, notes[lane])
        if lane == 'pg_duckdb' and 'DuckDBScan' not in text:
            errors[lane] = 'not supported: it runs on the PostgreSQL executor'
        ms, answers[lane], err, _ = s.run(sql[lane])   # the warm-up, whose answer is checked
        if err:
            errors.setdefault(lane, err[:110])
    ref = answers.get('native')
    for lane in lanes:
        if lane not in errors and lane != 'native' and ref is not None and not equal(ref, answers[lane], ordered):
            errors[lane] = 'RESULT DIFFERS from native (%d rows against %d)' % (len(answers[lane]), len(ref))
    for r in range(rounds):
        order = [l for l in lanes if l not in errors]
        order = order[r % len(order):] + order[:r % len(order)] if order else order
        for lane in order:
            for _ in range(runs):
                times[lane].append(lanes[lane].run(sql[lane])[0])
    return times, notes, errors


def stats(ms):
    return dict(value='%.3f' % statistics.median(ms), min='%.3f' % min(ms), max='%.3f' % max(ms), n=len(ms))


def wal_lsn(s):
    return int(s.run("SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')::bigint")[1][0])


def cpu_ns(pid):
    with open('/proc/%s/schedstat' % pid) as f:
        return int(f.read().split()[0])


def measure_load(case, csv, rounds, rows):
    """a load case: per lane and round one fresh table, the statement of `--@ load` timed with its WAL and the backend's CPU; metrics wal_bytes (per row), ms, cpu_ms"""
    lanes = {'sift': ('sift_table', []), 'native': ('heap', [])}
    for name, settings in case.variants:
        lanes[name] = ('sift_table', ['SET %s' % s for s in settings.split(';')])
    out, errors = {lane: {'wal_bytes': [], 'ms': [], 'cpu_ms': []} for lane in lanes}, {}
    for r in range(rounds):
        names = list(lanes)
        for lane in names[r % len(names):] + names[:r % len(names)]:
            am, sets = lanes[lane]
            s = server_session(600)
            for st in ['SET search_path = c_%s, public' % case.id] + sets:
                s.run(st)
            prep = case.sec['prepare'].replace('{am}', am)
            s.run(prep)
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
            if lane == 'sift' and not re.search(case.plan, '\n'.join(s.run(case.sec['check'])[1]), re.M):
                errors[lane] = 'PLAN LOST: the load did not write what /%s/ describes' % case.plan
            out[lane]['wal_bytes'].append(lsn / rows)
            out[lane]['ms'].append(ms)
            out[lane]['cpu_ms'].append(cpu / 1e6)
            s.close()
            build(case, rows)   # an empty schema again
    return out, errors


def host():
    cpu = next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name')), '?')
    mem = int(next(l.split()[1] for l in open('/proc/meminfo') if l.startswith('MemTotal'))) // 1048576
    return '%s, %d threads, %d GB RAM' % (cpu, os.cpu_count(), mem)


def summarize(results, cases):
    """the table printed after a run and the loss lines: where sift is slower than native, plainly"""
    lines, losses = ['%-26s %-10s %10s %10s %8s  %s' % ('case', 'metric', 'native', 'sift', 'speedup', 'plan')], []
    for c, m in [(c, m) for c in cases for m in c.metrics]:
        cell = lambda arm: next((r for r in results if r['case'] == c.id and r['arm'] == arm and r['metric'] == m), None)  # noqa: E731
        n, s = cell('native'), cell('sift')
        if not (n and s and 'NA' not in (n['value'], s['value']) and 'error' not in (n['value'], s['value'])):
            lines.append('%-26s %-10s %10s %10s %8s  %s' % (c.id, m, n['value'] if n else '-', s['value'] if s else '-', '-', (s or {}).get('note', '')[:60]))
            continue
        speed = float(n['value']) / float(s['value']) if float(s['value']) else float('inf')
        lines.append('%-26s %-10s %10s %10s %7.1fx  %s' % (c.id, m, n['value'], s['value'], speed, s['note'][:60]))
        if speed < 1:
            losses.append('%s: sift %s %s against native %s (%.1fx slower)' % (c.id, s['value'], m, n['value'], 1 / speed))
    if losses:
        lines += ['', 'slower than native on %d of %d numbers:' % (len(losses), sum(len(c.metrics) for c in cases))] + ['  ' + l for l in losses]
    return '\n'.join(lines)


def cmd_run(args):
    cases = [c for c in read_registry() if not args.cases or c.id in args.cases]
    if not cases:
        die('no case matches %s' % ','.join(args.cases))
    engines = args.engines.split(',')
    load_before = host_load()
    csv = dataset(args.rows)
    if args.duck_port:
        load_duck(args.duck_port, csv)
    results, failed = [], []
    for c in cases:
        log('%s: %s' % (c.id, c.label))
        build(c, args.rows)
        if not args.no_wait:
            wait_quiet(args.wait)
        noisy = busy()
        load = server_load()
        if c.load:
            out, errors = measure_load(c, csv, args.rounds, args.rows)
            failed += ['%s on %s: %s' % (c.id, lane, e) for lane, e in errors.items()]
            for lane, ms in out.items():
                for metric, xs in ms.items():
                    results.append(dict(case=c.id, arm=lane, metric=metric, note=('%s rows, %s' % (format(args.rows, ','), 'bulk groups' if lane == 'sift' else 'heap' if lane == 'native' else lane)), load=load, **stats(xs)))
            continue
        lanes = lane_sessions(c, engines, args.duck_port)
        times, notes, errors = measure_query(c, lanes, args.rounds, args.runs)
        for lane in lanes:
            if lane in errors:
                if lane in ('sift', 'native') or errors[lane].startswith(('PLAN', 'RESULT')):
                    failed.append('%s on %s: %s' % (c.id, lane, errors[lane]))
                results.append(dict(case=c.id, arm=lane, metric='ms', value='NA' if errors[lane].startswith('not supported') else 'error', note=errors[lane], load=load))
            else:
                results.append(dict(case=c.id, arm=lane, metric='ms', note=notes[lane] + (' NOISY' if noisy else ''), load=load, **stats(times[lane])))
            lanes[lane].close()
    print(summarize(results, cases))
    for f in failed:
        print('FAILED ' + f, file=sys.stderr)
    write_results(args, cases, results, load_before)
    return 1 if failed else 0


def host_load():
    pressure = ''
    try:
        pressure = ' cpu_psi %s%%' % re.search(r'some avg10=([0-9.]+)', open('/proc/pressure/cpu').read()).group(1)
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
            'host_load_before': load_before, 'host_load_after': host_load()}
    for r in results:
        r['version'] = version
    path = args.out
    if args.docs:
        path = os.path.join(REPO, 'docs', 'results', 'cases.tsv')
        if os.path.exists(path):   # a run of some cases replaces only their rows, at the size of the others
            old_meta, old = extras.read(path)
            ran = {c.id for c in cases}
            if str(args.rows) != old_meta.get('orders') and ran != {r['case'] for r in old}:
                die('docs/results/cases.tsv holds %s orders: run every case at --rows %d, or these at --rows %s' % (old_meta.get('orders'), args.rows, old_meta.get('orders')))
            results = [r for r in old if r['case'] not in ran] + results
    extras.write(path, 'Benchmark cases of the 3.3 optimizations (bench/cases.tsv): pg_sift against PostgreSQL and the other engines on the orders of the store benchmark.', meta, results)
    log('wrote %s' % path)


def cmd_list(_):
    for c in read_registry():
        print('%-26s %-9s %-30s %s' % (c.id, c.dataset, ','.join(['sift', 'native'] + c.baselines[1:] if c.baselines[:1] == ['native'] else c.baselines), c.label))


# --- the coverage check (scripts/check-bench-cases.sh): no measurement

CLAIM = re.compile(r'\d[\d,.]* ?(?:ms|us|µs|microseconds?|s)? (?:against|->|to|before|was|vs\.?) \d|\d[\d.]*x (?:faster|less|more|smaller|slower)|\b(?:faster|slower)\b|instructions|mispredict')


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
    results = os.path.join(root, 'docs', 'results', 'cases.tsv')
    rows = [l.split('\t') for l in open(results) if l.strip() and not l.startswith('#')][1:] if os.path.exists(results) else []
    measured = {(r[1], r[2], r[3]) for r in rows if len(r) > 4 and r[4] not in ('NA', 'error', '')}
    for f in sorted(os.listdir(os.path.join(root, 'bench', 'cases'))):
        if f.endswith('.sql') and f[:-4] not in ids:
            errors.append('bench/cases/%s is in no row of bench/cases.tsv' % f)
    for c in cases:
        if ids.count(c.id) > 1:
            errors.append('%s: the id is in bench/cases.tsv twice' % c.id)
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
                errors.append('%s: docs/results/cases.tsv has no %s row of %s (bench/bench cases --cases %s --docs)' % (c.id, arm, m, c.id))
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
    ap.add_argument('cmd', choices=['run', 'list', 'check'])
    ap.add_argument('--cases', type=lambda s: s.split(','), default=[])
    ap.add_argument('--engines', default='sift,native')
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--duck-port', type=int, default=0)
    ap.add_argument('--out', default=os.path.join(BENCH, 'out', 'cases-%s.tsv' % time.strftime('%Y%m%dT%H%M%S')))
    ap.add_argument('--docs', action='store_true')
    ap.add_argument('--no-wait', action='store_true')
    ap.add_argument('--wait', type=int, default=300)
    ap.add_argument('--root', default=REPO)
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--changed')
    args = ap.parse_args()
    if args.strict:
        args.changed = None
    sys.exit({'run': cmd_run, 'list': cmd_list, 'check': cmd_check}[args.cmd](args))


if __name__ == '__main__':
    main()
