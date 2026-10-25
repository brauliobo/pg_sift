#!/usr/bin/env python3
"""The generic suites (TPC-H-like, ClickBench-like, time series: bench/cases.tsv datasets tpch, hits, tseries) on the other engines: a pg_duckdb server and ClickHouse, with the answers
checked against PostgreSQL's before any time is taken. Standard library, psql and docker only.

    python3 -I generic.py run [--rows N] [--suites tpch,hits,tseries] [--engines pg_duckdb,clickhouse] [--runs 5] [--check-only] [--work DIR] [--out FILE] [--port P]

One container at a time: the pg_duckdb image (PostgreSQL 18 with pg_duckdb) generates the suite with bench/sql/SUITE/gen.sql (its pg_sift index lines left out), answers every query with
PostgreSQL's own plan (the reference) and with `duckdb.force_execution = on`, and exports each table to CSV in --work; it is removed, and ClickHouse (bench/sql/SUITE/ch.sql) loads the
CSV and answers the same queries (a `--@ ch` section of the case replaces the statement where the dialects differ). Rows are `arm ms` of the warm median, like bench/results/cases.tsv; the
pg_sift number of each query is the one of cases.tsv, so the comparison is made by steps/compare.py from the two files.
"""
import argparse
import datetime
import http.client
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cases as cases_mod  # noqa: E402
from duckdb import Session  # noqa: E402  (a persistent psql session)

REPO = os.path.dirname(os.path.dirname(HERE))
SQL = os.path.join(REPO, 'bench', 'sql')
NAME = 'sift-generic'
DUCK_FLAGS = ['-c', 'shared_buffers=2GB', '-c', 'effective_cache_size=6GB', '-c', 'work_mem=64MB', '-c', 'maintenance_work_mem=512MB', '-c', 'jit=off', '-c', 'max_wal_size=8GB']
TABLES = {  # suite -> table -> the columns exported (their order is that of ch.sql)
    'tpch': {'customer': 'c_custkey, c_name, c_nationkey, c_mktsegment, c_acctbal', 'orders': 'o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_shippriority',
             'lineitem': 'l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_shipdate, l_commitdate, l_receiptdate, l_returnflag, l_linestatus, l_shipmode, l_shipinstruct'},
    'hits': {'hits': 'watch_id, event_time, event_date, counter_id, client_ip, region_id, user_id, os, user_agent, url, referer, is_refresh, resolution_width, mobile_phone_model, search_phrase, adv_engine_id'},
    'tseries': {'metrics': 'ts, device, metric, value'},
}


def log(msg):
    print('[%s generic] %s' % (time.strftime('%T'), msg), file=sys.stderr, flush=True)


def sh(*cmd, check=True, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if check and p.returncode:
        sys.exit('generic: %s failed: %s' % (' '.join(cmd[:4]), (p.stderr or p.stdout).strip()[:400]))
    return p.stdout.strip()


def env_images():
    out = sh('bash', '-c', '. %s/bench/versions.env; echo "$STORE_DUCKDB_IMAGE $STORE_CH_IMAGE"' % REPO).split()
    return out[0], out[1]


def norm(cell):
    """one cell to a comparable value: a timestamp to microseconds, a number to 6 significant digits, anything else as text"""
    cell = cell.strip()
    if re.fullmatch(r'\d{4}-\d\d-\d\d', cell):   # pg_duckdb returns a date for date_trunc('day', ts), PostgreSQL a timestamp at midnight
        return datetime.date.fromisoformat(cell).isoformat() + 'T00:00:00'
    if re.fullmatch(r'\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(\.\d+)?', cell):
        return datetime.datetime.fromisoformat(cell.replace(' ', 'T')).isoformat()
    try:
        return float('%.6g' % float(cell))
    except ValueError:
        return cell


def canon(rows):
    """rows (lists of text cells) in their order; the query has an ORDER BY, and a tie is a failure of the query, not of the engine"""
    return [[norm(c) for c in r] for r in rows]


def same(a, b):
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if len(x) != len(y):
            return False
        for u, v in zip(x, y):
            if u != v and not (isinstance(u, float) and isinstance(v, float) and abs(u - v) <= 1e-5 * max(1.0, abs(u))):
                return False
    return True


def first_diff(a, b):
    for x, y in zip(a, b):
        if x != y:
            return ' (first difference: %s against %s)' % (x, y)
    return ''


def suite_cases(suites):
    return [c for c in cases_mod.read_registry() if c.dataset in suites]


def stripped_gen(suite):
    """gen.sql without the lines that build a pg_sift index: the reference server has none"""
    return ''.join(l for l in open(os.path.join(SQL, suite, 'gen.sql')) if 'USING sift_col' not in l)


class Duck:
    """the pg_duckdb server: the reference PostgreSQL plans (force_execution off) and DuckDB's (on) over the same heap tables"""

    def __init__(self, image, port, cpus, memory):
        self.name, self.port, self.image, self.cpus, self.memory = NAME + '-duck', port, image, cpus, memory

    def up(self):
        sh('docker', 'rm', '-fv', self.name, check=False)
        sh('docker', 'run', '-d', '--name', self.name, '--label', 'sift-generic=1', '--cpus', str(self.cpus), '--memory', self.memory, '--memory-swap', self.memory, '--shm-size', '1g',
           '-p', '127.0.0.1:%d:5432' % self.port, '-e', 'POSTGRES_HOST_AUTH_METHOD=trust', self.image, 'postgres', *DUCK_FLAGS, '-c', 'max_parallel_workers_per_gather=%d' % (self.cpus - 1),
           '-c', 'duckdb.threads=%d' % self.cpus, '-c', 'duckdb.threads_for_postgres_scan=%d' % self.cpus, '-c', 'duckdb.max_workers_per_postgres_scan=%d' % (self.cpus - 1))
        for _ in range(300):
            if subprocess.run(['pg_isready', '-q', '-h', '127.0.0.1', '-p', str(self.port)]).returncode == 0:
                break
            time.sleep(1)
        else:
            sys.exit('generic: the pg_duckdb server did not start (docker logs %s)' % self.name)
        self.psql('postgres', 'DROP DATABASE IF EXISTS bench')
        self.psql('postgres', 'CREATE DATABASE bench')
        self.psql('bench', 'CREATE EXTENSION IF NOT EXISTS pg_duckdb')

    def psql(self, db, sql, *args, stdin=None):
        return sh('psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-h', '127.0.0.1', '-p', str(self.port), '-U', 'postgres', '-d', db, '-c', sql, *args, input=stdin)

    def generate(self, suite, rows):
        p = subprocess.run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-v', 'n=%d' % rows, '-h', '127.0.0.1', '-p', str(self.port), '-U', 'postgres', '-d', 'bench', '-f', '-'],
                           input=stripped_gen(suite), capture_output=True, text=True, env={**os.environ, 'PGOPTIONS': '-c client_min_messages=warning'})
        if p.returncode:
            sys.exit('generic: generating %s failed: %s' % (suite, p.stderr.strip()[:400]))

    def export(self, suite, work):
        for table, cols in TABLES[suite].items():
            path = os.path.join(work, table + '.csv')
            with open(path, 'w') as f:
                subprocess.run(['psql', '-X', '-q', '-h', '127.0.0.1', '-p', str(self.port), '-U', 'postgres', '-d', 'bench', '-c', 'COPY (SELECT %s FROM %s) TO STDOUT CSV' % (cols, table)],
                               stdout=f, check=True)

    def down(self):
        sh('docker', 'rm', '-fv', self.name, check=False)


class ClickHouse:
    def __init__(self, image, port, cpus, memory):
        self.name, self.port, self.image, self.cpus, self.memory, self.conn = NAME + '-ch', port, image, cpus, memory, None

    def up(self):
        sh('docker', 'rm', '-fv', self.name, check=False)
        sh('docker', 'run', '-d', '--name', self.name, '--label', 'sift-generic=1', '--cpus', str(self.cpus), '--memory', self.memory, '--memory-swap', self.memory, '--ulimit', 'nofile=262144:262144',
           '-p', '127.0.0.1:%d:8123' % self.port, '-e', 'CLICKHOUSE_USER=bench', '-e', 'CLICKHOUSE_PASSWORD=bench', self.image)
        for _ in range(120):
            try:
                if self.query('SELECT 1', db='default').strip() == '1':
                    return
            except (OSError, RuntimeError, http.client.HTTPException):
                self.conn = None
            time.sleep(1)
        sys.exit('generic: ClickHouse did not start (docker logs %s)' % self.name)

    def query(self, sql, body=None, settings='', db='bench'):
        """the response text; sql travels in the URL when a body is sent (an INSERT), else it is the body"""
        for attempt in (0, 1):
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=600)
                path = '/?database=%s&default_format=TabSeparated&max_threads=%d&date_time_input_format=best_effort%s' % (db, self.cpus, settings)
                headers = {'X-ClickHouse-User': 'bench', 'X-ClickHouse-Key': 'bench'}
                if body is None:
                    self.conn.request('POST', path, body=sql.encode(), headers=headers)
                else:
                    self.conn.request('POST', path + '&query=' + urlquote(sql), body=body, headers=headers)
                r = self.conn.getresponse()
                data = r.read().decode(errors='replace')
                if r.status >= 400:
                    raise RuntimeError('ClickHouse %d: %s' % (r.status, data[:300]))
                return data
            except (http.client.HTTPException, OSError) as e:
                self.conn = None
                if attempt:
                    raise RuntimeError(str(e))

    def create_database(self):
        self.query('CREATE DATABASE IF NOT EXISTS bench', db='default')

    def load(self, suite, work):
        for stmt in [s for s in open(os.path.join(SQL, suite, 'ch.sql')).read().split(';\n') if s.strip()]:
            self.query(stmt)
        for table in TABLES[suite]:
            with open(os.path.join(work, table + '.csv'), 'rb') as f:
                self.query('INSERT INTO %s FORMAT CSV' % table, body=f.read())

    def down(self):
        sh('docker', 'rm', '-fv', self.name, check=False)


def urlquote(s):
    from urllib.parse import quote
    return quote(s, safe='')


def timed(fn, runs):
    """(median ms, min, max, answer) of `runs` timed executions after one untimed; the answer of the last"""
    answer = fn()[1]
    ms = []
    for _ in range(runs):
        t, answer = fn()
        ms.append(t)
    return statistics.median(ms), min(ms), max(ms), answer


def psql_run(session):
    def run(sql):
        def go():
            ms, rows, err, _ = session.run(sql)
            if err:
                raise RuntimeError(err)
            return ms, [r.split('|') for r in rows]
        return go
    return run


def ch_run(ch):
    def run(sql):
        def go():
            t = time.perf_counter()
            text = ch.query(sql)
            return (time.perf_counter() - t) * 1000, [l.split('\t') for l in text.split('\n') if l != '']
        return go
    return run


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['run'])
    ap.add_argument('--rows', type=int, default=1000000)
    ap.add_argument('--suites', default='tpch,hits,tseries')
    ap.add_argument('--engines', default='pg_duckdb,clickhouse')
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--cpus', type=int, default=3)
    ap.add_argument('--memory', default='8g')
    ap.add_argument('--timeout', type=int, default=60)
    ap.add_argument('--check-only', action='store_true', help='answer every query once and compare; no times')
    ap.add_argument('--work', default=os.environ.get('GENERIC_WORK', '/tmp/sift-generic'))
    ap.add_argument('--out')
    ap.add_argument('--docs', action='store_true', help='write bench/results/generic.tsv')
    ap.add_argument('--port', type=int, default=56180)
    a = ap.parse_args()
    suites, engines = a.suites.split(','), a.engines.split(',')
    if a.docs:
        a.out = os.path.join(REPO, 'bench', 'results', 'generic.tsv')
    os.makedirs(a.work, exist_ok=True)
    duck_image, ch_image = env_images()
    cases = suite_cases(suites)
    runs = 0 if a.check_only else a.runs
    results, failures, reference = [], [], {}   # (case, arm) -> (median, min, max); (case) -> the PostgreSQL answer

    duck = Duck(duck_image, a.port, a.cpus, a.memory)
    try:
        log('pg_duckdb server (one container), %d rows of each suite' % a.rows)
        duck.up()
        for suite in suites:
            wd = os.path.join(a.work, suite)
            os.makedirs(wd, exist_ok=True)
            duck.generate(suite, a.rows)
            duck.export(suite, wd)
        pg = Session(a.port, a.timeout, None, db='bench')
        dk = Session(a.port, a.timeout, 'on', db='bench')
        for c in cases:
            sql = c.sec['pg']
            if runs:
                med, lo, hi, ans = timed(psql_run(pg)(sql), runs)
                results.append((c.id, 'pg', med, lo, hi))
            else:
                ans = psql_run(pg)(sql)()[1]
            reference[c.id] = canon(ans)
            if not ans or ans == [['']]:
                log('%s: the answer is empty at %d rows, so the engines agree on nothing' % (c.id, a.rows))
            if 'pg_duckdb' in engines:
                plan = '\n'.join(dk.run('EXPLAIN ' + c.sec.get('duck', sql))[1])
                if 'DuckDBScan' not in plan:
                    results.append((c.id, 'pg_duckdb', None, None, None))
                    log('%s: pg_duckdb does not run it (no DuckDBScan in the plan)' % c.id)
                    continue
                fn = psql_run(dk)(c.sec.get('duck', sql))
                if runs:
                    med, lo, hi, got = timed(fn, runs)
                else:
                    med = lo = hi = None
                    got = fn()[1]
                got = canon(got)
                if not same(reference[c.id], got):
                    failures.append('%s: pg_duckdb answers differ from PostgreSQL (%d rows against %d)%s' % (c.id, len(got), len(reference[c.id]), first_diff(reference[c.id], got)))
                elif runs:
                    results.append((c.id, 'pg_duckdb', med, lo, hi))
        pg.close()
        dk.close()
    finally:
        duck.down()

    if 'clickhouse' in engines:
        ch = ClickHouse(ch_image, a.port + 1, a.cpus, a.memory)
        try:
            log('ClickHouse server (one container)')
            ch.up()
            ch.create_database()
            for suite in suites:
                ch.load(suite, os.path.join(a.work, suite))
            run = ch_run(ch)
            for c in cases:
                sql = c.sec.get('ch', c.sec['pg'])
                try:
                    if runs:
                        med, lo, hi, got = timed(run(sql), runs)
                    else:
                        med = lo = hi = None
                        got = run(sql)()[1]
                except RuntimeError as e:
                    failures.append('%s: ClickHouse: %s' % (c.id, str(e)[:200]))
                    continue
                if not same(reference[c.id], canon(got)):
                    failures.append('%s: ClickHouse answers differ from PostgreSQL (%d rows against %d)%s' % (c.id, len(got), len(reference[c.id]), first_diff(reference[c.id], canon(got))))
                elif runs:
                    results.append((c.id, 'clickhouse', med, lo, hi))
        finally:
            ch.down()

    ok = len({c.id for c in cases}) - len({f.split(':')[0] for f in failures})
    log('%d of %d queries answered alike by every engine asked%s' % (ok, len(cases), '' if not failures else '; ' + '; '.join(failures)))
    if a.out and runs:
        with open(a.out, 'w') as f:
            f.write('# The generic suites on the other engines (steps/generic.py): pg_duckdb and ClickHouse against PostgreSQL 18 (the pg_duckdb image), the same rows, answers compared before any time.\n')
            f.write('# rows: %d\n# date: %s\n# method: client-side milliseconds, warm median of %d runs after one untimed; %d CPUs and %s per container, one container at a time; pg is PostgreSQL 18.1 of the pg_duckdb image with its own plans\n'
                    % (a.rows, time.strftime('%Y-%m-%d'), runs, a.cpus, a.memory))
            f.write('version\tcase\tarm\tmetric\tvalue\tmin\tmax\tn\n')
            for case, arm, med, lo, hi in results:
                f.write('\t'.join([str(x) for x in ('3.4', case, arm, 'ms', 'NA' if med is None else '%.3f' % med, '' if lo is None else '%.3f' % lo, '' if hi is None else '%.3f' % hi, runs)]) + '\n')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
