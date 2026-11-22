#!/usr/bin/env python3
"""ClickBench, the 43 queries (Q0-Q42) on `hits` at 10M rows (fast tier: CI and development) and 100M rows (release tier), for stock PostgreSQL (arm `pg`,
the reference of every md5_equal), pg_sift `sift_table` and `sift_col`, ClickHouse (`ch`, and `ch_proj` with the same projections as pg_sift when this build
has them), and DuckDB. Every answer of every arm is compared with PostgreSQL's (clickbench_md5.py) before its timing is recorded.

    clickbench.py selftest                         the 43 queries, the schema, the generated SQL and the md5_equal normalizer, no server needed
    clickbench.py emit-sql [--check]               write (or check) bench/sql/clickbench/*.sql from clickbench_schema.py and clickbench_queries.py
    clickbench.py parse-check                      EXPLAIN the 43 queries on an empty `hits` of the PostgreSQL the --pg-* flags name
    clickbench.py load    --scale 10m|100m|N       create and fill the arms from one CSV (synthetic, or converted from the real hits parquet files)
    clickbench.py run     --mode warm|cold|both    verify md5_equal, then time every query on every arm that agrees
    clickbench.py report  --in docs/results/clickbench.tsv [--md docs/BENCHMARKS.md] [--svg docs/charts/clickbench.svg]

The real data: `--source parquet --parquet 'DIR/hits_*.parquet'` (or hits.parquet); without it the deterministic synthetic generator of gen.sql is used. Timing:
`wall` is the client's clock around one request, `server` the engine's own figure (PostgreSQL EXPLAIN ANALYZE Execution Time, ClickHouse X-ClickHouse-Summary
elapsed_ns, DuckDB .timer). Cold: the engine's caches are emptied and the operating system's page cache is dropped (--drop-caches-cmd, or POSIX_FADV_DONTNEED
on the files of --pg-datadir, --duckdb-file and the ClickHouse container); the result file says whether it could be."""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import benchlib  # noqa: E402
import clickbench_engines as engines  # noqa: E402
import clickbench_md5 as md5  # noqa: E402
import clickbench_queries as cq  # noqa: E402
import clickbench_schema as schema  # noqa: E402
import resultfile  # noqa: E402

SQL = os.path.join(os.path.dirname(HERE), 'sql', 'clickbench')
ARMS = ['pg', 'sift_table', 'sift_col', 'ch', 'duckdb']
DB = {'pg': 'clickbench_pg', 'sift_table': 'clickbench_sift_table', 'sift_col': 'clickbench_sift_col'}
EMITTED = {'queries.sql': cq.queries_sql, 'gen.sql': schema.gen_sql, 'pg.sql': schema.pg_sql, 'sift_table.sql': schema.sift_table_sql,
           'sift_col.sql': schema.sift_col_sql, 'ch.sql': schema.ch_sql, 'duckdb.sql': schema.duckdb_sql}
log, die = benchlib.logger('clickbench')


def sections(text):
    out, cur = {'': []}, ''
    for line in text.split('\n'):
        m = re.match(r'--@ (\w+)$', line)
        cur = m.group(1) if m else cur
        out.setdefault(cur, [])
        if not m:
            out[cur].append(line)
    return {k: '\n'.join(v).strip() for k, v in out.items()}


def read_sql(name):
    return sections(open(os.path.join(SQL, name)).read())


def parse_scale(s):
    m = re.fullmatch(r'(\d+)([kKmM]?)', s)
    return int(m.group(1)) * {'': 1, 'k': 1000, 'm': 1000000}[m.group(2).lower()] if m else die('--scale: %r is not 10m, 100m or a row count' % s)


def tier(rows):
    return {10000000: '10M', 100000000: '100M'}.get(rows, str(rows))


def pg_conn(a, db):
    return [x for k, v in (('-h', a.pg_host), ('-p', a.pg_port), ('-U', a.pg_user)) if v for x in (k, str(v))] + ['-d', db]


def psql(a, db, sql='', *args, **kw):
    """run SQL text (stdin) or, with args, those psql arguments (a COPY on the stdin / stdout given in kw)"""
    cmd = ['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', *pg_conn(a, db), *(args or ['-f', '-'])]
    return subprocess.run(cmd, input=sql or None, text=True, check=True, **kw)


def psql_value(a, db, sql):
    return psql(a, db, '', '-c', sql, capture_output=True).stdout.strip()


def csv_path(a):
    return os.path.join(a.work, 'hits-%d-%s.csv' % (a.rows, a.source))


PARQUET_FIXES = "make_date(EventDate) AS EventDate, epoch_ms(EventTime * 1000) AS EventTime, epoch_ms(ClientEventTime * 1000) AS ClientEventTime, " \
                "epoch_ms(LocalEventTime * 1000) AS LocalEventTime"


def parquet_select(a):
    texts = ', '.join("replace(%s, chr(0), '') AS %s" % (c, c) for c, k in schema.COLUMNS if k in ('text', 'vc', 'char'))
    return "SELECT * REPLACE (%s, %s) FROM read_parquet('%s', binary_as_string = true) LIMIT %d" % (PARQUET_FIXES, texts, a.parquet, a.rows)


def make_csv(a):
    path, stamp = csv_path(a), csv_path(a) + '.ok'
    if os.path.exists(stamp):
        return path
    os.makedirs(a.work, exist_ok=True)
    t = time.time()
    if a.source == 'parquet':
        sql = "COPY (%s) TO '%s' (FORMAT csv, HEADER false)" % (parquet_select(a), path)
        subprocess.run([a.duckdb_bin, '-noheader', ':memory:', '-c', sql], check=True, env=engines.CLEAN_ENV)
    else:
        with open(path, 'wb') as f:
            psql(a, 'postgres', '', '-v', 'n=%d' % a.rows, '-f', os.path.join(SQL, 'gen.sql'), stdout=f)
    open(stamp, 'w').write('%d\n' % (time.time() - t))
    log('%s: %d rows, %.0f MiB, %.0f s' % (path, a.rows, os.path.getsize(path) / 2**20, time.time() - t))
    return path


def create_db(db):
    """byte order (C collation): the one string order that PostgreSQL, ClickHouse and DuckDB share, so ORDER BY ties and MIN/MAX of text agree"""
    return "CREATE DATABASE %s TEMPLATE template0 ENCODING 'UTF8' LOCALE 'C';" % db


def load_pg(a, arm, csv):
    db, files = DB[arm], read_sql(arm + '.sql')
    psql(a, 'postgres', 'DROP DATABASE IF EXISTS %s WITH (FORCE);\n%s' % (db, create_db(db)))
    psql(a, db, 'CREATE EXTENSION IF NOT EXISTS pg_buffercache;\n' + files['table'])
    t = time.time()
    with open(csv, 'rb') as f:
        psql(a, db, '', '-c', 'COPY hits FROM STDIN WITH (FORMAT csv)', stdin=f)
    load_s = time.time() - t
    psql(a, db, files['after'])
    return load_s


def load_projections(a, arms):
    """pg_sift projections (SIFT_PT_PROJDIR) when this build has sift.create_projection (--projections auto); then ClickHouse gets the same ones"""
    sift = [x for x in ('sift_table', 'sift_col') if x in arms]
    has = bool(sift) and psql_value(a, DB[sift[0]], "SELECT to_regproc('sift.create_projection(regclass,text,text)') IS NOT NULL") == 't'
    on = {'auto': has, 'on': True, 'off': False}[a.projections]
    for arm in sift if on else []:
        psql(a, DB[arm], read_sql(arm + '.sql')['projections'])
    return on


def ch_client(a, arm='ch', table='hits'):
    return engines.ChArm(arm, a.ch_url, a.ch_user, a.ch_password, table, a.threads, engines.OsCache(a.drop_caches_cmd, container=a.ch_container or None))


def load_ch(a, csv, with_proj):
    c, files = ch_client(a), read_sql('ch.sql')
    for t in ('hits', 'hits_proj'):
        c.request('DROP TABLE IF EXISTS ' + t)
    c.request(files['table'].rstrip(';'))
    with open(csv, 'rb') as f:
        c.request('INSERT INTO hits FORMAT CSV', body=f)
    c.request(files['after'].rstrip(';'))
    if with_proj:   # ch_proj: the same rows and the projections of pg_sift, so every comparison is apples to apples
        c.request(files['proj_table'].rstrip(';'))
        for stmt in files['proj_fill'].split(';\n'):
            c.request(stmt.rstrip(';'))


def load_duckdb(a, csv):
    for f in (a.duckdb_file, a.duckdb_file + '.wal'):
        if os.path.exists(f):
            os.remove(f)
    d = engines.DuckArm('duckdb', a.duckdb_bin, a.duckdb_file, a.threads, a.duckdb_memory)
    d.exec(read_sql('duckdb.sql')['table'])
    d.exec("COPY hits FROM '%s' (FORMAT csv, HEADER false, allow_quoted_nulls false)" % csv)
    d.close()


def cmd_load(a):
    csv = make_csv(a)
    want = a.arms or ARMS
    state = dict(rows=a.rows, source=a.source, load_s={})
    for arm in [x for x in want if x in DB]:
        state['load_s'][arm] = round(load_pg(a, arm, csv), 1)
        log('%s loaded in %.1f s' % (arm, state['load_s'][arm]))
    proj = load_projections(a, want)
    arms = want + (['ch_proj'] if proj and 'ch' in want and 'ch_proj' not in want else [])
    if 'ch' in arms or 'ch_proj' in arms:
        load_ch(a, csv, 'ch_proj' in arms)
    if 'duckdb' in arms:
        load_duckdb(a, csv)
    state.update(projections=proj, arms=arms)
    json.dump(state, open(os.path.join(a.work, 'state.json'), 'w'))
    log('loaded: %s (projections %s)' % (', '.join(arms), 'on' if proj else 'off'))


def open_arm(a, arm):
    pg_cache = engines.OsCache(a.drop_caches_cmd, [a.pg_datadir] if a.pg_datadir else [])
    if arm in DB:
        return engines.PgArm(arm, pg_conn(a, DB[arm]), 'hits', a.threads, restart_cmd=a.pg_restart_cmd, os_cache=pg_cache)
    if arm.startswith('ch'):
        return ch_client(a, arm, 'hits_proj' if arm == 'ch_proj' else 'hits')
    return engines.DuckArm(arm, a.duckdb_bin, a.duckdb_file, a.threads, a.duckdb_memory, engines.OsCache(a.drop_caches_cmd, [a.duckdb_file]))


def med(xs):
    return dict(value='%.3f' % statistics.median(xs), min='%.3f' % min(xs), max='%.3f' % max(xs), n=len(xs)) if xs else dict(value='NA')


def cool(arm):
    arm.evict()
    arm.os_cache.drop()


def warm_samples(arm, q, a):
    for _ in range(a.warmups):
        arm.run(q, server=False)
    got = [arm.run(q) for _ in range(a.runs)]
    return [w for w, _ in got], [s for _, s in got if s is not None]


def cold_samples(arm, q, a):
    walls, servers = [], []
    for _ in range(a.cold_runs):
        cool(arm)
        wall, server = arm.run(q, server=False)
        walls.append(wall)
        if server is None:
            cool(arm)
            server = arm.server_only(q)
        servers.append(server)
    return walls, servers


def verify(arms, q, ref_rows):
    equal, notes = {}, {}
    ref = md5.md5s(ref_rows, q.floats)
    for name, arm in arms.items():
        rows = ref_rows if name == 'pg' else arm.rows(q)
        got = md5.md5s(rows, q.floats)
        equal[name] = got[0] == ref[0] and (not q.ordered or got[1] == ref[1])
        notes[name] = '' if equal[name] else (md5.first_diff(ref_rows, rows, q.floats) or 'row order differs')
    return equal, notes, ref[2]


def cmd_run(a):
    state = json.load(open(os.path.join(a.work, 'state.json')))
    a.rows = state['rows']
    names = [x for x in state['arms'] if x in (a.arms or state['arms'])]
    if 'pg' not in names:
        die('arm pg is the reference of md5_equal: load and run it')
    arms = {n: open_arm(a, n) for n in names}
    rows, failed = [], []
    v = a.version
    for q in cq.QUERIES:
        if a.queries and q.n not in a.queries:
            continue
        equal, notes, nrows = verify(arms, q, arms['pg'].rows(q))
        for n in arms:
            rows.append(dict(version=v, case=q.id, arm=n, metric='md5_equal', value=int(equal[n]), note=notes[n]))
        rows.append(dict(version=v, case=q.id, arm='pg', metric='result_rows', value=nrows))
        for n, arm in arms.items():
            if not equal[n]:
                failed.append('%s %s: %s' % (q.id, n, notes[n]))
                continue
            modes = [m for m in (('warm', warm_samples), ('cold', cold_samples)) if a.mode in (m[0], 'both')]
            for mode, sampler in modes:
                walls, servers = sampler(arm, q, a)
                note = 'os_cache=' + arm.os_cache.state if mode == 'cold' else ''
                rows.append(dict(version=v, case=q.id, arm=n, metric=mode + '_ms', note=note, load=resultfile.host_load(), **med(walls)))
                rows.append(dict(version=v, case=q.id, arm=n, metric=mode + '_server_ms', note=note, **med(servers)))
        log('%s: %s' % (q.id, ' '.join('%s=%s' % (n, 'ok' if equal[n] else 'DIFF') for n in arms)))
    ch = {n: arm.log() for n, arm in arms.items() if n.startswith('ch')}
    meta = dict(date=time.strftime('%Y-%m-%d'), cpu=benchlib.host_label(), rows=state['rows'], tier=tier(state['rows']), source=state['source'],
                threads=a.threads, projections='on' if state['projections'] else 'off', arms=','.join(arms),
                os_cache=','.join(sorted({x.os_cache.state for x in arms.values()})),
                method='warm: %d untimed, %d timed runs; cold: %d runs after the engine caches are emptied; wall = client clock, server = engine figure' % (
                    a.warmups, a.runs, a.cold_runs))
    named = {p['name'] for p in schema.PROJECTIONS}
    used = {n.split('.')[-1] for log_ in ch.values() for _, _, p in log_.values() for n in p.split('+') if n.split('.')[-1] in named}
    meta['clickhouse_projections_used'] = ', '.join(sorted(used)) or 'none'
    resultfile.write(a.out, 'ClickBench Q0-Q42 on %s hits rows' % tier(state['rows']), meta, rows)
    for arm in arms.values():
        arm.close()
    if failed:
        die('md5_equal failed:\n  ' + '\n  '.join(failed))
    log('%d queries md5_equal on every arm, written to %s' % (len(a.queries or cq.QUERIES), a.out))


def cmd_emit(a):
    stale = []
    for name, make in EMITTED.items():
        path = os.path.join(SQL, name)
        if a.check:
            stale += [name] if not os.path.exists(path) or open(path).read() != make() else []
        else:
            os.makedirs(SQL, exist_ok=True)
            open(path, 'w').write(make())
    stale and die('out of date, run `clickbench.py emit-sql`: ' + ', '.join(stale))
    log('%s %d files in %s' % ('checked' if a.check else 'wrote', len(EMITTED), SQL))


def cmd_parse_check(a):
    psql(a, 'postgres', 'DROP DATABASE IF EXISTS clickbench_parse WITH (FORCE);\n' + create_db('clickbench_parse'))
    psql(a, 'clickbench_parse', read_sql('pg.sql')['table'] + '\n' + ''.join('EXPLAIN %s;\n' % q.render_verify('pg') for q in cq.QUERIES),
         stdout=subprocess.DEVNULL)
    psql(a, 'postgres', 'DROP DATABASE clickbench_parse WITH (FORCE)')
    log('%d queries and their verify variants plan on an empty hits' % len(cq.QUERIES))


WORDS = set('''SELECT FROM WHERE GROUP BY ORDER LIMIT OFFSET AS AND OR NOT LIKE IN HAVING DESC ASC COUNT SUM AVG MIN MAX DISTINCT CASE WHEN THEN ELSE END
hits REGEXP_REPLACE DATE_TRUNC extract minute length'''.split())


def selftest(a):
    md5.selftest()
    columns = {c for c, _ in schema.COLUMNS}
    assert len(cq.QUERIES) == 43 and len(columns) == 105
    for q in cq.QUERIES:
        for sql in (q.sql, q.verify_sql):
            bare = re.sub(r"'(?:[^']|'')*'", "''", sql)
            assert bare.startswith('SELECT ') and bare.count('(') == bare.count(')') and ';' not in bare, q.id
            aliases = set(re.findall(r'\bAS (\w+)', bare))
            unknown = {w for w in re.findall(r'\b[A-Za-z_]\w*\b', bare) if w not in columns | aliases | WORDS}
            assert not unknown, '%s: unknown words %s' % (q.id, sorted(unknown))
        assert q.render('ch', 'hits_proj').count('hits_proj') == 1 and 'length(' not in q.render('ch'), q.id
        assert all(isinstance(i, int) for i in q.floats)
    assert cq.QUERIES[28].floats == (1,) and cq.with_order('SELECT a FROM hits LIMIT 5', 'a') == 'SELECT a FROM hits ORDER BY a LIMIT 5'
    assert len(schema.used_columns()) <= 32, 'a sift_col index has at most 32 columns'
    for name, make in EMITTED.items():
        assert make().endswith('\n') and '\r' not in make(), name
    for name in ('pg.sql', 'sift_table.sql', 'sift_col.sql', 'ch.sql'):
        assert 'table' in sections(EMITTED[name]()), name
    assert sections(schema.ch_sql())['proj_fill'].count('ADD PROJECTION') == len(schema.PROJECTIONS)
    for name in ('sift_table.sql', 'sift_col.sql'):
        assert sections(EMITTED[name]())['projections'].count('SELECT sift.create_projection') == len(schema.PROJECTIONS), name
    for p in schema.PROJECTIONS:
        assert p['name'] in schema.ch_projection_sql(p) and p['name'] in schema.sift_projection_sql(p, 'hits')
    print('clickbench selftest: ok (43 queries and verify variants, 105 columns, %d generated files, %d projections shared by sift and ClickHouse)' % (
        len(EMITTED), len(schema.PROJECTIONS)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    for name in ('selftest', 'emit-sql', 'parse-check', 'load', 'run', 'report'):
        s = sub.add_parser(name)
        s.add_argument('--pg-host', default=os.environ.get('PGHOST'))
        s.add_argument('--pg-port', default=os.environ.get('PGPORT'))
        s.add_argument('--pg-user', default=os.environ.get('PGUSER'))
        s.add_argument('--pg-datadir', default='', help='the data directory, for POSIX_FADV_DONTNEED before a cold run')
        s.add_argument('--pg-restart-cmd', default='', help='restart the server between cold runs (default: pg_buffercache_evict_all())')
        s.add_argument('--ch-url', default='http://127.0.0.1:8123')
        s.add_argument('--ch-user', default='default')
        s.add_argument('--ch-password', default='')
        s.add_argument('--ch-container', default='', help='a ClickHouse container: its data files leave the page cache before a cold run')
        s.add_argument('--duckdb-bin', default=os.environ.get('DUCKDB_BIN', 'duckdb'))
        s.add_argument('--duckdb-memory', default='16GB')
        s.add_argument('--drop-caches-cmd', default=os.environ.get('DROP_CACHES_CMD', ''))
        s.add_argument('--work', default=os.environ.get('BENCH_WORK', '/tmp/sift-bench') + '/clickbench')
        s.add_argument('--arms', type=lambda x: x.split(','), default=None, help='default for load: %s; for run: every loaded arm' % ','.join(ARMS))
        s.add_argument('--threads', type=int, default=4)
        s.add_argument('--projections', choices=['auto', 'on', 'off'], default='auto')
        s.add_argument('--scale', default='10m')
        s.add_argument('--source', choices=['synthetic', 'parquet'], default='synthetic')
        s.add_argument('--parquet', default='')
        s.add_argument('--check', action='store_true')
        s.add_argument('--mode', choices=['warm', 'cold', 'both'], default='both')
        s.add_argument('--runs', type=int, default=5)
        s.add_argument('--warmups', type=int, default=1)
        s.add_argument('--cold-runs', type=int, default=3)
        s.add_argument('--queries', type=lambda x: [int(i) for i in x.split(',')], default=[])
        s.add_argument('--version', default='6.0')
        s.add_argument('--out', default='docs/results/clickbench.tsv')
        s.add_argument('--in', dest='inp', default='docs/results/clickbench.tsv')
        s.add_argument('--md', default='')
        s.add_argument('--svg', default='')
    a = p.parse_args()
    a.duckdb_file = os.path.join(a.work, 'hits.duckdb')
    a.rows = parse_scale(a.scale)
    if a.source == 'parquet' and not a.parquet and a.cmd == 'load':
        die('--source parquet needs --parquet FILE_OR_GLOB')
    if a.cmd == 'report':
        import clickbench_report
        return clickbench_report.main(a)
    {'selftest': selftest, 'emit-sql': cmd_emit, 'parse-check': cmd_parse_check, 'load': cmd_load, 'run': cmd_run}[a.cmd](a)


if __name__ == '__main__':
    main()
