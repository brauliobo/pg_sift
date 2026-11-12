#!/usr/bin/env python3
"""The scope index against tuned stock PostgreSQL on a real-shaped fixture: nine read classes, eight tenants, one arm at a time.

    python3 -I bench/steps/scope_stock_arms.py run --root PREFIX --dir SCRATCH --port N --out RESULTS [--arms a,b] [--facts 2400000]
    python3 -I bench/steps/scope_stock_arms.py report RESULTS

The fixture (bench/sql/scope_stock/base.sql): 2.4 million events of 2,648 dim_a_id values (log-normal counts, ordered by time), 32 tenants that reach them directly or through groups
(3,600 distinct pairs, 1.45 tenants a member), the eight measured tenants hold 49, 20, 17, 6, 6, 4, 1 and 0.1% of the events. Each arm is built in a copy of the fixture database and
dropped after its reads and writes; the fixture and the arms are on one cluster (a tmpfs directory). Arms:
  stock_default  the b-tree on the member the application has, the semi join as written (the oracle of the md5 of every cell)
  semijoin       covering (member, time) b-tree, partial b-tree for the filter class, per-member LATERAL probe for latest-N; nothing maintained
  denorm_flat    a copy of the events by (tenant, event), b-tree (tenant, time), kept by triggers (events, pair edits)
  denorm_part    the same copy as a list partition by tenant
  rollup         rollups by member kept by a statement trigger; the classes read them through the semi join (latest-N: not answerable)
  ivm            pg_ivm incremental views of events joined to the maintained pair table (latest-N: not answerable)
  sift_reuse     the sift_col entity index, the pairs read through the member key
  sift_scope     the same index with the scope of two paths
The stock arms run on a server without pg_sift loaded; the sift arms on the same data directory restarted with it. Every cell: warm (median of 3 of `instructions:u` of the backend, EXPLAIN
ANALYZE time and shared blocks hit/read) and cold (shared buffers emptied: one run), the md5 of the rows against the oracle. Writes: 10,000 inserted events (statement), a pair edit
(grant a member to a tenant, revoke it) for 10 dim_a_id values of ~400 events and for the largest member; the sift arm adds the promotion of the write buffer. Size: bytes of what the arm adds.
"""
import argparse
import csv
import math
import os
import re
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SQL = os.path.join(HERE, '..', 'sql', 'scope_stock')
TENANTS = (1, 2, 3, 4, 5, 6, 7, 8)
EDIT_TENANT = 6    # the tenant whose pairs the edits change; none of the measured reads is on it for its dim_a_id values
ENV = dict(os.environ, LC_ALL='C')

SEMI = 'dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = {t})'
LATERAL_LATEST = ('SELECT id, created_at FROM (SELECT l.id, l.created_at FROM (SELECT member FROM tenant_pairs WHERE tenant_id = {t}) m, '
                  'LATERAL (SELECT id, created_at FROM events WHERE dim_a_id = m.member ORDER BY created_at DESC, id DESC LIMIT 20) l) x '
                  'ORDER BY created_at DESC, id DESC LIMIT 20')
MONTH = "date_trunc('month', created_at)"


def event_classes(src, pred):
    """the nine classes over a table of events (or of their per-tenant copy) and a predicate"""
    return {
        'sum_by_month': f"SELECT {MONTH} m, count(*), sum(amount) FROM {src} WHERE {pred} AND created_at >= '2023-01-01' GROUP BY 1",
        'count_by_score': f"SELECT score, count(*) FROM {src} WHERE {pred} GROUP BY 1",
        'latest20': f"SELECT id, created_at FROM {src} WHERE {pred} ORDER BY created_at DESC, id DESC LIMIT 20",
        'top_members': f"SELECT dim_a_id, count(*) c FROM {src} WHERE {pred} GROUP BY 1 ORDER BY c DESC, 1 LIMIT 20",
        'distinct_by_month': f"SELECT {MONTH} m, count(DISTINCT dim_a_id) FROM {src} WHERE {pred} AND created_at >= '2022-01-01' GROUP BY 1",
        'by_member': f"SELECT dim_a_id, count(*), sum(amount) FROM {src} WHERE {pred} GROUP BY 1",
        'filtered_count': f"SELECT count(*) FROM {src} WHERE {pred} AND channel_id = 9 AND region = 'DE'",
        'date_range_count': f"SELECT count(*) FROM {src} WHERE {pred} AND created_at >= '2025-06-01' AND created_at < '2026-01-01'",
        'count_by_kind': f"SELECT kind, count(*) FROM {src} WHERE {pred} AND created_at >= '2025-01-01' GROUP BY 1",
    }


def semi_classes():
    return event_classes('events', SEMI)


def lateral_classes():
    return dict(semi_classes(), latest20=LATERAL_LATEST)


def copy_classes(table):
    return event_classes(table, 'tenant_id = {t}')


def rollup_classes():
    """over the member rollups; the semi join picks the dim_a_id values, a month filter is on the month start (every filter of the classes is)"""
    m = 'dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = {t})'
    return {
        'sum_by_month': f"SELECT month::timestamptz m, sum(n), sum(amount_sum) FROM r_month WHERE {m} AND month >= '2023-01-01' GROUP BY month",
        'count_by_score': f"SELECT score, sum(n) FROM r_score WHERE {m} GROUP BY 1",
        'top_members': f"SELECT dim_a_id, sum(n) c FROM r_month WHERE {m} GROUP BY 1 ORDER BY c DESC, 1 LIMIT 20",
        'distinct_by_month': f"SELECT month::timestamptz m, count(DISTINCT dim_a_id) FROM r_month WHERE {m} AND month >= '2022-01-01' GROUP BY month",
        'by_member': f"SELECT dim_a_id, sum(n), sum(amount_sum) FROM r_month WHERE {m} GROUP BY 1",
        'filtered_count': f"SELECT coalesce(sum(n), 0) FROM r_chan WHERE {m} AND channel_id = 9 AND region = 'DE'",
        'date_range_count': f"SELECT coalesce(sum(n), 0) FROM r_month WHERE {m} AND month >= '2025-06-01' AND month < '2026-01-01'",
        'count_by_kind': f"SELECT kind, sum(n) FROM r_kind WHERE {m} AND month >= '2025-01-01' GROUP BY 1",
    }


def ivm_classes():
    t = 'tenant_id = {t}'
    return {
        'sum_by_month': f"SELECT month::timestamptz m, sum(n), sum(amount_sum) FROM im_main WHERE {t} AND month >= '2023-01-01' GROUP BY month",
        'count_by_score': f"SELECT score, n FROM im_score WHERE {t}",
        'top_members': f"SELECT dim_a_id, sum(n) c FROM im_main WHERE {t} GROUP BY 1 ORDER BY c DESC, 1 LIMIT 20",
        'distinct_by_month': f"SELECT month::timestamptz m, count(DISTINCT dim_a_id) FROM im_main WHERE {t} AND month >= '2022-01-01' GROUP BY month",
        'by_member': f"SELECT dim_a_id, sum(n), sum(amount_sum) FROM im_main WHERE {t} GROUP BY 1",
        'filtered_count': f"SELECT coalesce(sum(n), 0) FROM im_chan WHERE {t} AND channel_id = 9 AND region = 'DE'",
        'date_range_count': f"SELECT coalesce(sum(n), 0) FROM im_main WHERE {t} AND month >= '2025-06-01' AND month < '2026-01-01'",
        'count_by_kind': f"SELECT kind, sum(n) FROM im_kind WHERE {t} AND month >= '2025-01-01' GROUP BY 1",
    }


def sql_text(name):
    with open(os.path.join(SQL, name)) as f:
        return f.read()


def rollup_ddl():
    ups = sql_text('rollup_upserts.sql')
    return sql_text('rollup.sql').replace('@UPSERTS_EVENTS@', ups.replace('@ROWS@', 'events')).replace('@UPSERTS_NEWROWS@', ups.replace('@ROWS@', 'newrows'))


def denorm_ddl(copy, create, indexes):
    return sql_text('chain.sql') + sql_text('denorm.sql').replace('@COPY@', copy).replace('@CREATE@', create).replace('@INDEXES@', indexes)


COPY_COLS = 'tenant_id int NOT NULL, id int NOT NULL, dim_a_id int NOT NULL, created_at date NOT NULL, kind text NOT NULL, channel_id int NOT NULL, region text NOT NULL, score int NOT NULL, amount int NOT NULL'
COVER = 'INCLUDE (dim_a_id, kind, score, amount)'
FLAT = denorm_ddl('events_t', f'CREATE TABLE events_t ({COPY_COLS});',
                  f"CREATE INDEX events_t_main ON events_t (tenant_id, created_at, id) {COVER};\nCREATE INDEX events_t_member ON events_t (tenant_id, dim_a_id);\n"
                  "CREATE INDEX events_t_filtered ON events_t (tenant_id) WHERE channel_id = 9 AND region = 'DE';")
PART = denorm_ddl('events_p', f"CREATE TABLE events_p ({COPY_COLS}) PARTITION BY LIST (tenant_id);\n"
                  "SELECT format('CREATE TABLE events_p_%s PARTITION OF events_p FOR VALUES IN (%s)', t, t) FROM generate_series(1, 32) t \\gexec",
                  f"CREATE INDEX events_p_main ON events_p (created_at, id) {COVER};\nCREATE INDEX events_p_member ON events_p (dim_a_id);\n"
                  "CREATE INDEX events_p_filtered ON events_p (dim_a_id) WHERE channel_id = 9 AND region = 'DE';")

# name: (needs pg_sift, DDL, classes, SQL for the bytes of what the arm adds)
ARMS = {
    'stock_default': (False, sql_text('stock_default.sql'), semi_classes, "SELECT pg_relation_size('events_dim')"),
    'semijoin': (False, sql_text('semijoin.sql'), lateral_classes, "SELECT pg_relation_size('events_cover') + pg_relation_size('events_filtered')"),
    'denorm_flat': (False, FLAT, lambda: copy_classes('events_t'),
                    "SELECT pg_total_relation_size('events_t') + pg_total_relation_size('pair_paths') + pg_total_relation_size('pairs') + pg_relation_size('events_dim')"),
    'denorm_part': (False, PART, lambda: copy_classes('events_p'),
                    "SELECT (SELECT sum(pg_total_relation_size(relid)) FROM pg_partition_tree('events_p')) + pg_total_relation_size('pair_paths') + pg_total_relation_size('pairs') + pg_relation_size('events_dim')"),
    'rollup': (False, rollup_ddl(), rollup_classes,
               "SELECT pg_total_relation_size('r_month') + pg_total_relation_size('r_score') + pg_total_relation_size('r_chan') + pg_total_relation_size('r_kind')"),
    'ivm': (False, sql_text('chain.sql') + sql_text('ivm.sql'), ivm_classes,
            "SELECT pg_total_relation_size('im_main') + pg_total_relation_size('im_score') + pg_total_relation_size('im_chan') + pg_total_relation_size('im_kind') "
            "+ pg_total_relation_size('pair_paths') + pg_total_relation_size('pairs') + pg_relation_size('events_dim')"),
    'sift_reuse': (True, sql_text('sift_reuse.sql'), semi_classes, "SELECT pg_relation_size('events_col')"),
    'sift_scope': (True, sql_text('sift_scope.sql'), semi_classes,
                   "SELECT pg_relation_size('events_col') + pg_relation_size('grants_by_member') + pg_relation_size('grants_by_group') + pg_relation_size('memberships_by_member')"),
}
ORDER = list(ARMS)


class Sess:
    def __init__(self, db, setup=''):
        self.p = subprocess.Popen(['psql', '-X', '-q', '-At', '-U', 'postgres', '-d', db], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=ENV)
        self.run("SET max_parallel_workers_per_gather = 0; SET jit = off; SET work_mem = '256MB'; SET maintenance_work_mem = '256MB'; "
                 "SET max_parallel_maintenance_workers = 0; SET client_min_messages = warning; SET statement_timeout = '900s'; " + setup)
        self.pid = int(self.run('SELECT pg_backend_pid()')[0])

    def run(self, sql):
        self.p.stdin.write(sql.rstrip().rstrip(';') + ";\n\\echo @@done\n")
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == '':
                raise RuntimeError('psql ended: ' + '\n'.join(out))
            if line.strip() == '@@done':
                break
            out.append(line.rstrip('\n'))
        if any(l.startswith('ERROR') for l in out):
            raise RuntimeError('\n'.join(out) + '\nin: ' + sql[:300])
        return out

    def script(self, text):
        """a script with psql meta-commands (\\gexec) and several statements; echoes nothing"""
        self.p.stdin.write(text + "\n\\echo @@done\n")
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == '':
                raise RuntimeError('psql ended: ' + '\n'.join(out))
            if line.strip() == '@@done':
                break
            out.append(line.rstrip('\n'))
        if any(l.startswith('ERROR') for l in out):
            raise RuntimeError('\n'.join(out))
        return out

    def counted(self, sql):
        """(instructions:u of this backend while it runs sql, output)"""
        for _ in range(5):
            perf = subprocess.Popen(['perf', 'stat', '-x,', '-e', 'instructions:u', '-p', str(self.pid)], stderr=subprocess.PIPE, text=True,
                                    preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL))
            time.sleep(0.2)
            t0 = time.time()
            out = self.run(sql)
            self.seconds = time.time() - t0
            perf.send_signal(signal.SIGINT)
            m = re.search(r'^(\d+),,instructions', perf.communicate()[1], re.M)
            if m:
                return int(m.group(1)), out
        raise RuntimeError('perf stat gave nothing')

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def explain(sess, sql):
    """(instructions, ms, hit, read, plan text) of one EXPLAIN (ANALYZE, BUFFERS, TIMING OFF) run"""
    ins, out = sess.counted('EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON) ' + sql)
    txt = '\n'.join(out)
    b = re.search(r'Buffers: shared(?: hit=(\d+))?(?: read=(\d+))?', txt)
    return ins, float(re.search(r'Execution Time: ([\d.]+)', txt).group(1)), int(b.group(1) or 0) if b else 0, int(b.group(2) or 0) if b else 0, txt


def measure(sess, sql, runs=3):
    sess.run(sql)
    res = sorted((explain(sess, sql) for _ in range(runs)), key=lambda r: r[0])
    warm = res[len(res) // 2]
    sess.run('SELECT pg_buffercache_evict_all()')
    time.sleep(0.2)
    return warm, explain(sess, sql)


def md5(sess, sql):
    return sess.run(f"SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM ({sql}) x")[0]


def plan_words(txt):
    """what the plan names of a sift read: the arm of the statement"""
    words = sorted(set(re.findall(r'(Scope Index|Scope Units|Scope Fallback: [^\n]*|Lookup Join|SiftAgg[A-Za-z ]*|Index Only Scan|Parallel Seq Scan|Seq Scan|Hash Semi Join|Nested Loop)', txt)))
    return '; '.join(w.strip() for w in words)[:160]


class Cluster:
    def __init__(self, a):
        self.a = a
        self.data = os.path.join(a.dir, 'data')
        self.env = dict(ENV, PGHOST=os.path.join(a.dir, 'sock'), PGPORT=str(a.port), PGUSER='postgres')
        os.environ.update(PGHOST=self.env['PGHOST'], PGPORT=self.env['PGPORT'], PGUSER='postgres')
        for k in ('PGSERVICE', 'PGSERVICEFILE', 'PGPASSWORD', 'PGHOSTADDR', 'PGDATABASE'):
            os.environ.pop(k, None)
        self.root = os.path.join(a.root, 'usr')

    def conf(self, preload):
        a = self.a
        lines = [f"port = {a.port}", f"unix_socket_directories = '{os.path.join(a.dir, 'sock')}'", "listen_addresses = ''", "shared_buffers = 512MB", "fsync = off",
                 "full_page_writes = off", "autovacuum = off", "jit = off", "max_wal_size = 1GB", "min_wal_size = 80MB", "checkpoint_timeout = 30min",
                 "max_parallel_workers_per_gather = 0", "max_parallel_maintenance_workers = 0", "work_mem = 256MB", "maintenance_work_mem = 256MB", "timezone = 'UTC'",
                 f"extension_control_path = '{self.root}/share/postgresql:$system'", f"dynamic_library_path = '{self.root}/lib/postgresql:$libdir'"]
        if preload:
            lines += ["shared_preload_libraries = 'pg_sift'", "sift.col_auto_merge = off"]
        with open(os.path.join(self.data, 'postgresql.auto.conf'), 'w') as f:
            f.write('\n'.join(lines) + '\n')

    def restart(self, preload):
        self.stop()
        self.conf(preload)
        subprocess.run(['pg_ctl', '-D', self.data, '-l', os.path.join(self.a.dir, 'log'), 'start', '-w', '-o', ''], check=True, env=self.env, stdout=subprocess.DEVNULL)

    def stop(self):
        if os.path.exists(os.path.join(self.data, 'postmaster.pid')):
            subprocess.run(['pg_ctl', '-D', self.data, '-m', 'fast', 'stop', '-w'], check=True, env=self.env, stdout=subprocess.DEVNULL)

    def psql(self, db, *args):
        return subprocess.run(['psql', '-X', '-q', '-At', '-d', db, *args], check=True, env=self.env, capture_output=True, text=True).stdout


def wal(sess):
    return int(sess.run("SELECT pg_current_wal_lsn() - '0/0'::pg_lsn")[0])


def timed(sess, sql):
    """(instructions, ms, WAL bytes) of one statement"""
    w0 = wal(sess)
    ins, _ = sess.counted(sql)
    return ins, sess.seconds * 1000, wal(sess) - w0


def write_costs(sess, sift):
    """the rows of the write table: {name: (instr, ms, wal)} per operation, averaged over its edits"""
    out = {}
    sess.run('CHECKPOINT')
    ins = ("INSERT INTO events SELECT id + 100000000, dim_a_id, date '2026-11-01' + (id % 10), kind, channel_id, region, score, amount FROM events "
           "WHERE id > (SELECT max(id) - 10000 FROM events)")
    out['insert 10k events'] = [timed(sess, ins)]
    if sift:
        out['promotion of 10k events'] = [timed(sess, "SELECT sift.col_promote('events_col')")]
    medians = sess.run("SELECT dim_a_id FROM (SELECT dim_a_id, count(*) n FROM events WHERE id < 100000000 GROUP BY 1) m "
                       f"WHERE n BETWEEN 350 AND 450 AND NOT EXISTS (SELECT 1 FROM tenant_pairs p WHERE p.tenant_id = {EDIT_TENANT} AND p.member = m.dim_a_id) ORDER BY 1 LIMIT 10")
    big = sess.run("SELECT dim_a_id FROM (SELECT dim_a_id, count(*) n FROM events WHERE id < 100000000 GROUP BY 1) m "
                   f"WHERE NOT EXISTS (SELECT 1 FROM tenant_pairs p WHERE p.tenant_id = {EDIT_TENANT} AND p.member = m.dim_a_id) ORDER BY n DESC LIMIT 1")
    for label, ms_ in (('median member', medians), ('largest member', big)):
        grants, revokes = [], []
        for m in ms_:
            grants.append(timed(sess, f"INSERT INTO grants (tenant_id, dim_a_id) VALUES ({EDIT_TENANT}, {m})"))
            revokes.append(timed(sess, f"UPDATE grants SET deleted_at = now() WHERE tenant_id = {EDIT_TENANT} AND dim_a_id = {m} AND deleted_at IS NULL"))
        out[f'pair grant, {label}'] = grants
        out[f'pair revoke, {label}'] = revokes
    return out


def pressure(kind):
    with open(f'/proc/pressure/{kind}') as f:
        return float(f.readline().split()[1].split('=')[1])


def quiet(limit=3600):
    """waits until the host is quiet (cpu and io `some` avg10 < 10%, 12 GB available): the host is shared by many runs"""
    t0 = time.time()
    while time.time() - t0 < limit:
        with open('/proc/meminfo') as f:
            avail = next(int(l.split()[1]) for l in f if l.startswith('MemAvailable'))
        if pressure('cpu') < 10 and pressure('io') < 10 and avail > 12 * 1024 * 1024:
            return
        time.sleep(15)
    raise SystemExit('host never quiet')


def run_arm(cl, a, name, oracle, outdir):
    quiet()
    sift, ddl, classes, size_sql = ARMS[name]
    cl.restart(sift)
    db = f'arm_{name}'
    cl.psql('postgres', '-c', f'DROP DATABASE IF EXISTS {db} WITH (FORCE)')
    cl.psql('postgres', '-c', 'CHECKPOINT')
    cl.psql('postgres', '-c', f'CREATE DATABASE {db} TEMPLATE {a.base} STRATEGY FILE_COPY')
    s = Sess(db)
    s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    s.run('CHECKPOINT')
    w0, t0 = wal(s), time.time()
    s.script(ddl)
    build_s, build_wal = time.time() - t0, wal(s) - w0
    size = int(s.run(size_sql)[0])
    sift_extra = ''
    if name == 'sift_scope':
        probe = s.run('EXPLAIN SELECT count(*) FROM events WHERE dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = 1)')
        if not any('Scope' in l for l in probe):
            t1, w1 = time.time(), wal(s)
            s.run('REINDEX INDEX events_col')
            build_s += time.time() - t1
            build_wal += wal(s) - w1
            size = int(s.run(size_sql)[0])
            sift_extra = 'REINDEX after the build'
    cs = classes()
    cells = []
    for t in a.tenants:
        quiet()
        for cname, tmpl in cs.items():
            sql = tmpl.format(t=t)
            warm, cold = measure(s, sql)
            digest = md5(s, sql)
            ref = oracle.get((t, cname)) if oracle is not None else digest
            cells.append((name, t, cname, warm, cold, 'ok' if digest == ref else 'DIFF', digest, plan_words(warm[4])))
            print(f'{name} t{t} {cname}: {warm[0] / 1e6:.2f} M instr {warm[1]:.1f} ms hit {warm[2]} read {cold[3]} {"ok" if digest == ref else "DIFF"}', file=sys.stderr, flush=True)
    parallel = []
    if name == 'semijoin':
        s.run('SET max_parallel_workers_per_gather = 4; SET parallel_setup_cost = 100; SET parallel_tuple_cost = 0.01')
        for t in a.tenants:
            for cname, tmpl in cs.items():
                sql = tmpl.format(t=t)
                s.run(sql)
                ms = statistics.median(explain(s, sql)[1] for _ in range(3))
                parallel.append((t, cname, ms))
        s.run('SET max_parallel_workers_per_gather = 0')
    writes = write_costs(s, sift)
    # reads after the writes: the pair edits are committed, the new events are in the write buffer / not promoted
    dirty = []
    for t in (a.tenants[0], a.tenants[-1]):
        for cname, tmpl in cs.items():
            sql = tmpl.format(t=t)
            warm, _ = measure(s, sql, 1)
            dirty.append((t, cname, warm[0], warm[1], md5(s, sql)))
    s.close()
    cl.psql('postgres', '-c', f'DROP DATABASE {db} WITH (FORCE)')
    return dict(name=name, build_s=build_s, build_wal=build_wal, size=size, note=sift_extra, cells=cells, parallel=parallel, writes=writes, dirty=dirty)


def save(outdir, r):
    n = r['name']
    with open(os.path.join(outdir, f'{n}.cells.tsv'), 'w') as f:
        f.write('arm\ttenant\tclass\twarm_minstr\twarm_ms\twarm_hit\twarm_read\tcold_minstr\tcold_ms\tcold_hit\tcold_read\tmd5\tmd5_ok\tplan\n')
        for name, t, c, w, k, ok, dig, plan in r['cells']:
            f.write('\t'.join(map(str, [name, t, c, '%.3f' % (w[0] / 1e6), w[1], w[2], w[3], '%.3f' % (k[0] / 1e6), k[1], k[2], k[3], dig, ok, plan])) + '\n')
    with open(os.path.join(outdir, f'{n}.build.tsv'), 'w') as f:
        f.write('arm\tbuild_s\tbuild_wal_bytes\tadded_bytes\tnote\n')
        f.write(f"{n}\t{r['build_s']:.2f}\t{r['build_wal']}\t{r['size']}\t{r['note']}\n")
    with open(os.path.join(outdir, f'{n}.writes.tsv'), 'w') as f:
        f.write('arm\toperation\tstatements\tminstr\tms\twal_bytes\n')
        for op, v in r['writes'].items():
            f.write(f"{n}\t{op}\t{len(v)}\t{statistics.mean(x[0] for x in v) / 1e6:.3f}\t{statistics.mean(x[1] for x in v):.2f}\t{statistics.mean(x[2] for x in v):.0f}\n")
    with open(os.path.join(outdir, f'{n}.dirty.tsv'), 'w') as f:
        f.write('arm\ttenant\tclass\twarm_minstr\twarm_ms\tmd5\n')
        for t, c, ins, ms, dig in r['dirty']:
            f.write(f'{n}\t{t}\t{c}\t{ins / 1e6:.3f}\t{ms}\t{dig}\n')
    if r['parallel']:
        with open(os.path.join(outdir, f'{n}.parallel.tsv'), 'w') as f:
            f.write('arm\ttenant\tclass\twarm_ms_4_workers\n')
            for t, c, ms in r['parallel']:
                f.write(f'{n}\t{t}\t{c}\t{ms}\n')


def load_oracle(outdir):
    path = os.path.join(outdir, 'stock_default.cells.tsv')
    if not os.path.exists(path):
        return None
    with open(path) as f:
        rows = [l.rstrip('\n').split('\t') for l in f][1:]
    return {(int(r[1]), r[2]): r[11] for r in rows}


def cmd_run(a):
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(os.path.join(a.dir, 'sock'), exist_ok=True)
    cl = Cluster(a)
    if not os.path.exists(os.path.join(cl.data, 'PG_VERSION')):
        subprocess.run(['initdb', '-D', cl.data, '-U', 'postgres', '--no-sync', '-E', 'UTF8', '--locale=C'], check=True, env=cl.env, stdout=subprocess.DEVNULL)
    cl.restart(False)
    have = cl.psql('postgres', '-c', f"SELECT 1 FROM pg_database WHERE datname = '{a.base}'").strip()
    if not have:
        cl.psql('postgres', '-c', f'CREATE DATABASE {a.base}')
        subprocess.run(['psql', '-X', '-q', '-d', a.base, '-v', f'facts={a.facts}', '-v', f'member_total={a.member_total}', '-f', os.path.join(SQL, 'base.sql')],
                       check=True, env=cl.env, stdout=open(os.path.join(a.out, 'fixture.txt'), 'w'))
        cl.psql('postgres', '-c', 'CHECKPOINT')
    for name in a.arms.split(','):
        r = run_arm(cl, a, name, load_oracle(a.out) if name != 'stock_default' else None, a.out)
        save(a.out, r)
    cl.stop()


def geomean(xs):
    xs = [max(x, 1e-6) for x in xs]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float('nan')


def cmd_report(a):
    def rows(suffix):
        out = []
        for name in ORDER:
            p = os.path.join(a.dir_, f'{name}.{suffix}.tsv')
            if os.path.exists(p):
                with open(p) as f:
                    out += list(csv.DictReader(f, delimiter='\t'))
        return out
    cells = rows('cells')
    arms = [n for n in ORDER if any(r['arm'] == n for r in cells)]
    classes = list(event_classes('events', ''))
    tenants = sorted({int(r['tenant']) for r in cells})
    cell = {(r['arm'], int(r['tenant']), r['class']): r for r in cells}
    print(f"md5: {sum(r['md5_ok'] == 'ok' for r in cells)} of {len(cells)} cells equal the oracle (stock_default, enable_agg-free plain statement); different: {sum(r['md5_ok'] != 'ok' for r in cells)}\n")
    print('### Reads, warm: M instr / ms / shared blocks hit; cold: blocks read\n')
    for t in tenants:
        print(f'**tenant {t}**\n\n| class | ' + ' | '.join(arms) + ' |\n|---|' + '---|' * len(arms))
        for c in classes:
            cols = []
            for n in arms:
                r = cell.get((n, t, c))
                cols.append('n/a' if r is None else f"{float(r['warm_minstr']):.2f} / {float(r['warm_ms']):.2f} / {r['warm_hit']}; {r['cold_read']}")
            print(f'| {c} | ' + ' | '.join(cols) + ' |')
        print()
    common = [c for c in classes if all((n, tenants[0], c) in cell for n in arms)]
    for what, col, fmt in (('warm ms', 'warm_ms', '{:.2f}'), ('warm M instr', 'warm_minstr', '{:.2f}'), ('cold blocks read', 'cold_read', '{:.0f}')):
        print(f'### Geometric mean over the {len(common)} classes every arm answers ({what})\n\n| tenant | ' + ' | '.join(arms) + ' |\n|---|' + '---|' * len(arms))
        for t in tenants:
            print(f'| {t} | ' + ' | '.join(fmt.format(geomean([float(cell[(n, t, c)][col]) for c in common])) for n in arms) + ' |')
        print()
    base = 'sift_scope'
    print(f'### Cells where another arm is within 10% of {base} in warm ms, or faster (absolute ms; the arm named is the fastest such)\n\n| tenant | class | {base} ms | arm | ms |\n|---|---|---|---|---|')
    for t in tenants:
        for c in classes:
            ref = cell.get((base, t, c))
            if ref is None:
                continue
            best = min((n for n in arms if n != base and (n, t, c) in cell), key=lambda n: float(cell[(n, t, c)]['warm_ms']), default=None)
            if best and float(cell[(best, t, c)]['warm_ms']) <= 1.1 * float(ref['warm_ms']):
                print(f"| {t} | {c} | {float(ref['warm_ms']):.2f} | {best} | {float(cell[(best, t, c)]['warm_ms']):.2f} |")
    print('\n### Build and size\n\n| arm | build s | build WAL MB | added MB | note |\n|---|---|---|---|---|')
    for r in rows('build'):
        print(f"| {r['arm']} | {float(r['build_s']):.1f} | {int(r['build_wal_bytes']) / 1e6:.1f} | {int(r['added_bytes']) / 1e6:.1f} | {r['note']} |")
    print('\n### Writes (mean of the statements)\n\n| arm | operation | n | M instr | ms | WAL bytes |\n|---|---|---|---|---|---|')
    for r in rows('writes'):
        print(f"| {r['arm']} | {r['operation']} | {r['statements']} | {float(r['minstr']):.2f} | {float(r['ms']):.1f} | {float(r['wal_bytes']):.0f} |")
    dirty = rows('dirty')
    oracle = {(r['tenant'], r['class']): r['md5'] for r in dirty if r['arm'] == 'stock_default'}
    print(f"\n### Reads after the writes (10,000 new events, committed pair edits): md5 differing from stock_default: {sum(r['md5'] != oracle.get((r['tenant'], r['class'])) for r in dirty)} of {len(dirty)}\n")
    print('| arm | tenant | geomean M instr | geomean ms |\n|---|---|---|---|')
    for n in arms:
        for t in sorted({r['tenant'] for r in dirty}):
            rs = [r for r in dirty if r['arm'] == n and r['tenant'] == t and r['class'] in common]
            if rs:
                print(f"| {n} | {t} | {geomean([float(r['warm_minstr']) for r in rs]):.2f} | {geomean([float(r['warm_ms']) for r in rs]):.2f} |")
    par = rows('parallel')
    if par:
        print('\n### semijoin with 4 parallel workers (warm ms, geometric mean over the classes)\n\n| tenant | serial | 4 workers |\n|---|---|---|')
        for t in tenants:
            ser = geomean([float(cell[('semijoin', t, c)]['warm_ms']) for c in classes])
            print(f"| {t} | {ser:.2f} | {geomean([float(r['warm_ms_4_workers']) for r in par if int(r['tenant']) == t]):.2f} |")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--root', required=True, help='DESTDIR prefix that holds usr/ with pg_sift (and pg_ivm) installed')
    r.add_argument('--dir', required=True, help='scratch directory (tmpfs): data directory, socket, log')
    r.add_argument('--port', type=int, required=True)
    r.add_argument('--out', required=True)
    r.add_argument('--arms', default=','.join(ORDER))
    r.add_argument('--facts', type=int, default=2400000)
    r.add_argument('--member-total', type=int, default=2648)
    r.add_argument('--base', default='base', help='name of the fixture database (built once, the template of every arm)')
    r.add_argument('--tenants', default=','.join(map(str, TENANTS)), type=lambda v: [int(x) for x in v.split(',')])
    p = sub.add_parser('report')
    p.add_argument('dir_')
    a = ap.parse_args()
    cmd_run(a) if a.cmd == 'run' else cmd_report(a)


if __name__ == '__main__':
    main()
