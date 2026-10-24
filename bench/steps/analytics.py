#!/usr/bin/env python3
"""Measurement of the analytics queries (bench/sql/analytics/queries, or a ClickBench queries.sql): CPU time, instructions and the
result of every query on the engines of the comparison. Called by steps/analytics.sh, which starts the server and loads the data.

    python3 -I analytics.py run [--corpus orders|clickbench] [--engines heap,col,flat,ch] [--sets K10,K400,K4000] [--queries S1,G]
                                [--runs 5] [--out DIR] ...
    python3 -I analytics.py report [DIR|analytics.json] [--markdown]

Engines (the server is the one named by PGHOST / PGPORT / PGDATABASE, never a default one):
  heap  the query as the application writes it, on the heap table, with the GUCs of --base-gucs (default sift.enable_agg = off): the plan
        PostgreSQL has without sift_col
  col   the same text with --col-gucs (default sift.enable_agg = on); present when the table has an index of the access method --col-am
  flat  the flat tables pg_items / pg_metrics / pg_notes (bench/sql/analytics/flat.sql): the best conventional PostgreSQL layout (--flat)
  ch    the tuned ClickHouse layout of a throwaway server (--ch-port, --ch-pid), single thread (--clickhouse)

A measurement is one backend running with max_parallel_workers_per_gather = 0: the CPU time is the on-CPU time of the backend from
/proc/PID/schedstat around the statement (load independent), the instructions are user-space ones counted by `perf stat -p PID` (steps/perf.sh;
NA when perf may not count them). Per query and store set: one warm-up run, whose result is checked against the heap engine's, then RUNS
measured runs with the engines interleaved (the first one rotates every run). ClickHouse is measured the same way: its single query
thread's CPU time from system.query_log and perf stat on the server process.

Results go to DIR/analytics.json in the layout of docs/results (one row per measurement: dataset tier engine class variant metric value
min max unit stderr n values) and are printed as a table. A result differs from the reference when a string or integer differs, or a
number by more than the tolerance (--tol, or the `tol:` line of the query), after sorting the rows by the `key:` columns of the query.
"""
import argparse, glob, json, math, os, re, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from stats import number, summary  # noqa: E402  (steps/stats.py)
SQL = os.path.join(HERE, '..', 'sql', 'analytics')
PERF = os.path.join(HERE, 'perf.sh')
SPAN_A = ('2024-11-01', '2025-10-31')
SPAN_B = ('2023-11-01', '2024-10-31')
SECTIONS = {'heap': 'pg', 'col': 'pg', 'flat': 'flat', 'ch': 'ch'}
NULL = '\\N'


def die(msg):
    sys.exit(f'analytics: {msg}')


def log(msg):
    print(f'[{time.strftime("%T")}] {msg}', file=sys.stderr, flush=True)


# --- the query corpus

class Query:
    def __init__(self, qid, title, sets, key, tol, sql):
        self.id, self.title, self.sets, self.key, self.tol, self.sql = qid, title, sets, key, tol, sql


def parse_query(path):
    """-- ID: title, -- sets: K10 K400 (or -), -- key: 1 2, -- tol: 1e-5, then `--@ pg|flat|ch` sections of SQL"""
    head, sections, cur = {}, {}, None
    for line in open(path).read().split('\n'):
        m = re.match(r'--@ (\w+)$', line)
        if m:
            cur = sections.setdefault(m.group(1), [])
        elif cur is not None:
            cur.append(line)
        else:
            m = re.match(r'-- (sets|key|tol): ?(.*)$', line)
            if m:
                head[m.group(1)] = m.group(2).split()
            elif line.startswith('-- '):
                head.setdefault('title', []).append(line[3:])
    qid = os.path.basename(path)[:-4]
    return Query(qid, ' '.join(head.get('title', [qid])), [] if head.get('sets') == ['-'] else head.get('sets', []),
                 [int(k) - 1 for k in head.get('key', [])], float(head['tol'][0]) if 'tol' in head else None,
                 {k: '\n'.join(v).strip() for k, v in sections.items()})


def load_corpus(corpus, queries_file):
    if corpus == 'clickbench':
        lines = [l.strip() for l in open(queries_file) if l.strip()]
        return [Query(f'Q{i}', lines[i - 1][:70], [], [], None, {'pg': lines[i - 1].rstrip(';')}) for i in range(1, len(lines) + 1)]
    names = sorted(glob.glob(os.path.join(SQL, 'queries', '*.sql')), key=lambda p: (os.path.basename(p)[0] != 'S', os.path.basename(p)))
    return [parse_query(p) for p in names]


def span(col):
    return (f"CASE WHEN {col} >= '{SPAN_A[0]}' AND {col} <= '{SPAN_A[1]}' THEN 'span_a' "
            f"WHEN {col} >= '{SPAN_B[0]}' AND {col} <= '{SPAN_B[1]}' THEN 'span_b' ELSE NULL END")


def expand(sql, stores, sources):
    """{stores} the store ids of the set, {sources} the quoted sources, {range(col)} both ranges, {span(col)} the range label"""
    macros = {'stores': lambda a: stores, 'sources': lambda a: sources, 'span': span,
              'range': lambda a: f"{a} >= '{SPAN_B[0]}' AND {a} <= '{SPAN_A[1]}'"}
    return re.sub(r'\{(\w+)(?:\(([^)]*)\))?\}', lambda m: macros[m.group(1)](m.group(2)), sql)


# --- comparison of results

def cells(text, key):
    lines = text.split('\n')
    if lines[-1] == '':
        lines.pop()
    rows = [[NULL if c in ('', NULL) else c for c in line.split('\t')] for line in lines]
    return sorted(rows, key=lambda r: [r[k] for k in key] if key else r)


def difference(want, got, key, tol):
    """None when equal, else what differs first"""
    a, b = cells(want, key), cells(got, key)
    if len(a) != len(b):
        return f'{len(b)} rows, expected {len(a)}'
    for i, (ra, rb) in enumerate(zip(a, b)):
        if len(ra) != len(rb):
            return f'row {i}: {len(rb)} columns, expected {len(ra)}'
        for j, (x, y) in enumerate(zip(ra, rb)):
            nx, ny = number(x), number(y)
            same = x == y or (nx is not None and ny is not None and abs(nx - ny) <= tol * max(1.0, abs(nx), abs(ny)))
            if not same:
                return f'row {i} column {j + 1}: {y} expected {x} ({"|".join(ra)[:80]})'
    return None


# --- engines

def cpu_ns(pid):
    with open(f'/proc/{pid}/schedstat') as f:
        return int(f.read().split()[0])


class Perf:
    """instructions:u of a process between start() and stop(), counted by steps/perf.sh; unavailable (None) when perf may not count them"""
    def __init__(self):
        self.ok = subprocess.run(['bash', PERF, str(os.getpid()), '--', 'true'], capture_output=True).returncode == 0

    def start(self, pid):
        if not self.ok:
            return None
        p = subprocess.Popen(['bash', PERF, str(pid)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        time.sleep(0.15)
        return p

    def stop(self, p):
        if p is None:
            return None
        p.send_signal(signal.SIGTERM)   # perf.sh ends the count and prints "cycles instructions task-clock-ms"
        fields = p.communicate()[0].split()
        return int(fields[1]) if len(fields) == 3 and fields[1].isdigit() else None


class Postgres:
    """one psql backend, kept for the whole run: its pid is the one whose CPU time is read"""
    def __init__(self, timeout):
        for var in ('PGHOST', 'PGPORT', 'PGDATABASE'):
            if not os.environ.get(var):
                die(f'{var} is not set: the measurement runs on the server of steps/analytics.sh only')
        if os.environ['PGPORT'] == '5432' or not os.environ['PGHOST'].startswith('/'):
            die('refusing to use a server that bench/steps/analytics.sh did not start (PGHOST must be its socket directory)')
        self.p = subprocess.Popen(['psql', '-X', '-q', '-At', '-P', f'null={NULL}', '-F', '\t'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.pid = int(self.run('SELECT pg_backend_pid()')[0])
        self.run('SET max_parallel_workers_per_gather = 0')
        self.run(f'SET statement_timeout = {int(timeout * 1000)}')
        self.run('\\o /dev/null')

    def run(self, sql, capture=False):
        """the lines the statement prints (all of them, or none: the output is discarded unless capture)"""
        if capture:
            self.p.stdin.write('\\o\n')
        self.p.stdin.write(sql + ('' if sql.startswith('\\') else ';') + '\n')
        if capture:
            self.p.stdin.write('\\o /dev/null\n')
        self.p.stdin.write('\\echo __DONE__\n')   # \echo goes to stdout whatever \o says: the end of the output of the statement
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == '':
                die('psql ended: ' + ' '.join(out[-3:]))
            if line.strip() == '__DONE__':
                return out
            out.append(line.rstrip('\n'))

    def scalar(self, sql):
        return self.run(sql, capture=True)[0]

    def settings(self, names):
        """the names this server accepts; a library's setting (`sift.x`) is in pg_settings only after the library loaded, but SET takes it before"""
        return {n for n in names if '.' in n or self.scalar(f"SELECT count(*) FROM pg_settings WHERE name = '{n}'") == '1'}

    def close(self):
        self.p.stdin.close()
        self.p.wait()


class ClickHouse:
    def __init__(self, binary, port, pid, timeout):
        self.cmd = [binary, 'client', '--host', '127.0.0.1', '--port', str(port)]
        self.pid, self.timeout = pid, timeout
        self.n = 0

    def client(self, sql, *opts):
        r = subprocess.run(self.cmd + ['--max_threads', '1', '--max_execution_time', str(self.timeout), *opts, '-q', sql], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr.strip().split('\n')[0][:300])
        return r.stdout

    def result(self, sql):
        return self.client(sql, '--format', 'TSV', '--format_tsv_null_representation', NULL)

    def timed(self, sql, perf):
        self.n += 1
        qid = f'a{os.getpid()}_{self.n}'
        h = perf.start(self.pid)
        t0 = time.perf_counter()
        try:
            self.client(sql, '--format', 'Null', '--query_id', qid)
        finally:
            wall = (time.perf_counter() - t0) * 1e3
            instr = perf.stop(h)
        return qid, wall, instr

    def cpu_ms(self, qids):
        self.client('SYSTEM FLUSH LOGS')
        ids = ','.join(f"'{q}'" for q in qids)
        rows = self.client(f"SELECT query_id, ProfileEvents['UserTimeMicroseconds'] + ProfileEvents['SystemTimeMicroseconds'] FROM system.query_log "
                           f"WHERE type = 'QueryFinish' AND query_id IN ({ids}) FORMAT TSV")
        return {l.split('\t')[0]: float(l.split('\t')[1]) / 1e3 for l in rows.strip().split('\n') if l}


# --- rows

class Rows:
    def __init__(self, dataset, tier, path):
        self.dataset, self.tier, self.rows, self.f = dataset, tier, [], open(path, 'a')

    def add(self, engine, cls, variant, metric, unit, value=None, st=None):
        r = dict(dataset=self.dataset, tier=self.tier, engine=engine, **{'class': cls}, variant=variant, metric=metric, unit=unit)
        r.update(st if st else dict(value=value, min=value, max=value, stderr=None, n=1, values=[value]))
        r['values'] = [round(v, 4) if isinstance(v, float) else v for v in r['values']]
        for k in ('value', 'min', 'max', 'stderr'):
            if isinstance(r[k], float):
                r[k] = round(r[k], 4)
        self.rows.append(r)
        self.f.write(json.dumps(r) + '\n')
        self.f.flush()


# --- the run

def wanted(q, selection):
    return not selection or any(q.id == s or (len(s) == 1 and q.id.startswith(s)) for s in selection)


def engines_available(args, pg, queries, flat_ok):
    have = {'heap': True}
    am = pg.scalar(f"SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam "
                   f"WHERE i.indrelid = '{args.table}'::regclass AND a.amname = '{args.col_am}'")
    have['col'] = am != '0'
    have['flat'] = flat_ok and pg.scalar("SELECT to_regclass('pg_items') IS NOT NULL") == 't' and any('flat' in q.sql for q in queries)
    have['ch'] = bool(args.ch_port) and any('ch' in q.sql for q in queries)
    use = []
    for e in args.engines:
        if e not in SECTIONS:
            die(f'unknown engine {e} (heap, col, flat, ch)')
        if have[e]:
            use.append(e)
        else:
            log(f'skipping {e}: ' + {'col': f'no index of the access method {args.col_am} on {args.table} (the baseline plan is measured alone)',
                                      'flat': 'no flat tables (--flat) or no flat query', 'ch': 'no ClickHouse server (--clickhouse)',
                                      'heap': ''}[e])
    return use


def apply_gucs(pg, gucs, known, applied):
    """SET the GUCs of an engine (applied: what the session has now); the ones that only another engine sets go back to their default"""
    for name in sorted(known):
        value = gucs.get(name)
        if applied.get(name) == value:
            continue
        pg.run(f"SET {name} = '{value}'" if value is not None else f'RESET {name}')
        applied[name] = value


def parse_gucs(text):
    return dict(x.split('=', 1) for x in text.replace(',', ' ').split())


def plan_text(pg, gucs, known, applied, sql):
    apply_gucs(pg, gucs, known, applied)
    return [l[:200] for l in pg.run('EXPLAIN (COSTS OFF) ' + sql, capture=True)]


def measure_task(q, set_name, stores, sources, args, engines, pg, ch, perf, known, rows):
    sql = {e: expand(q.sql[SECTIONS[e]], stores, sources) if args.corpus == 'orders' else q.sql[SECTIONS[e]] for e in engines if SECTIONS[e] in q.sql}
    engines = [e for e in engines if e in sql]
    gucs = {'heap': parse_gucs(args.base_gucs), 'col': parse_gucs(args.col_gucs), 'flat': parse_gucs(args.base_gucs)}
    applied = {}
    variant = set_name or '-'
    tol = q.tol if q.tol is not None else args.tol
    out, runs, fail, plans = {}, {e: dict(cpu=[], wall=[], instr=[], qids=[]) for e in engines}, {}, {}
    # warm-up run per engine: the first result of heap is the reference of the others
    for e in engines:
        try:
            if e == 'ch':
                out[e] = ch.result(sql[e])
            else:
                apply_gucs(pg, gucs[e], known, applied)
                lines = pg.run(sql[e], capture=True)
                bad = [l for l in lines if l.startswith(('ERROR', 'FATAL', 'psql:'))]
                if bad:
                    raise RuntimeError(bad[0][:300])
                out[e] = '\n'.join(lines) + ('\n' if lines else '')
        except RuntimeError as x:
            fail[e] = str(x)
            log(f'{q.id} {variant} {e}: {x}')
    engines = [e for e in engines if e not in fail]
    for e in ('heap', 'col'):
        if e in engines:
            plans[e] = plan_text(pg, gucs[e], known, applied, sql[e])
    for i in range(args.runs):
        for e in engines[i % max(len(engines), 1):] + engines[:i % max(len(engines), 1)]:
            try:
                if e == 'ch':
                    qid, wall, instr = ch.timed(sql[e], perf)
                    runs[e]['qids'].append(qid)
                    runs[e]['wall'].append(wall)
                    runs[e]['instr'].append(instr / 1e6 if instr else None)
                    continue
                apply_gucs(pg, gucs[e], known, applied)
                h = perf.start(pg.pid)
                c0, t0 = cpu_ns(pg.pid), time.perf_counter()
                lines = pg.run(sql[e])
                c1, t1 = cpu_ns(pg.pid), time.perf_counter()
                instr = perf.stop(h)
                bad = [l for l in lines if l.startswith(('ERROR', 'FATAL'))]
                if bad:
                    raise RuntimeError(bad[0][:300])
                runs[e]['cpu'].append((c1 - c0) / 1e6)
                runs[e]['wall'].append((t1 - t0) * 1e3)
                runs[e]['instr'].append(instr / 1e6 if instr else None)
            except RuntimeError as x:
                fail[e] = str(x)
                log(f'{q.id} {variant} {e}: {x}')
                break
    if 'ch' in engines and 'ch' not in fail:
        cpu = ch.cpu_ms(runs['ch']['qids'])
        runs['ch']['cpu'] = [cpu[x] for x in runs['ch']['qids'] if x in cpu]
    for e in engines:
        if e in fail:
            continue
        rows.add(e, q.id, variant, 'cpu_ms', 'ms', st=summary(runs[e]['cpu']))
        rows.add(e, q.id, variant, 'wall_ms', 'ms', st=summary(runs[e]['wall']))
        if any(x is not None for x in runs[e]['instr']):
            rows.add(e, q.id, variant, 'instr_m', 'M instructions', st=summary(runs[e]['instr']))
        if e == 'heap':
            verdict = 'reference'
        elif 'heap' not in out:
            verdict = 'unchecked'
        else:
            diff = difference(out['heap'], out[e], q.key, tol)
            verdict = 'ok' if diff is None else 'DIFF: ' + diff
        rows.add(e, q.id, variant, 'result', 'check', value=verdict)
        rows.add(e, q.id, variant, 'rows', 'rows', value=out[e].count('\n'))
    for e, msg in fail.items():
        rows.add(e, q.id, variant, 'result', 'check', value='error: ' + msg)
    if plans:
        same = plans.get('heap') == plans.get('col') if len(plans) == 2 else None
        marked = bool(re.search(args.col_plan, '\n'.join(plans.get('col', [])))) if 'col' in plans else False
        for e, p in plans.items():
            rows.add(e, q.id, variant, 'plan', 'node', value='baseline' if e == 'heap' else 'sift_col' if marked else 'same as heap' if same else 'other')
        with open(os.path.join(args.out, 'plans.txt'), 'a') as f:
            for e, p in plans.items():
                f.write(f'=== {q.id} {variant} {e}\n' + '\n'.join(p) + '\n')


def sizes(pg, rows, args):
    """the table, its sift_col index and the build facts recorded by the loader"""
    if pg.scalar("SELECT to_regclass('analytics_meta') IS NOT NULL") == 't':
        for line in pg.run('SELECT key, value FROM analytics_meta ORDER BY key', capture=True):
            key, val = line.split('\t')
            if key.endswith('stamp'):
                continue
            rows.add('-', '-', '-', key, 's' if key.endswith('_s') else 'rows', value=number(val) if number(val) is not None else val)
    mib = lambda sql: round(float(pg.scalar(sql)) / 1048576, 1)
    rows.add('-', '-', '-', 'table_mib', 'MiB', value=mib(f"SELECT pg_table_size('{args.table}')"))
    rows.add('-', '-', '-', 'indexes_mib', 'MiB', value=mib(f"SELECT pg_indexes_size('{args.table}')"))
    col = pg.scalar(f"SELECT coalesce(sum(pg_relation_size(i.indexrelid)), 0) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam "
                    f"WHERE i.indrelid = '{args.table}'::regclass AND a.amname = '{args.col_am}'")
    if float(col):
        rows.add('col', '-', '-', 'index_mib', 'MiB', value=round(float(col) / 1048576, 1))


def cmd_run(args):
    os.makedirs(args.out, exist_ok=True)
    queries = [q for q in load_corpus(args.corpus, args.queries_file) if wanted(q, args.queries)]
    if not queries:
        die('no query selected')
    pg = Postgres(args.timeout)
    perf = Perf()
    if not perf.ok:
        log('perf cannot count instructions here (perf_event_paranoid?): instructions are not measured')
    rows = Rows('analytics' if args.corpus == 'orders' else 'clickbench', args.tier, os.path.join(args.out, 'rows.jsonl'))
    engines = engines_available(args, pg, queries, args.corpus == 'orders')
    log(f'{len(queries)} queries, engines {" ".join(engines)}, {args.runs} runs + 1 warm-up')
    ch = ClickHouse(args.ch_bin, args.ch_port, args.ch_pid, args.timeout) if 'ch' in engines else None
    known = pg.settings(list(parse_gucs(args.base_gucs)) + list(parse_gucs(args.col_gucs)))
    for name in set(parse_gucs(args.base_gucs)) | set(parse_gucs(args.col_gucs)):
        if name not in known:
            log(f'{name} is not a setting of this server: not set')
    sets, sources = {}, ''
    if args.corpus == 'orders':
        members = lambda name, quote: pg.scalar(f"SELECT string_agg({quote}, ',' ORDER BY member) FROM analytics_sets WHERE set_name = '{name}'")
        sets = {name: members(name, 'member') for name in args.sets}
        sources = members('sources', 'quote_literal(member)')
    sizes(pg, rows, args)
    for q in queries:
        for set_name in (args.sets if q.sets else ['']):
            if set_name and q.sets and set_name not in q.sets:
                continue
            log(f'{q.id} {set_name or "-"}')
            measure_task(q, set_name, sets.get(set_name, ''), sources, args, engines, pg, ch, perf, known, rows)
    pg.close()
    meta = dict(date=time.strftime('%FT%TZ', time.gmtime()), engines=engines, runs=args.runs, tier=args.tier, base_gucs=args.base_gucs, col_gucs=args.col_gucs,
                perf=perf.ok, method='single backend, max_parallel_workers_per_gather = 0, CPU time from /proc/PID/schedstat')
    with open(os.path.join(args.out, 'analytics.json'), 'w') as f:   # one line per row
        f.write('{"meta": ' + json.dumps(meta) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows.rows) + '\n]}\n')
    report(rows.rows)
    print(f'results: {os.path.join(args.out, "analytics.json")}')
    return 1 if any(r['metric'] == 'result' and str(r['value']).startswith(('DIFF', 'error')) for r in rows.rows) else 0


# --- the report

def report(rows, markdown=False):
    by = {}
    for r in rows:
        if r['class'] != '-':
            by.setdefault((r['class'], r['variant']), {}).setdefault(r['engine'], {})[r['metric']] = r
    engines = [e for e in ['heap', 'col', 'flat', 'ch'] if any(e in v for v in by.values())]
    cpu = lambda d, e: d.get(e, {}).get('cpu_ms', {}).get('value')
    ins = lambda d, e: d.get(e, {}).get('instr_m', {}).get('value')
    num = lambda x, fmt='.1f': format(x, fmt) if x is not None else 'NA'
    ratio = lambda a, b: num(a / b if a and b else None, '.2f')
    head = ['query', 'set'] + [e + ' cpu ms' for e in engines] + ['heap Minstr']
    if 'col' in engines:
        head += ['heap/col', 'instr heap/col']
    head += ['heap/' + e for e in engines if e in ('flat', 'ch')] + ['result']
    lines, speed = [], []
    for (cls, var), d in by.items():
        h = cpu(d, 'heap')
        line = [cls, var] + [num(cpu(d, e)) for e in engines] + [num(ins(d, 'heap'), '.0f')]
        if 'col' in engines:
            line += [ratio(h, cpu(d, 'col')), ratio(ins(d, 'heap'), ins(d, 'col'))]
            if h and cpu(d, 'col'):
                speed.append(h / cpu(d, 'col'))
        line += [ratio(h, cpu(d, e)) for e in engines if e in ('flat', 'ch')]
        checks = [f'{e}: {d[e]["result"]["value"][:60]}' for e in engines if e in d and d[e].get('result', {}).get('value') not in ('ok', 'reference')]
        plan = d.get('col', {}).get('plan', {}).get('value')
        lines.append(line + ['; '.join(checks) if checks else 'ok' + (f' [{plan}]' if plan and plan != 'sift_col' else '')])
    if markdown:
        print('| ' + ' | '.join(head) + ' |')
        print('|' + '|'.join('---' if i < 2 or i == len(head) - 1 else '--:' for i in range(len(head))) + '|')
        for line in lines:
            print('| ' + ' | '.join(line) + ' |')
    else:
        width = [max(len(c[i]) for c in lines + [head]) for i in range(len(head))]
        for line in [head] + lines:
            print('  '.join(c.ljust(w) if i < 2 or i == len(head) - 1 else c.rjust(w) for i, (c, w) in enumerate(zip(line, width))))
    if speed:
        print(f'\ngeometric mean of heap/col over {len(speed)} queries: {math.exp(sum(map(math.log, speed)) / len(speed)):.2f} (above 1: col is cheaper)')
    print('\ncpu ms: median over the runs of the on-CPU time of one backend (ClickHouse: one query thread); heap/X: heap over engine X, above 1 X is cheaper')


def cmd_report(args):
    path = args.dir
    if os.path.isdir(path):
        path = os.path.join(path, 'analytics.json')
    report(json.load(open(path))['rows'], args.markdown)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--corpus', default='orders', choices=['orders', 'clickbench'])
    r.add_argument('--queries-file', help='clickbench: the queries.sql, one query per line')
    r.add_argument('--table', default='orders', help='the table the col index is looked for on (clickbench: hits)')
    r.add_argument('--engines', type=lambda s: s.split(','), default=['heap', 'col', 'flat', 'ch'])
    r.add_argument('--sets', type=lambda s: s.split(','), default=['K10', 'K400', 'K4000'])
    r.add_argument('--queries', type=lambda s: s.split(','), default=[], help='ids or letters (S, G): default all')
    r.add_argument('--runs', type=int, default=5)
    r.add_argument('--timeout', type=int, default=300, help='seconds per statement')
    r.add_argument('--tol', type=float, default=1e-6, help='relative tolerance of numbers')
    r.add_argument('--tier', default='')
    r.add_argument('--out', required=True)
    r.add_argument('--base-gucs', default='sift.enable_agg=off')
    r.add_argument('--col-gucs', default='sift.enable_agg=on')
    r.add_argument('--col-am', default='sift_col')
    r.add_argument('--col-plan', default='(?i)sift', help='regex that the EXPLAIN of the col plan matches when sift_col was used')
    r.add_argument('--ch-bin', default='clickhouse')
    r.add_argument('--ch-port', type=int)
    r.add_argument('--ch-pid', type=int)
    p = sub.add_parser('report')
    p.add_argument('dir')
    p.add_argument('--markdown', action='store_true', help='a Markdown table (for docs)')
    args = ap.parse_args()
    sys.exit({'run': cmd_run, 'report': cmd_report}[args.cmd](args))


main()
