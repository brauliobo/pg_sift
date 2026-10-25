#!/usr/bin/env python3
"""pg_duckdb against pg_sift and PostgreSQL on the orders of the orders benchmark (bench/bench duckdb, steps/duckdb.sh).

    python3 -I bench/steps/duckdb.py run --sift-port P --duck-port P --out FILE --data DIR [--rows N] [--runs 3] [--timeout 30] [--version V] [--meta KEY=VALUE ...]

Two PostgreSQL 18 servers hold the same CSV (the generator of bench/sql/store/gen.sql): the pg_sift image (a `sift_col` index and a BM25 index) and the pg_duckdb image
(the heap with btree and GIN indexes, plus the same rows as a Parquet file that pg_duckdb writes). Four lanes run the ten questions of bench/sql/store/queries:

    pg              PostgreSQL's own executor on the heap of the pg_duckdb server (duckdb.force_execution = off)
    sift            the pg_sift server
    duckdb_heap     pg_duckdb with duckdb.force_execution = on: DuckDB reads the heap through its Postgres scanner
    duckdb_parquet  pg_duckdb, a view over read_parquet() of the file it wrote

Warm: one persistent session per lane, one untimed run, then the median of --runs (more runs, up to three times as many, while the spread of a cell is over 30% of its median). Cold: the median of --runs first runs, each in a new session right after
pg_buffercache_evict_all() (the OS page cache stays; the Parquet lane has nothing to evict). A statement DuckDB cannot execute does not fail under force_execution:
pg_duckdb warns and runs it on the PostgreSQL executor, so the plan decides: no `DuckDBScan` in it is "not supported". Where a question has a `--@ duck` section, that
statement stands in for it and the row says so. Every lane's rows are compared with PostgreSQL's. A question is measured again while the host is busy (pressure stall
information or load average above the limits). The rows go to a results file of steps/extras.py.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import benchlib  # noqa: E402  (steps/benchlib.py)
import extras  # noqa: E402
import pgq  # noqa: E402  (steps/pgq.py)
from store_lib import load_queries  # noqa: E402

SQL = os.path.join(HERE, '..', 'sql', 'store')
PARQUET = '/var/lib/postgresql/orders.parquet'   # in the data volume of the pg_duckdb container
PSI_MAX = float(os.environ.get('SIFT_QUIET_MAX', 10))
LOAD_MAX = float(os.environ.get('D_LOAD_MAX', 12))
SPREAD = 0.3
# arm, server, table, duckdb.force_execution (None: no such setting on that server)
LANES = [('pg', 'duck', 'orders', 'off'), ('sift', 'sift', 'orders', None), ('duckdb_heap', 'duck', 'orders', 'on'), ('duckdb_parquet', 'duck', 'orders_pq', 'on')]
COLUMNS = ['id', 'customer_id', 'country', 'category', 'status', 'amount', 'quantity', 'created_at', 'title']
TYPES = ['bigint', 'int', 'text', 'text', 'text', 'numeric(10,2)', 'smallint', 'timestamp', 'text']
ENV = {**os.environ, 'LC_ALL': 'C'}


log = benchlib.logger('duckdb')[0]


def psql(port, db, *args, stdin=None):
    p = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-h', '127.0.0.1', '-p', str(port), '-U', 'postgres', '-d', db, *args], input=stdin, capture_output=True, text=True, env=ENV)
    if p.returncode:
        sys.exit('duckdb: psql on port %s failed: %s' % (port, p.stderr.strip()))
    return p.stdout.strip()


def section(file, name):
    out, on = [], False
    for line in open(os.path.join(SQL, file)):
        m = re.match(r'--@ (\w+)$', line)
        if m:
            on = m.group(1) == name
        elif on:
            out.append(line)
    return ''.join(out)


def busy():
    pressure = float(re.search(r'some avg10=([0-9.]+)', open('/proc/pressure/cpu').read()).group(1))
    return pressure >= PSI_MAX or float(open('/proc/loadavg').read().split()[0]) >= LOAD_MAX


def wait_quiet(limit=300):
    """sleeps, never spins, until the host is quiet; False when it is not within limit seconds"""
    t = time.time()
    while busy():
        if time.time() - t > limit:
            return False
        time.sleep(5)
    return True


class Session(pgq.Backend):
    """one psql process kept open; run() returns (milliseconds as psql timed the statement, its rows, the error or warning)"""

    def __init__(self, port, timeout, setting, db='bench', host='127.0.0.1'):
        super().__init__(['psql', '-X', '-qAt', '-F', '|', '-h', host, '-p', str(port), '-U', 'postgres', '-d', db], {**ENV, 'PGOPTIONS': '-c statement_timeout=%ds' % timeout},
                         setup=['\\set VERBOSITY terse'], strict=False)
        if setting:
            self.run('SET duckdb.force_execution = %s' % setting)

    def run(self, sql):
        lines = super().run('\\timing on\n%s;\n\\timing off' % sql.rstrip(';'), capture=True)
        ms = next((float(m.group(1)) for l in reversed(lines) if (m := re.match(r'Time: ([0-9.]+) ms', l))), None)
        err = next((l[6:].strip() for l in lines if l.startswith('ERROR:')), '')
        warn = next((l[8:].strip() for l in lines if l.startswith('WARNING:')), '')
        rows = [l for l in lines if not l.startswith(('Time:', 'ERROR:', 'WARNING:', 'NOTICE:'))]
        return ms, rows, err, warn


def key(row):
    return tuple(round(float(x), 4) if re.fullmatch(r'-?[0-9]+\.[0-9]+', x) else x for x in row.split('|'))


def same(a, b):
    """equal rows in any order; numbers to a relative 1e-9 (an average of money is exact in PostgreSQL and a double in DuckDB)"""
    if len(a) != len(b):
        return False
    for x, y in zip(sorted(a, key=key), sorted(b, key=key)):
        for u, v in zip(x.split('|'), y.split('|')):
            if u != v:
                try:
                    if abs(float(u) - float(v)) > 1e-9 * max(1.0, abs(float(u))):
                        return False
                except ValueError:
                    return False
    return True


def text(sql, n, table):
    sql = sql.replace('{point_id}', str(n * 42 // 100))
    return sql if table == 'orders' else re.sub(r'\borders\b', table, sql)


def cell(session, q, arm, table, args):
    """(timed ms of --runs runs, the rows, the note, the statement); the ms are empty for a question the lane cannot run, and the note says why"""
    sql = text(q.sec.get('sift' if arm == 'sift' else 'pg', q.sec['pg']), args.rows, table)
    note = ''
    if arm.startswith('duckdb'):
        while not any('DuckDBScan' in l for l in session.run('EXPLAIN ' + sql)[1]):
            reason = session.run(sql)[3] or 'it runs on the PostgreSQL executor'
            if note or 'duck' not in q.sec:
                return [], None, 'not supported: ' + reason[:110], sql
            sql, note = text(q.sec['duck'], args.rows, table), 'substring LIKE form (tsvector is not supported)'
    elif arm == 'sift':
        note = 'SiftAgg' if any('SiftAgg' in l for l in session.run('EXPLAIN ' + sql)[1]) else 'normal plan'
    out, times, i = [], [], 0
    # the first run is not timed; a cell whose runs spread by more than SPREAD of their median is run again, up to three times --runs (the harness's rule for noisy cells)
    while i <= args.runs or (len(times) < 3 * args.runs and max(times) - min(times) > SPREAD * statistics.median(times)):
        ms, out, err, _ = session.run(sql)
        if err:
            what = 'timeout %d s' % args.timeout if 'timeout' in err else err[:110]
            return [], None, ('not supported: ' if arm.startswith('duckdb') and 'timeout' not in err else 'error: ') + what, sql
        if i:
            times.append(ms)
        i += 1
    return times, out, note, sql


def measure(sessions, queries, args):
    rows, used = [], {}
    for q in queries:
        for attempt in range(3):
            quiet = wait_quiet()
            cells = {arm: cell(sessions[arm], q, arm, table, args) for arm, _, table, _ in LANES}
            noisy = not quiet or busy()
            if not noisy:
                break
            log('%s: the host was busy, measuring again' % q.name)
        for arm, _, _, _ in LANES:
            ms, out, note, used[q.id, arm] = cells[arm]
            if ms and arm != 'pg' and cells['pg'][1] is not None:
                if not same(out, cells['pg'][1]):
                    note += ' RESULT DIFFERS'
                elif 'substring' in note:
                    note += ', same rows'
            rows.append(dict(version=args.version, case='%s %s' % (q.id, q.name), arm=arm, metric='warm_ms', note=(note + (' NOISY' if noisy else '')).strip(), load=open('/proc/loadavg').read().split()[0],
                             **stats(ms, note)))
    return rows, used


def stats(ms, note):
    if not ms:
        return dict(value='NA' if note.startswith('not supported') else 'error')
    return dict(value='%.3f' % statistics.median(ms), min='%.3f' % min(ms), max='%.3f' % max(ms), n=len(ms))


def cold(queries, ports, used, args, rows):
    for q in queries:
        for arm, srv, table, setting in LANES:
            warm = next(r for r in rows if r['case'] == '%s %s' % (q.id, q.name) and r['arm'] == arm and r['metric'] == 'warm_ms')
            times = []
            if warm['value'] not in ('NA', 'error'):
                for _ in range(args.runs):
                    wait_quiet()
                    if table == 'orders':
                        psql(ports[srv], 'bench', '-c', 'SELECT pg_buffercache_evict_all()')
                    s = Session(ports[srv], args.timeout, setting)
                    times.append(s.run(used[q.id, arm])[0])
                    s.close()
            rows.append(dict(version=args.version, case=warm['case'], arm=arm, metric='cold_ms', note=warm['note'], load=open('/proc/loadavg').read().split()[0],
                             **(stats(times, warm['note']) if times else dict(value=warm['value']))))


def load(port, ddl, label, csv, extension):
    """a `bench` database with the table of bench/sql/store/DDL and the CSV in it; returns the load and index build seconds"""
    psql(port, 'postgres', '-c', 'DROP DATABASE IF EXISTS bench WITH (FORCE)', '-c', 'CREATE DATABASE bench')
    psql(port, 'bench', '-c', 'CREATE EXTENSION pg_buffercache')
    if extension:
        psql(port, 'bench', '-c', 'CREATE EXTENSION %s' % extension)
    psql(port, 'bench', stdin=section(ddl, 'table'))
    t = time.time()
    with open(csv) as f:
        subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-h', '127.0.0.1', '-p', str(port), '-U', 'postgres', '-d', 'bench', '-c', 'COPY orders FROM STDIN CSV'], stdin=f, check=True, env=ENV)
    load_s = time.time() - t
    t = time.time()
    psql(port, 'bench', stdin=section(ddl, 'indexes'))
    index_s = time.time() - t
    psql(port, 'bench', '-c', 'VACUUM (ANALYZE) orders', '-c', 'CHECKPOINT')
    log('%s: loaded in %.1f s, indexes in %.1f s' % (label, load_s, index_s))
    return load_s, index_s


def build_rows(port, arm, seconds, version):
    mib = lambda sql: '%.1f' % (int(psql(port, 'bench', '-c', sql)) / 1048576)  # noqa: E731
    return [dict(version=version, case='load', arm=arm, metric=m, value=v) for m, v in (
        ('load_s', '%.1f' % seconds[0]), ('index_s', '%.1f' % seconds[1]), ('table_mib', mib("SELECT pg_table_size('orders')")), ('index_mib', mib("SELECT pg_indexes_size('orders')")))]


def host():
    cpu = next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name')), '?')
    mem = int(next(l.split()[1] for l in open('/proc/meminfo') if l.startswith('MemTotal'))) // 1048576
    return '%s, %d threads, %d GB memory' % (cpu, os.cpu_count(), mem)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['run'])
    ap.add_argument('--sift-port', type=int, required=True)
    ap.add_argument('--duck-port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--timeout', type=int, default=30)
    ap.add_argument('--version', default='')
    ap.add_argument('--meta', action='append', default=[])
    ap.add_argument('--queries', default='', help='comma separated question numbers (default all)')
    args = ap.parse_args()
    ports = {'sift': args.sift_port, 'duck': args.duck_port}
    load_before = extras.host_load()

    csv = os.path.join(args.data, 'orders-%d.csv' % args.rows)
    if not os.path.exists(csv):
        os.makedirs(args.data, exist_ok=True)
        psql(args.duck_port, 'postgres', '-v', 'n=%d' % args.rows, '-f', os.path.join(SQL, 'gen.sql'))
        with open(csv + '.part', 'w') as f:
            subprocess.run(['psql', '-X', '-qAt', '-h', '127.0.0.1', '-p', str(args.duck_port), '-U', 'postgres', '-d', 'postgres', '-c', 'COPY (SELECT * FROM orders_src ORDER BY id) TO STDOUT CSV'], stdout=f, check=True, env=ENV)
        os.rename(csv + '.part', csv)
        psql(args.duck_port, 'postgres', '-c', 'DROP TABLE orders_src')

    rows = build_rows(args.sift_port, 'sift', load(args.sift_port, 'sift.sql', 'pg_sift', csv, None), args.version)
    rows += build_rows(args.duck_port, 'pg', load(args.duck_port, 'pg.sql', 'pg_duckdb server', csv, 'pg_duckdb'), args.version)
    t = time.time()
    psql(args.duck_port, 'bench', '-c', "COPY orders TO '%s' (FORMAT parquet)" % PARQUET)
    pq_s = time.time() - t
    cols = ', '.join("r['%s']::%s AS %s" % (c, t_, c) for c, t_ in zip(COLUMNS, TYPES))
    psql(args.duck_port, 'bench', '-c', "CREATE VIEW orders_pq AS SELECT %s FROM read_parquet('%s') r" % (cols, PARQUET))
    pq_mib = int(psql(args.duck_port, 'bench', '-c', "SELECT size FROM pg_stat_file('%s')" % PARQUET)) / 1048576
    rows += [dict(version=args.version, case='load', arm='duckdb_parquet', metric='load_s', value='%.1f' % pq_s), dict(version=args.version, case='load', arm='duckdb_parquet', metric='table_mib', value='%.1f' % pq_mib)]
    settings = ', '.join('%s %s' % (n[7:], psql(args.duck_port, 'bench', '-c', 'SHOW %s' % n)) for n in ('duckdb.threads', 'duckdb.threads_for_postgres_scan', 'duckdb.max_workers_per_postgres_scan', 'duckdb.max_memory'))
    engine = psql(args.duck_port, 'bench', '-c', "SELECT r['v'] FROM duckdb.query('SELECT version() AS v') r")

    queries = [q for q in load_queries() if not args.queries or q.id in args.queries.split(',')]
    sessions = {arm: Session(ports[srv], args.timeout, setting) for arm, srv, _, setting in LANES}
    log('warm: %d questions in %d lanes' % (len(queries), len(LANES)))
    measured, used = measure(sessions, queries, args)
    rows += measured
    for s in sessions.values():
        s.close()
    log('cold')
    cold(queries, ports, used, args, rows)

    meta = dict(m.split('=', 1) for m in args.meta)
    meta.update(date=time.strftime('%Y-%m-%d', time.gmtime()), cpu=host(), duckdb='DuckDB %s (%s)' % (engine, settings), host_load_before=load_before, host_load_after=extras.host_load())
    extras.write(args.out, 'pg_duckdb against pg_sift and PostgreSQL on the orders of the orders benchmark (%d orders).' % args.rows, meta, rows)
    log('wrote %s' % args.out)


if __name__ == '__main__':
    main()
