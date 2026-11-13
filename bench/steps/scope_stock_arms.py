#!/usr/bin/env python3
"""The scope index against tuned stock PostgreSQL on a real-shaped fixture: nine read classes, eight tenants, one arm at a time.

    python3 -I bench/steps/scope_stock_arms.py run --root PREFIX [--root PREFIX2] --dir SCRATCH --port N --out RAWDIR [--size quick|standard] [--arms a,b] [--facts N] [--tenants 1,4,8]
    python3 -I bench/steps/scope_stock_arms.py collect RAWDIR --out docs/results/scope-stock.tsv     # the one results file, in the format of steps/extras.py
    python3 -I bench/steps/scope_stock_arms.py report [RESULTS]                                      # every table of the results file (steps/scope_stock_tables.py)

`bench/bench scope-stock` runs the three (it builds pg_sift and pg_ivm, makes the cluster, and with --docs rewrites the generated blocks of README.md and docs/COL-SCOPE.md).
The classes are bench/sql/scope_stock/queries/SLUG.sql, one file each (`--@ events` is the statement over a table of events and its predicate, `--@ lateral`, `--@ rollup` and `--@ ivm`
the forms of the arms that read another table; a class with no section for an arm is not answerable by it).

The fixture (bench/sql/scope_stock/base.sql), size `standard`: 2.4 million events of 2,648 member keys (log-normal counts, ordered by time), 32 tenants that reach them directly or through groups
(3,600 distinct pairs, 1.45 tenants a member), the eight measured tenants hold 49, 20, 17, 6, 6, 4, 1 and 0.1% of the events. Size `quick` is 240,000 events and the tenants 1, 4 and 8 (the large,
a medium and the small one) on the arms stock_default, semijoin, denorm_flat, ivm and sift_scope: minutes, not an hour. Each arm is built in a copy of the fixture database and
dropped after its reads and writes; the fixture and the arms are on one cluster (a tmpfs directory). Arms:
  stock_default  the b-tree on the member the application has, the semi join as written (the oracle of the md5 of every cell)
  semijoin       covering (member, time) b-tree, partial b-tree for the filter class, per-member LATERAL probe for latest-N; nothing maintained
  denorm_flat    a copy of the events by (tenant, event), b-tree (tenant, time), kept by triggers (events, pair edits)
  denorm_part    the same copy as a list partition by tenant
  rollup         rollups by member kept by a statement trigger; the classes read them through the semi join (latest-N: not answerable)
  ivm            pg_ivm incremental views of events joined to the maintained pair table (latest-N: not answerable); skipped, with a notice, when pg_ivm is not installed under a --root
  sift_reuse     the sift_col entity index, the pairs read through the member key
  sift_scope     the same index with the scope of two paths
The stock arms run on a server without pg_sift loaded; the sift arms on the same data directory restarted with it. Every cell: warm (median of 3 of `instructions:u` of the backend, EXPLAIN
ANALYZE time and shared blocks hit/read) and cold (shared buffers emptied: one run), the md5 of the rows against the oracle. Writes: 10,000 inserted events (statement), a pair edit
(grant a member to a tenant, revoke it) for 10 member keys of ~400 events and for the largest member; the sift arm adds the promotion of the write buffer. Size: bytes of what the arm adds.
"""
import argparse
import datetime
import os
import re
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
SQL = os.path.join(HERE, '..', 'sql', 'scope_stock')
TENANTS = (1, 2, 3, 4, 5, 6, 7, 8)
EDIT_TENANT = 6    # the tenant whose pairs the edits change; none of the measured reads is on it for its member keys
ENV = dict(os.environ, LC_ALL='C')
SIZES = {'standard': dict(facts=2400000, tenants=TENANTS, arms=None),
         'quick': dict(facts=240000, tenants=(1, 4, 8), arms=('stock_default', 'semijoin', 'denorm_flat', 'ivm', 'sift_scope'))}

SEMI = 'dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = {t})'


def sections(text):
    """`--@ name` lines cut a file into sections (the text before the first one is the header)"""
    out, cur = {'': []}, ''
    for line in text.split('\n'):
        m = re.match(r'--@ (\w+)$', line)
        if m:
            cur = m.group(1)
            out[cur] = []
        else:
            out[cur].append(line)
    return {k: '\n'.join(v).strip() for k, v in out.items()}


def load_queries():
    """{class slug: {section: statement}} of bench/sql/scope_stock/queries, in the `-- order` of the headers"""
    d = os.path.join(SQL, 'queries')
    out = {}
    for f in sorted(os.listdir(d)):
        text = sql_text(os.path.join('queries', f))
        out[f[:-4]] = (int(re.search(r'^-- order: (\d+)', text, re.M).group(1)), sections(text))
    return {k: v[1] for k, v in sorted(out.items(), key=lambda kv: kv[1][0])}


def classes_of(section, src='events', pred=SEMI, over=None):
    """{class: statement with {t}} of the classes that have the section (`over`: the section that replaces it for a class that has it)"""
    out = {}
    for name, q in QUERIES.items():
        text = q.get(over) if over in q else q.get(section)
        if text:
            out[name] = text.replace('{src}', src).replace('{pred}', pred)
    return out


def semi_classes():
    return classes_of('events')


def lateral_classes():
    return classes_of('events', over='lateral')


def copy_classes(table):
    return classes_of('events', src=table, pred='tenant_id = {t}')


def rollup_classes():
    return classes_of('rollup')


def ivm_classes():
    return classes_of('ivm')


def sql_text(name):
    with open(os.path.join(SQL, name)) as f:
        return f.read()


QUERIES = load_queries()


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
        ENV.update(PGHOST=self.env['PGHOST'], PGPORT=self.env['PGPORT'])
        for k in ('PGSERVICE', 'PGSERVICEFILE', 'PGPASSWORD', 'PGHOSTADDR', 'PGDATABASE'):
            os.environ.pop(k, None)
        self.roots = [os.path.join(r, 'usr') for r in a.root]

    def has_extension(self, name):
        return any(os.path.exists(os.path.join(r, 'share', 'postgresql', 'extension', name + '.control')) for r in self.roots)

    def conf(self, preload):
        a = self.a
        lines = [f"port = {a.port}", f"unix_socket_directories = '{os.path.join(a.dir, 'sock')}'", "listen_addresses = ''", "shared_buffers = 512MB", "fsync = off",
                 "full_page_writes = off", "autovacuum = off", "jit = off", "max_wal_size = 1GB", "min_wal_size = 80MB", "checkpoint_timeout = 30min",
                 "max_parallel_workers_per_gather = 0", "max_parallel_maintenance_workers = 0", "work_mem = 256MB", "maintenance_work_mem = 256MB", "timezone = 'UTC'",
                 "extension_control_path = '%s:$system'" % ':'.join(f'{r}/share/postgresql' for r in self.roots),
                 "dynamic_library_path = '%s:$libdir'" % ':'.join(f'{r}/lib/postgresql' for r in self.roots)]
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
    return {(int(r['tenant']), r['class']): r['md5'] for r in read_tsv(path)}


def read_tsv(path):
    with open(path) as f:
        head, *rows = [l.rstrip('\n').split('\t') for l in f]
    return [dict(zip(head, r)) for r in rows]


def fixture_stats(cl, a):
    """what the fixture is, for the tables: the totals, and for each tenant its member keys and its share of the events"""
    totals = cl.psql(a.base, '-F', '\t', '-c', "SELECT (SELECT count(*) FROM events), (SELECT count(DISTINCT dim_a_id) FROM events), (SELECT count(*) FROM tenant_pairs), "
                     "(SELECT count(DISTINCT tenant_id) FROM tenant_pairs), current_setting('server_version')").strip().split('\t')
    per = cl.psql(a.base, '-F', '\t', '-c', "SELECT p.tenant_id, count(*), sum(c.n), round(100.0 * sum(c.n) / (SELECT count(*) FROM events), 2) "
                  "FROM tenant_pairs p JOIN (SELECT dim_a_id, count(*) n FROM events GROUP BY 1) c ON c.dim_a_id = p.member GROUP BY 1 ORDER BY 1")
    with open(os.path.join(a.out, 'fixture.tsv'), 'w') as f:
        f.write('tenant\tmember_total\tevents\tevents_pct\n' + per)
    return dict(zip(('facts', 'member_total', 'pairs', 'tenant_total', 'postgresql'), totals))


def versions(cl, a):
    """the build of pg_sift as `control version (build id)` and the version of pg_ivm the control files of the roots give"""
    v = lambda name: cl.psql('postgres', '-c', f"SELECT default_version FROM pg_available_extensions WHERE name = '{name}'").strip()
    return dict(pg_sift=f'{v("pg_sift")} ({a.build_id})', pg_ivm=v('pg_ivm') or 'not installed')


def cmd_run(a):
    size = SIZES[a.size]
    a.facts, a.tenants = a.facts or size['facts'], a.tenants or list(size['tenants'])
    arms = (a.arms or ','.join(size['arms'] or ORDER)).split(',')
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(os.path.join(a.dir, 'sock'), exist_ok=True)
    cl = Cluster(a)
    skipped = []
    if 'ivm' in arms and not cl.has_extension('pg_ivm'):
        print('NOTICE: the arm ivm is skipped: pg_ivm is not installed under any --root (bench/steps/fetch.sh pg_ivm builds the pinned release)', file=sys.stderr, flush=True)
        arms.remove('ivm')
        skipped.append('ivm (pg_ivm not installed)')
    if 'stock_default' not in arms and load_oracle(a.out) is None:
        raise SystemExit('the arms need stock_default (the oracle of every md5): add it to --arms or run it first into the same --out')
    if not os.path.exists(os.path.join(cl.data, 'PG_VERSION')):
        subprocess.run(['initdb', '-D', cl.data, '-U', 'postgres', '--no-sync', '-E', 'UTF8', '--locale=C'], check=True, env=cl.env, stdout=subprocess.DEVNULL)
    cl.restart(False)
    have = cl.psql('postgres', '-c', f"SELECT 1 FROM pg_database WHERE datname = '{a.base}'").strip()
    if not have:
        cl.psql('postgres', '-c', f'CREATE DATABASE {a.base}')
        subprocess.run(['psql', '-X', '-q', '-d', a.base, '-v', f'facts={a.facts}', '-v', f'member_total={a.member_total}', '-f', os.path.join(SQL, 'base.sql')],
                       check=True, env=cl.env, stdout=open(os.path.join(a.out, 'fixture.txt'), 'w'))
        cl.psql('postgres', '-c', 'CHECKPOINT')
    meta = dict(fixture_stats(cl, a), **versions(cl, a), size=a.size, date=datetime.date.today().isoformat(), arms=','.join(arms), skipped='; '.join(skipped) or 'none',
                tenants=','.join(map(str, a.tenants)), classes=','.join(QUERIES))
    for name in arms:
        r = run_arm(cl, a, name, load_oracle(a.out) if name != 'stock_default' else None, a.out)
        save(a.out, r)
    cl.stop()
    with open(os.path.join(a.out, 'run.meta'), 'w') as f:
        f.write(''.join(f'{k}\t{v}\n' for k, v in meta.items()))


def cpu_model():
    with open('/proc/cpuinfo') as f:
        return next((l.split(':', 1)[1].strip() for l in f if l.startswith('model name')), 'unknown cpu')


def cmd_collect(a):
    """the one results file of a run: every cell, build, write and read-after-write of every arm, in the format of steps/extras.py"""
    import extras
    raw = a.raw
    with open(os.path.join(raw, 'run.meta')) as f:
        meta = dict(l.rstrip('\n').split('\t', 1) for l in f)
    arms = meta['arms'].split(',')
    rows = []

    def add(case, arm, metric, value, n='', note=''):
        rows.append(dict(version=meta['pg_sift'], case=case, arm=arm, metric=metric, value=value, n=n, load=load, note=note))

    load = extras.host_load()
    for r in read_tsv(os.path.join(raw, 'fixture.tsv')):
        for m in ('member_total', 'events', 'events_pct'):
            add('t' + r['tenant'], 'fixture', m, r[m])
    stock_dirty = {(r['tenant'], r['class']): r['md5'] for r in read_tsv(os.path.join(raw, 'stock_default.dirty.tsv'))}
    for arm in arms:
        for r in read_tsv(os.path.join(raw, f'{arm}.cells.tsv')):
            case = 't%s:%s' % (r['tenant'], r['class'])
            add(case, arm, 'warm_ms', r['warm_ms'], note=r['plan'])
            for m in ('warm_minstr', 'warm_hit', 'cold_ms', 'cold_minstr', 'cold_hit', 'cold_read'):
                add(case, arm, m, r[m])
            add(case, arm, 'md5_equal', int(r['md5_ok'] == 'ok'))
        b = read_tsv(os.path.join(raw, f'{arm}.build.tsv'))[0]
        for m in ('build_s', 'build_wal_bytes', 'added_bytes'):
            add('build', arm, m, b[m], note=b['note'])
        for r in read_tsv(os.path.join(raw, f'{arm}.writes.tsv')):
            for m, c in (('ms', 'ms'), ('minstr', 'minstr'), ('wal_bytes', 'wal_bytes')):
                add('write:' + r['operation'], arm, m, r[c], n=r['statements'])
        for r in read_tsv(os.path.join(raw, f'{arm}.dirty.tsv')):
            case = 'dirty:t%s:%s' % (r['tenant'], r['class'])
            add(case, arm, 'warm_ms', r['warm_ms'])
            add(case, arm, 'warm_minstr', r['warm_minstr'])
            add(case, arm, 'md5_equal', int(r['md5'] == stock_dirty[(r['tenant'], r['class'])]))
        par = os.path.join(raw, f'{arm}.parallel.tsv')
        if os.path.exists(par):
            for r in read_tsv(par):
                add('t%s:%s' % (r['tenant'], r['class']), arm, 'parallel_ms', r['warm_ms_4_workers'])
    head = dict(date=meta['date'], cpu=cpu_model(), postgresql='PostgreSQL ' + meta['postgresql'], pg_sift=meta['pg_sift'], pg_ivm=meta['pg_ivm'],
                size=meta['size'], facts=meta['facts'], member_total=meta['member_total'], pairs=meta['pairs'], tenant_total=meta['tenant_total'], tenants=meta['tenants'],
                classes=meta['classes'], arms=meta['arms'], skipped=meta['skipped'],
                method='warm: median of 3 of instructions:u of the backend, EXPLAIN (ANALYZE, BUFFERS, TIMING OFF); cold: shared buffers emptied, one run; one backend, no parallel workers')
    extras.write(a.out, 'The scope index against tuned stock PostgreSQL (bench/bench scope-stock)', head, rows)


def cmd_report(a):
    import scope_stock_tables
    print('\n'.join(scope_stock_tables.report(scope_stock_tables.Results(a.results))))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--root', action='append', required=True, help='DESTDIR prefix that holds usr/ with pg_sift (and pg_ivm) installed; repeat for pg_ivm in another prefix')
    r.add_argument('--dir', required=True, help='scratch directory (tmpfs): data directory, socket, log')
    r.add_argument('--port', type=int, required=True)
    r.add_argument('--out', required=True, help='directory of the raw per-arm tables')
    r.add_argument('--size', choices=SIZES, default='standard')
    r.add_argument('--build-id', default='unknown', help='the commit the installed pg_sift was built from, for the results file')
    r.add_argument('--arms', help='default: every arm (quick: stock_default, semijoin, denorm_flat, ivm, sift_scope)')
    r.add_argument('--facts', type=int, help='events (default: those of --size)')
    r.add_argument('--member-total', type=int, default=2648)
    r.add_argument('--base', default='base', help='name of the fixture database (built once, the template of every arm)')
    r.add_argument('--tenants', type=lambda v: [int(x) for x in v.split(',')], help='default: those of --size')
    c = sub.add_parser('collect')
    c.add_argument('raw')
    c.add_argument('--out', required=True)
    p = sub.add_parser('report')
    p.add_argument('results', nargs='?', default=os.path.join(HERE, '..', '..', 'docs', 'results', 'scope-stock.tsv'))
    a = ap.parse_args()
    {'run': cmd_run, 'collect': cmd_collect, 'report': cmd_report}[a.cmd](a)


if __name__ == '__main__':
    main()
