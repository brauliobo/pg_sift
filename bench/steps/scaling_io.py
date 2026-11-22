#!/usr/bin/env python3
"""The probes of bench/bench_scaling.sh (Spec 14 section 4): warm compute scaling (suite A), cold I/O scaling (suite B) and concurrent writer scaling (suite C).

    scaling_io.py load    --rows N                         the wide `events` table, the sift_col index on it, the extensions
    scaling_io.py suite-a --rows N --workers 1,2,4,8       A1-A4 scans and group-bys, A5 parallel CREATE INDEX, A6 parallel VACUUM
    scaling_io.py suite-b --io-method M --data-dir DIR     B1-B4 cold scans for each workers x read-stream setting (the server runs with io_method M)
    scaling_io.py finish  --out FILE                       the speedups and gates of the B rows, after every io_method has run
    scaling_io.py suite-b5 --data-dir DIR                  B5 the hot table survives a cold scan and an index build (needs shared_buffers = 256MB)
    scaling_io.py suite-c --clients 1,4,16 --duration 15   C1 multi-partition writers, C2 FOR KEY SHARE, C3 readers next to writers

Every command appends rows to --out: suite, probe, io_method, workers, rows, median_ms, speedup, blks_read, blks_hit, gate_pass, then read_stream, throughput (MB/s of
a cold scan, rows/s of a warm probe or a build, operations/s of suite C), blk_read_ms (I/O wait of a cold probe), p99_ms and wal (bytes per row of A5, bytes of C2), wait_pct
(samples in lock waits, suite C). gate_pass is pass, fail, `-` (no gate on the row) or `notcold` (a cold probe whose reads were under 90% of its blocks, so it measured the cache); a gate is scaled by
workers / 8 when the ladder stops short of 8 workers. `--control` runs the A and B probes on the heap with no sift index: the stock parallel executor as the reference.
A cold probe evicts the relation from shared_buffers (pg_buffercache_evict_relation) and the page cache (posix_fadvise DONTNEED on its segment files): no root.
Standard library only, psql and pgbench from the PATH.
"""
import argparse
import ctypes
import glob
import json
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from v6_harness import INDEX_COLS, INDEX_KEY_ORDER, INDEX_PATHS, PgClient, events_insert_sql, events_table_ddl  # noqa: E402

COLUMNS = ['suite', 'probe', 'io_method', 'workers', 'rows', 'median_ms', 'speedup', 'blks_read', 'blks_hit', 'gate_pass', 'read_stream', 'throughput', 'blk_read_ms', 'p99_ms', 'wal', 'wait_pct']
RUNS = 3
POSIX_FADV_DONTNEED = 4

def index_ddl(table, name):
    return f"CREATE INDEX {name} ON {table} USING sift_col ({INDEX_COLS}) WITH (key_order = '{INDEX_KEY_ORDER}', paths = '{INDEX_PATHS}')"


SESSION = ["SET jit = off", "SET work_mem = '64MB'"]

A_PROBES = [   # (probe, statement, the gate of 8 workers over 1)
    ('A1_scan_filter', "SELECT count(*), sum(amount), avg(score) FROM events WHERE status BETWEEN 10 AND 60", 6.0),
    ('A2_lowcard_groupby', "SELECT status, metrics->>'channel', count(*), sum(amount) FROM events GROUP BY 1, 2", 6.0),
    ('A3_highcard_groupby', "SELECT group_key, count(*), sum(amount), min(score), max(score) FROM events GROUP BY 1", 5.5),
    ('A4_count_distinct', "SELECT status, count(DISTINCT group_key), count(DISTINCT distinct_tag) FROM events GROUP BY 1", 5.5),
]
WIDE = ("SELECT sum(id), sum(entity_id), max(event_date), count(deleted_at), max(updated_at), sum(group_key), count(distinct_tag), sum(status), sum(amount), avg(score), sum(qty), "
        "count(event_uuid), sum(length(metrics::text)), sum(length(notes::text)), sum(length(analysis::text)) FROM events")
NARROW = "SELECT status, sum(amount) FROM events GROUP BY status"
TABLE_SCAN = "SELECT status, amount FROM events_tbl WHERE group_key BETWEEN 1000 AND 6000"
BM25_SCAN = "SELECT id, sift.score(ctid) FROM docs_bm WHERE body @@> '(w11 OR w12) AND (w13 OR w14)' ORDER BY sift.score(ctid) DESC LIMIT 20"


class Rows:
    def __init__(self, path, rows, control):
        self.path, self.rows, self.control = path, rows, control

    def add(self, suite, probe, **kw):
        row = dict.fromkeys(COLUMNS, '')
        row.update(io_method='-', read_stream='-')
        row.update(kw)
        row.update(suite=suite + ('-control' if self.control else ''), probe=probe, rows=self.rows)
        with open(self.path, 'a') as f:
            f.write('\t'.join(str(row[c]) for c in COLUMNS) + '\n')
        print('  %-14s %-24s %-9s w=%-2s %9s ms  x%-6s read %-8s hit %-8s %s' % (
            row['suite'], probe, row['io_method'], row['workers'], row['median_ms'], row['speedup'], row['blks_read'], row['blks_hit'], row['gate_pass']), file=sys.stderr)


def gate_of(gate8, workers):
    return gate8 * min(1.0, workers / 8.0)


def verdict(value, gate):
    return 'pass' if value >= gate else 'fail'


def explain(pg, statement, settings=()):
    """(exec_ms, shared hit, shared read, io read ms, workers launched) of one EXPLAIN (ANALYZE, BUFFERS) in a session of its own"""
    sql = ';\n'.join(SESSION + list(settings)) + ';\nEXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' + statement + ';\n'
    plan = json.loads(pg._run_psql(['-At'], sql=sql).stdout)[0]
    root = plan['Plan']
    return (plan['Execution Time'], root.get('Shared Hit Blocks', 0), root.get('Shared Read Blocks', 0),
            root.get('Shared I/O Read Time', root.get('I/O Read Time', 0.0)), root.get('Workers Launched', 0))


def median_run(pg, statement, settings=(), before=None):
    runs = []
    for _ in range(RUNS):
        if before:
            before()
        runs.append(explain(pg, statement, settings))
    mid = sorted(runs, key=lambda r: r[0])[len(runs) // 2]
    return statistics.median(r[0] for r in runs), mid


def libc_fadvise(path):
    libc = ctypes.CDLL('libc.so.6', use_errno=True)
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
        rc = libc.posix_fadvise(fd, ctypes.c_longlong(0), ctypes.c_longlong(0), ctypes.c_int(POSIX_FADV_DONTNEED))
        if rc:
            raise OSError(rc, 'posix_fadvise failed on ' + path)
    finally:
        os.close(fd)


def evict_cold(pg, data_dir, rel):
    """shared_buffers and the page cache hold nothing of `rel`, its TOAST table and its indexes (PostgreSQL 18: pg_buffercache_evict_relation)"""
    pg.exec_sql('CHECKPOINT')
    oids = pg.query_scalar(f"SELECT string_agg(oid::text, ',') FROM (SELECT '{rel}'::regclass::oid AS oid UNION SELECT reltoastrelid FROM pg_class WHERE oid = '{rel}'::regclass AND reltoastrelid <> 0 "
                           f"UNION SELECT indexrelid FROM pg_index WHERE indrelid = '{rel}'::regclass) s").split(',')
    for oid in oids:
        pg.query_scalar(f"SELECT count(*) FROM (SELECT pg_buffercache_evict_relation({oid}::oid::regclass)) e")
        base = pg.query_scalar(f"SELECT pg_relation_filepath({oid}::oid::regclass)")
        for path in [os.path.join(data_dir, base)] + sorted(glob.glob(os.path.join(data_dir, base) + '.[0-9]*')):
            if os.path.exists(path):
                libc_fadvise(path)


def cold(pg, args, rel):
    return lambda: evict_cold(pg, args.data_dir, rel)


def is_cold(read, hit):
    return read + hit > 0 and read >= 0.90 * (read + hit)


def pg_of(args):
    return PgClient(args.host, args.port, args.db, args.user)


def load_events(pg, nrows, control):
    pg.exec_sql("CREATE EXTENSION IF NOT EXISTS pg_buffercache;\n" + ("" if control else "CREATE EXTENSION IF NOT EXISTS pg_sift;\n") + events_table_ddl('events')
                + events_insert_sql('events', 1, nrows) + "\nVACUUM (ANALYZE, FREEZE) events;")


def cmd_load(args):
    pg = pg_of(args)
    load_events(pg, args.rows, args.control)
    if not args.control:
        pg.exec_sql("SET maintenance_work_mem = '256MB'; " + index_ddl('events', 'events_sift') + "; VACUUM events;")
    print('loaded %d rows%s: heap %s MB' % (args.rows, '' if args.control else ' and the sift_col index', pg.query_scalar("SELECT pg_table_size('events') / 1048576")), file=sys.stderr)


def workers_of(args):
    return [int(w) for w in args.workers.split(',')]


def timed_ddl(pg, sql, setup):
    lsn0 = pg.get_wal_lsn()
    t0 = time.monotonic()
    pg.exec_sql(setup + ';\n' + sql)
    return (time.monotonic() - t0) * 1000.0, pg.wal_diff_bytes(lsn0, pg.get_wal_lsn())


def a_build(pg, out, args, ladder):
    base = None
    for w in ladder:
        ddl = 'CREATE INDEX events_scale ON events (entity_id, event_date, group_key, status)' if args.control else index_ddl('events', 'events_scale')
        ms, wal = timed_ddl(pg, ddl, f"SET max_parallel_maintenance_workers = {w}; SET maintenance_work_mem = '256MB'")
        pg.exec_sql('DROP INDEX events_scale')
        base = base or ms
        speed = base / ms
        out.add('A', 'A5_parallel_build', workers=w, median_ms='%.1f' % ms, speedup='%.2f' % speed, blks_read=0, blks_hit=0, throughput='%.0f' % (args.rows / ms * 1000),
                gate_pass=('pass' if speed >= gate_of(5.0, w) and (args.control or wal / args.rows <= 42) else 'fail') if w > 1 else '-', wal='%.1f' % (wal / args.rows))


def a_vacuum(pg, out, args, ladder):
    base, n = None, args.rows // 4
    for w in ladder:
        pg.exec_sql(events_table_ddl('events_v') + events_insert_sql('events_v', 1, n))
        if not args.control:
            pg.exec_sql(index_ddl('events_v', 'events_v_sift'))
        pg.exec_sql("DELETE FROM events_v WHERE id % 5 = 0")
        ms, _ = timed_ddl(pg, f"VACUUM (PARALLEL {w}) events_v", "SET maintenance_work_mem = '256MB'")
        pg.exec_sql('DROP TABLE events_v')
        base = base or ms
        speed = base / ms
        out.add('A', 'A6_parallel_vacuum', workers=w, median_ms='%.1f' % ms, speedup='%.2f' % speed, blks_read=0, blks_hit=0, throughput='%.1f' % (n * 0.2 / ms * 1000),
                gate_pass=verdict(speed, gate_of(5.0, w)) if w > 1 else '-')


def cmd_suite_a(args):
    pg, out, ladder = pg_of(args), Rows(args.out, args.rows, args.control), workers_of(args)
    for probe, statement, gate8 in A_PROBES:
        base = None
        for w in ladder:
            settings = [f"SET max_parallel_workers_per_gather = {w}", "SET parallel_leader_participation = off"]
            ms, (_, hit, read, _, _) = median_run(pg, statement, settings)
            base = base or ms
            speed = base / ms
            out.add('A', probe, workers=w, median_ms='%.2f' % ms, speedup='%.2f' % speed, blks_read=read, blks_hit=hit, throughput='%.0f' % (args.rows / ms * 1000),
                    gate_pass=verdict(speed, gate_of(gate8, w)) if w > 1 else '-')
    a_build(pg, out, args, ladder)
    a_vacuum(pg, out, args, ladder)


def prepare_b(pg, args):
    if not args.control:
        pg.exec_sql(f"CREATE TABLE IF NOT EXISTS events_tbl (LIKE events) USING sift_table")
        if pg.query_scalar("SELECT count(*) FROM events_tbl") == '0':
            pg.exec_sql(f"SELECT sift.table_set_option('events_tbl', 'key_order', '{INDEX_KEY_ORDER}');\nINSERT INTO events_tbl SELECT * FROM events;\nCREATE INDEX events_tbl_gk ON events_tbl (group_key);\nVACUUM events_tbl")
        pg.exec_sql("CREATE TABLE IF NOT EXISTS docs_bm (id bigint, body text)")
        if pg.query_scalar("SELECT count(*) FROM docs_bm") == '0':
            for part in range(8):   # eight segments: the index is built on the first batch and each later batch is a segment of its own
                lo, hi = part * 25000 + 1, (part + 1) * 25000
                pg.exec_sql(f"INSERT INTO docs_bm SELECT i, (SELECT string_agg('w' || ((i * k * 7 + k) % 600), ' ') FROM generate_series(1, 12) k) FROM generate_series({lo}, {hi}) i"
                            + ("; CREATE INDEX docs_bm_body ON docs_bm USING sift (body)" if part == 0 else ''))
            pg.exec_sql("VACUUM docs_bm")


def b_probes(args):
    """(probe, statement, relation, settings that make the plan of the probe, the gate: speedup against sync without a read stream)"""
    plan = [("SET enable_seqscan = off", "SET enable_indexscan = off")]
    probes = [('B1_cold_wide_scan', WIDE, 'events', [], 3.5), ('B2_cold_narrow_scan', NARROW, 'events', [], 3.0)]
    if not args.control:
        probes += [('B3_cold_table_bitmap', TABLE_SCAN, 'events_tbl', list(plan[0]), 2.5), ('B4_cold_bm25', BM25_SCAN, 'docs_bm', [], 2.5)]
    return probes


def has_read_stream_switch(pg):
    """whether the build has the A/B switch of the read stream: release builds always stream, only SIFT_TESTING builds register `sift.io_read_stream`"""
    return pg.query_scalar("LOAD 'pg_sift'; SELECT count(*) FROM pg_settings WHERE name = 'sift.io_read_stream'") == '1'


def cmd_suite_b(args):
    pg, out, ladder = pg_of(args), Rows(args.out, args.rows, args.control), workers_of(args)
    prepare_b(pg, args)
    switchable = not args.control and has_read_stream_switch(pg)
    streams = ['on', 'off'] if switchable else ['on']
    for probe, statement, rel, extra, _ in b_probes(args):
        for stream in streams:
            for w in ladder:
                settings = [f"SET max_parallel_workers_per_gather = {w}", "SET parallel_leader_participation = off"] + extra
                if switchable:
                    settings.append(f"SET sift.io_read_stream = {stream}")
                ms, (_, hit, read, io_ms, _) = median_run(pg, statement, settings, cold(pg, args, rel))
                out.add('B', probe, io_method=args.io_method, workers=w, median_ms='%.1f' % ms, speedup='', blks_read=read, blks_hit=hit, gate_pass='-' if is_cold(read, hit) else 'notcold',
                        read_stream=stream, throughput='%.0f' % (read * 8192 / 1048576 / (ms / 1000)), blk_read_ms='%.1f' % io_ms)


B_GATES = {'B1_cold_wide_scan': 3.5, 'B2_cold_narrow_scan': 3.0, 'B3_cold_table_bitmap': 2.5, 'B4_cold_bm25': 2.5}


def cmd_finish(args):
    """the speedups and gates of the B rows, which need rows of other server runs: B1 against sync without a read stream, the others against the same io_method without one;
    the gate is on the io_uring row of the first worker count with the stream on; B2 also reads at least 80% fewer blocks than B1"""
    with open(args.out) as f:
        rows = [dict(zip(COLUMNS, line.rstrip('\n').split('\t'))) for line in f if line.strip()]
    key = lambda r: (r['suite'], r['probe'], r['io_method'], r['read_stream'], r['workers'])  # noqa: E731
    by = {key(r): r for r in rows}
    for r in rows:
        if not r['suite'].startswith('B') or r['probe'] not in B_GATES:
            continue
        method = 'sync' if r['probe'] == 'B1_cold_wide_scan' else r['io_method']
        ref = by.get((r['suite'], r['probe'], method, 'off', r['workers'])) or by.get((r['suite'], r['probe'], method, 'on', r['workers']))
        if ref is None or not float(ref['median_ms']):
            continue
        r['speedup'] = '%.2f' % (float(ref['median_ms']) / float(r['median_ms']))
        first = min(int(x['workers']) for x in rows if x['probe'] == r['probe'])
        if r['io_method'] == 'io_uring' and r['read_stream'] == 'on' and int(r['workers']) == first and r['gate_pass'] != 'notcold':
            ok = float(r['speedup']) >= B_GATES[r['probe']]
            if r['probe'] == 'B2_cold_narrow_scan':
                wide = by.get((r['suite'], 'B1_cold_wide_scan', r['io_method'], 'on', r['workers']))
                ok = ok and wide is not None and int(r['blks_read']) <= 0.2 * int(wide['blks_read'])
            r['gate_pass'] = 'pass' if ok else 'fail'
    with open(args.out, 'w') as f:
        for r in rows:
            f.write('\t'.join(str(r[c]) for c in COLUMNS) + '\n')


def cmd_suite_b5(args):
    pg, out = pg_of(args), Rows(args.out, args.rows, args.control)
    pg.exec_sql("DROP TABLE IF EXISTS hot_t; CREATE TABLE hot_t AS SELECT i AS id, md5(i::text) AS pad FROM generate_series(1, 1800000) i; VACUUM hot_t")
    blocks = int(pg.query_scalar("SELECT relpages FROM pg_class WHERE relname = 'hot_t'"))
    explain(pg, "SELECT count(*) FROM hot_t")
    evict_cold(pg, args.data_dir, 'events')
    explain(pg, WIDE)
    if not args.control:
        pg.exec_sql(index_ddl('events', 'events_isolation') + '; DROP INDEX events_isolation')
    _, (_, hit, read, _, _) = median_run(pg, "SELECT count(*) FROM hot_t")
    ratio = hit / max(1, hit + read)
    out.add('B', 'B5_buffer_isolation', workers=0, median_ms='', speedup='%.3f' % ratio, blks_read=read, blks_hit=hit, gate_pass=verdict(ratio, 0.90), throughput=blocks)


class Sampler(threading.Thread):
    """wait events of the active backends every 10 ms, from one psql session that repeats the query with \\watch"""
    def __init__(self, pg):
        super().__init__(daemon=True)
        self.proc = subprocess.Popen(pg.base_cmd + ['-At', '-F', '|'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.proc.stdin.write("SELECT coalesce(wait_event_type, '-'), coalesce(wait_event, '-') FROM pg_stat_activity WHERE datname = 'bench' AND state = 'active' AND pid <> pg_backend_pid() \\watch 0.01\n")
        self.proc.stdin.flush()
        self.samples = []

    def run(self):
        for line in self.proc.stdout:
            if '|' in line:
                self.samples.append(line.strip().split('|'))

    def stop(self):
        self.proc.kill()
        self.join(timeout=2)
        waiting = [s for s in self.samples if s[0] in ('LWLock', 'Lock', 'BufferContent', 'BufferPin')]
        return 100.0 * len(waiting) / max(1, len(self.samples)), self.samples


def pgbench(pg, args, scripts, clients, duration, log_dir):
    cmd = ['pgbench', '-n', '-D', f'n={c_rows(args)}', '-h', args.host, '-p', str(args.port), '-U', args.user, '-c', str(clients), '-j', str(min(clients, 8)), '-T', str(duration), '-l', f'--log-prefix={log_dir}/pgb', '--sampling-rate=1']
    for path, weight in scripts:
        cmd += ['-f', f'{path}@{weight}']
    out = subprocess.run(cmd + [args.db], capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError('pgbench failed:\n' + out.stderr)
    latencies = []
    for log in glob.glob(f'{log_dir}/pgb*'):
        with open(log) as f:
            latencies += [int(line.split()[2]) / 1000.0 for line in f if len(line.split()) >= 3]
        os.remove(log)
    latencies.sort()
    pick = lambda q: latencies[min(len(latencies) - 1, int(len(latencies) * q))] if latencies else 0.0  # noqa: E731
    return len(latencies) / duration, pick(0.50), pick(0.99)


WRITE_SCRIPTS = {
    'ins': "WITH n AS (SELECT nextval('w_seq') AS i) INSERT INTO events_w SELECT i, 1 + (i - 1) % 2000, DATE '2025-01-01' + (i % 400)::int, NULL, now(), 1 + (i % 50000), 'tag-' || (i % 5000), "
           "(i % 100)::smallint, (i % 10000) / 100.0, (i % 1000) / 10.0, 1 + (i % 50), md5(i::text)::uuid, '{\"channel\":\"web\",\"latency\":12.5,\"region\":\"na\"}'::jsonb, "
           "'{\"tag\":\"n-1\",\"priority\":1,\"summary\":\"w\"}'::jsonb, '{}'::jsonb FROM n;\n",
    'upd': "\\set rid random(1, :n)\n\\set eid 1 + (:rid - 1) % 2000\nUPDATE events_w SET amount = amount + 1 WHERE entity_id = :eid AND id = :rid;\n",
    'del': "\\set rid random(1, :n)\n\\set eid 1 + (:rid - 1) % 2000\nDELETE FROM events_w WHERE entity_id = :eid AND id = :rid;\n",
    'read': "SELECT status, count(*), sum(amount) FROM events_w GROUP BY status;\nSELECT id, amount FROM events_w WHERE entity_id = 7 ORDER BY event_date DESC LIMIT 20;\n",
    'lock': "\\set pid random(1, 1000)\nSELECT id FROM parent_t WHERE id = :pid FOR KEY SHARE;\n",
    'child': "\\set pid random(1, 1000)\nINSERT INTO child_t VALUES (nextval('c_seq'), :pid);\n",
}


def prepare_c(pg, args):
    index = '' if args.control else index_ddl('events_w', 'events_w_sift') + ';\n'
    pg.exec_sql("DROP TABLE IF EXISTS events_w, child_t, parent_t CASCADE; DROP SEQUENCE IF EXISTS w_seq, c_seq;\n"
                "CREATE TABLE events_w (LIKE events INCLUDING DEFAULTS) PARTITION BY HASH (entity_id);\n"
                "DO $$ BEGIN FOR p IN 0..63 LOOP EXECUTE format('CREATE TABLE events_w_%s PARTITION OF events_w FOR VALUES WITH (modulus 64, remainder %s) "
                "WITH (autovacuum_enabled = false)', p, p); END LOOP; END $$;\n"
                f"CREATE SEQUENCE w_seq START {args.rows + 1};\nINSERT INTO events_w SELECT * FROM events WHERE id <= {c_rows(args)};\n"
                "UPDATE events_w SET entity_id = 1 + (id - 1) % 2000;\nCREATE UNIQUE INDEX events_w_pk ON events_w (entity_id, id);\n" + index + "VACUUM (ANALYZE) events_w")
    kind = '' if args.control else 'USING sift_table'
    pg.exec_sql(f"CREATE TABLE parent_t (id int PRIMARY KEY) {kind}; INSERT INTO parent_t SELECT generate_series(1, 1000);\n"
                f"CREATE SEQUENCE c_seq; CREATE TABLE child_t (id bigint, parent_id int REFERENCES parent_t (id)) {kind};")


def c_rows(args):
    return min(200000, args.rows)


def cmd_suite_c(args):
    pg, out = pg_of(args), Rows(args.out, args.rows, args.control)
    prepare_c(pg, args)
    clients, results = [int(c) for c in args.clients.split(',')], {}
    with tempfile.TemporaryDirectory() as tmp:
        scripts = {}
        for name, text in WRITE_SCRIPTS.items():
            scripts[name] = os.path.join(tmp, name + '.sql')
            with open(scripts[name], 'w') as f:
                f.write(text)
        mix = [(scripts['ins'], 6), (scripts['upd'], 3), (scripts['del'], 1)]
        for c in clients:
            sampler = Sampler(pg)
            sampler.start()
            ops, p50, p99 = pgbench(pg, args, mix, c, args.duration, tmp)
            wait, _ = sampler.stop()
            results[c] = ops
            speed = ops / results[clients[0]]
            gate = verdict(speed, 8.0 * min(1.0, max(clients) / 16.0)) if c == max(clients) and c > clients[0] else '-'
            if gate == 'pass' and wait >= 5.0:
                gate = 'fail'
            out.add('C', 'C1_multi_partition_writes', workers=c, median_ms='%.2f' % p50, speedup='%.2f' % speed, blks_read=0, blks_hit=0, gate_pass=gate, throughput='%.0f' % ops,
                    p99_ms='%.2f' % p99, wait_pct='%.2f' % wait)
        lsn0 = pg.get_wal_lsn()
        ops, p50, _ = pgbench(pg, args, [(scripts['lock'], 1)], max(clients), max(2, args.duration // 3), tmp)
        wal = pg.wal_diff_bytes(lsn0, pg.get_wal_lsn())
        out.add('C', 'C2_key_share_locks', workers=max(clients), median_ms='%.3f' % p50, speedup='', blks_read=0, blks_hit=0, gate_pass='pass' if wal <= 4096 else 'fail', throughput='%.0f' % ops, wal=wal)
        readers = max(1, max(clients) // 2)
        _, idle50, idle99 = pgbench(pg, args, [(scripts['read'], 1)], readers, max(3, args.duration // 3), tmp)
        sampler = Sampler(pg)
        sampler.start()
        done = {}
        writer = threading.Thread(target=lambda: done.update(w=pgbench(pg, args, mix, readers, max(3, args.duration // 3) + 1, tmp)))
        writer.start()
        _, mixed50, mixed99 = pgbench(pg, args, [(scripts['read'], 1)], readers, max(3, args.duration // 3), tmp)
        writer.join()
        wait, _ = sampler.stop()
        out.add('C', 'C3_mixed_readers_writers', workers=readers, median_ms='%.2f' % mixed50, speedup='%.2f' % (mixed99 / idle99 if idle99 else 0), blks_read=0, blks_hit=0,
                gate_pass=verdict(1.15, mixed99 / idle99) if idle99 else '-', throughput='%.0f' % done['w'][0], p99_ms='%.2f' % mixed99, wait_pct='%.2f' % wait)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['load', 'suite-a', 'suite-b', 'finish', 'suite-b5', 'suite-c'])
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--db', default='bench')
    ap.add_argument('--user', default='postgres')
    ap.add_argument('--data-dir')
    ap.add_argument('--rows', type=int, default=2000000)
    ap.add_argument('--workers', default='1,2,4,8')
    ap.add_argument('--clients', default='1,4,16')
    ap.add_argument('--duration', type=int, default=15)
    ap.add_argument('--io-method', default='-')
    ap.add_argument('--out', required=True)
    ap.add_argument('--control', action='store_true')
    args = ap.parse_args()
    if args.cmd in ('suite-b', 'suite-b5') and not args.data_dir:
        ap.error(args.cmd + ' needs --data-dir (the segment files it advises the kernel to drop)')
    {'load': cmd_load, 'suite-a': cmd_suite_a, 'suite-b': cmd_suite_b, 'finish': cmd_finish, 'suite-b5': cmd_suite_b5, 'suite-c': cmd_suite_c}[args.cmd](args)


if __name__ == '__main__':
    main()
