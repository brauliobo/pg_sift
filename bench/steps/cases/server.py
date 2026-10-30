# The server side of a run: psql, the datasets, the table of a case, the sessions of its lanes and the comparison of rows.
# A lane is an engine or a variant of the sift lane.

import os
import re
import subprocess
from collections import Counter

from duckdb import Session, key as row_key, same

from . import runcfg
from .runcfg import BUILT, COLUMNS, DATASETS, DB, GEN, INDEX_COLUMNS, REPO, TYPES, die, log


# --- the server

ENV = {**os.environ, 'LC_ALL': 'C'}


def psql(sql, db=DB, port=None, *args):
    cmd = ['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres', '-d', db] + (['-h', '127.0.0.1', '-p', str(port)] if port else []) + list(args)
    r = subprocess.run(cmd, input=sql, capture_output=True, text=True, env={**ENV, 'PGOPTIONS': '-c statement_timeout=%ds -c client_min_messages=warning' % runcfg.TIMEOUT})
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
    for schema, pre in [(case.schema(), '')] + ([(case.schema() + '_base', 'SET %s;\n' % case.opt['base'])] if case.base_settings() else []):
        sql = pre + ';\n'.join(d.format(schema) for d in ddl) + ';\n' + case.sec.get('setup', '').replace('{cols}', INDEX_COLUMNS) + ';\n'
        if case.opt.get('vacuum', 'yes') == 'yes':
            # every table of the case, not only orders: without the visibility map the scan checks the heap of each page for its rows (many_groups: 17,668 blocks read against 1,500 of the index)
            sql += "SELECT format('VACUUM (FREEZE, ANALYZE) %%s', oid::regclass) FROM pg_class WHERE relnamespace = '%s'::regnamespace AND relkind = 'r' \\gexec\n" % schema
        psql(sql)
    psql('CHECKPOINT')
    BUILT.add((case.schema(), rows))


def server_session(timeout):
    return Session(os.environ['PGPORT'], timeout, None, db=DB, host=os.environ['PGHOST'])


class Fresh:
    """a lane whose every statement is the first of a new backend (`-- fresh: yes`): a pooled connection that was reset, a script, the first connection of the day. The library is
    loaded by the server at its start (the cases cluster preloads it), so the plan of the first statement is the one of any other"""

    def __init__(self, setup):
        self.setup = setup

    def run(self, sql):
        s = server_session(runcfg.TIMEOUT)
        try:
            for st in self.setup:
                s.run(st)
            return s.run(sql)
        finally:
            s.close()

    def persistent(self):
        """a new backend with the lane's settings, for the measures that need one backend across statements (steps/iolib.py); the caller closes it"""
        s = server_session(runcfg.TIMEOUT)
        for st in self.setup:
            s.run(st)
        return s

    def close(self):
        pass


def lane_sessions(case, engines, duck_port):
    """lane -> Session: sift and native always, then the variants of the registry and the engines the run asked for (the registry lists the ones a case may use)"""
    # a case that turns the access-method scan on (-- set: sift.enable_col_scan = on) compares it with the plan PostgreSQL makes without it
    lanes = {'sift': ['SET sift.enable_agg = on'], 'native': ['SET sift.enable_agg = off'] + (['SET sift.enable_col_scan = off'] if 'sift.enable_col_scan' in case.opt.get('set', '') else [])}
    for name, settings in case.variants:
        lanes[name] = ['SET %s' % s for s in settings.split(';')]
    if case.base_settings():
        lanes['base'] = ['SET sift.enable_agg = on']
    out = {}
    wm = ['SET work_mem = %s' % os.environ['CASES_WORK_MEM']] if os.environ.get('CASES_WORK_MEM') else []   # a large work_mem is 4GB; the cluster default is 64MB
    for lane, sets in lanes.items():
        setup = ['SET search_path = %s, public' % (case.schema() + '_base' if lane == 'base' else case.schema()), 'SET max_parallel_workers_per_gather = %s' % os.environ.get('CASES_WORKERS', '2')] + wm + ['SET %s' % x for x in case.opt.get('set', '').split(';') if x] + sets
        if case.opt.get('fresh') == 'yes' and not lane.startswith('warm'):   # a lane named warm... keeps its backend: the same statement as the second of a backend
            out[lane] = Fresh(setup)
            continue
        out[lane] = server_session(runcfg.TIMEOUT)
        for st in setup:
            out[lane].run(st)
    if 'pg_duckdb' in engines and 'pg_duckdb' in case.baselines and not case.load and duck_port:
        s = Session(duck_port, runcfg.TIMEOUT, None)
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


def tie_keys(ordered):
    """`ties 3` or `ties 3 2` of `-- ordered:` -> [2, 1], the 0-based result columns the statement orders by; None for yes and no"""
    kind, *cols = ordered.split()
    return [int(c) - 1 for c in cols] if kind == 'ties' else None


def equal_ties(ref, got, keys, full):
    """the same page of an ordering whose ties may come in any order. The rows of `ref` and `got` (psql lines) have equal values in the key columns `keys`, row by row. A group of rows
    with one key value inside the page is the same rows in any order; the first and the last group may be cut at the edge of the page (LIMIT, OFFSET), so each of their rows must be one of
    `full(key values)`, all the rows of the ordering with that key value, each counted once: the columns that are not the key (the totals of a window among them) are those of the ordering"""
    if len(ref) != len(got):
        return False
    kv = lambda r: '|'.join(r.split('|')[k] for k in keys)  # noqa: E731
    if not all(same([kv(x)], [kv(y)]) for x, y in zip(ref, got)):
        return False
    groups, lo = [], 0
    for i in range(1, len(ref) + 1):
        if i == len(ref) or kv(ref[i]) != kv(ref[lo]):
            groups.append((lo, i))
            lo = i
    for n, (lo, hi) in enumerate(groups):
        if n in (0, len(groups) - 1):
            left = Counter(map(row_key, full(ref[lo].split('|'))))
            left.subtract(map(row_key, got[lo:hi]))
            if any(v < 0 for v in left.values()):
                return False
        elif not same(ref[lo:hi], got[lo:hi]):
            return False
    return True


IDS = {}
