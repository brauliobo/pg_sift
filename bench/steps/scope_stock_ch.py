"""The ClickHouse arms of scope-stock (steps/scope_stock_arms.py): the same events, tenants and nine read classes on a ClickHouse server of its own.

  ch_join  events and the pair table as two MergeTree tables; every class reads events INNER JOIN the pairs of the tenant
  ch_proj  the same tables; the pairs enter as an IN set because a PROJECTION cannot hold a JOIN: PROJECTIONs pre-aggregate the events by member (month, score, channel and
           region, kind), so a class is answered from one only when its statement has the shape of one (the plan column names the projections used)
  ch_mv    opt-in (--arms): the same tables and one materialized view of (member, month) counts and sums; five classes read it, the others read events; it shows what a view costs to write

The server is `clickhouse-server` of the PATH, else the pinned image of bench/versions.env (STORE_CH_IMAGE) on the host network; one thread per query (`max_threads=1`, the PostgreSQL arms
run no parallel workers). Instructions are ClickHouse's own `PerfInstructions` of the query's threads (`perf stat -p` of the server misses its pool threads), milliseconds its `elapsed_ns`; ClickHouse has no shared buffers and no WAL:
hit/read are the blocks of 8 KiB the query read, WAL bytes the bytes it wrote to parts. The events are ClickHouse's own copy of the fixture, so `added_bytes` is the whole table.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

import clickbench_engines as engines

THREADS = 1
MONTH = "concat(toString(toStartOfMonth(created_at)), ' 00:00:00+00')"
IN_SET = 'dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = {t})'
JOIN_SRC = '(SELECT e.* FROM events AS e INNER JOIN (SELECT member FROM tenant_pairs WHERE tenant_id = {t}) AS p ON e.dim_a_id = p.member) AS j'
EVENTS = ('CREATE TABLE events (id Int32, dim_a_id Int32, created_at Date, kind LowCardinality(String), channel_id Int32, region LowCardinality(String), '
          'score Int32, amount Int32) ENGINE = MergeTree ORDER BY (dim_a_id, created_at, id)')
PAIRS = 'CREATE TABLE tenant_pairs (tenant_id Int32, member Int32) ENGINE = MergeTree ORDER BY (tenant_id, member)'
PROJECTIONS = {
    'p_month': 'SELECT dim_a_id, toStartOfMonth(created_at) AS month, count() AS n, sum(amount) AS amount_sum GROUP BY dim_a_id, month',
    'p_score': 'SELECT dim_a_id, score, count() AS n GROUP BY dim_a_id, score',
    'p_chan': 'SELECT dim_a_id, channel_id, region, count() AS n GROUP BY dim_a_id, channel_id, region',
    'p_kind': 'SELECT dim_a_id, toStartOfMonth(created_at) AS month, kind, count() AS n GROUP BY dim_a_id, month, kind',
}
MV_TABLE = ('CREATE TABLE ev_mm (dim_a_id Int32, month Date, n UInt64, amount_sum Int64) ENGINE = SummingMergeTree ORDER BY (dim_a_id, month)',
            'CREATE MATERIALIZED VIEW ev_mm_mv TO ev_mm AS SELECT dim_a_id, toStartOfMonth(created_at) AS month, count() AS n, sum(amount) AS amount_sum FROM events GROUP BY dim_a_id, month')
MV_CLASSES = {
    'by_member': 'SELECT dim_a_id, sum(n), sum(amount_sum) FROM ev_mm WHERE {pred} GROUP BY 1',
    'date_range_count': "SELECT sum(n) FROM ev_mm WHERE {pred} AND month >= '2025-06-01' AND month < '2026-01-01'",
    'sum_by_month': "SELECT concat(toString(month), ' 00:00:00+00') m, sum(n), sum(amount_sum) FROM ev_mm WHERE {pred} AND month >= '2023-01-01' GROUP BY month",
    'distinct_members_by_month': "SELECT concat(toString(month), ' 00:00:00+00') m, count(DISTINCT dim_a_id) FROM ev_mm WHERE {pred} AND month >= '2022-01-01' GROUP BY month",
    'top_members': 'SELECT dim_a_id, sum(n) c FROM ev_mm WHERE {pred} GROUP BY 1 ORDER BY c DESC, 1 LIMIT 20',
}
PAIR_SQL = ("SELECT DISTINCT tenant_id, dim_a_id FROM (SELECT tenant_id, dim_a_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL UNION ALL "
            "SELECT g.tenant_id, m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL) x ORDER BY 1, 2")
EVENT_SQL = 'SELECT id, dim_a_id, created_at, kind, channel_id, region, score, amount FROM events ORDER BY id'
INSERT_10K = ("INSERT INTO events SELECT id + 100000000, dim_a_id, toDate('2026-11-01') + (id % 10), kind, channel_id, region, score, amount FROM events "
              "WHERE id > (SELECT max(id) - 10000 FROM events)")
EDIT_TENANT = 6
SPECS = {
    'ch_join': dict(create=[EVENTS, PAIRS], after=[], tables=('events', 'tenant_pairs'), src=JOIN_SRC, pred='true', mv=False,
                    note='own copy of the events (ORDER BY member, date), the pairs joined in; one thread'),
    'ch_proj': dict(create=[EVENTS, PAIRS] + [f'ALTER TABLE events ADD PROJECTION {n} ({q})' for n, q in PROJECTIONS.items()], tables=('events', 'tenant_pairs'),
                    after=[], src='events', pred=IN_SET, mv=False,
                    note='own copy of the events with 4 PROJECTIONs by member, the pairs as an IN set (a PROJECTION cannot hold a JOIN); one thread'),
    'ch_mv': dict(create=[EVENTS, PAIRS, *MV_TABLE], after=[], tables=('events', 'tenant_pairs', 'ev_mm'), src='events', pred=IN_SET, mv=True,
                  note='own copy of the events and a materialized view of (member, month); 5 classes read the view; one thread'),
}
OPTIONAL = ('ch_mv',)


def ddl(name):
    return '\n'.join(SPECS[name]['create'] + SPECS[name]['after'])


def size_sql(name):
    tables = ', '.join(f"'{t}'" for t in SPECS[name]['tables'])
    return f"SELECT sum(bytes_on_disk) FROM system.parts WHERE database = 'default' AND active AND table IN ({tables})"


def statements(name, classes_of):
    """{class: statement with {t}} of an arm: the events statements of the class files with this arm's source and predicate (the view's classes read the view)"""
    spec = SPECS[name]
    out = {c: s.replace('date_trunc(\'month\', created_at)', MONTH) for c, s in classes_of('events', src=spec['src'], pred=spec['pred']).items()}
    if spec['mv']:
        out.update({c: s.replace('{pred}', spec['pred']) for c, s in MV_CLASSES.items()})
    return out


def server_binary():
    return shutil.which('clickhouse-server') or shutil.which('clickhouse')


def version():
    """the release of the server these arms would run on (a cache key); '' when there is none"""
    if binary := server_binary():
        out = subprocess.run([binary, 'server', '--version'] if os.path.basename(binary) == 'clickhouse' else [binary, '--version'], capture_output=True, text=True).stdout
        return re.search(r'version (\S+)', out).group(1)
    return os.environ.get('STORE_CH_IMAGE', '') if shutil.which('docker') else ''


def pg_text(value):
    """the text of one column inside a PostgreSQL record: quoted when it holds a space, a comma, a parenthesis, a quote or a backslash, or is empty"""
    return f'"{value}"' if value == '' or re.search(r'[\s,()"\\]', value) else value


def rows_md5(tsv):
    """the md5 scope_stock_arms.md5 takes in PostgreSQL: rows as `x::text` of a record, sorted bytewise, joined by `|`"""
    rows = ['(' + ','.join(pg_text('' if v == '\\N' else v) for v in line.split('\t')) + ')' for line in tsv.splitlines()]
    return hashlib.md5('|'.join(sorted(rows)).encode()).hexdigest()


CONFIG = """<clickhouse>
<logger><level>warning</level><log>{dir}/server.log</log><errorlog>{dir}/error.log</errorlog></logger>
<http_port>{port}</http_port><listen_host>127.0.0.1</listen_host><path>{dir}/data/</path><tmp_path>{dir}/tmp/</tmp_path><user_files_path>{dir}/files/</user_files_path>
<format_schema_path>{dir}/schemas/</format_schema_path><mlock_executable>false</mlock_executable>
<users><default><password></password><profile>default</profile><quota>default</quota><networks><ip>127.0.0.1</ip></networks><access_management>1</access_management></default></users>
<profiles><default/></profiles><quotas><default/></quotas>
<query_log><database>system</database><table>query_log</table><flush_interval_milliseconds>1000</flush_interval_milliseconds></query_log>
</clickhouse>
"""


class Server:
    """one ClickHouse server on 127.0.0.1:port, its files under dir; native when clickhouse-server is on the PATH, else the pinned image on the host network"""

    def __init__(self, dir, port):
        self.dir, self.port, self.proc, self.container = os.path.abspath(dir), port, None, None
        os.makedirs(self.dir, exist_ok=True)
        with open(os.path.join(self.dir, 'config.xml'), 'w') as f:
            f.write(CONFIG.format(dir=self.dir, port=port))
        self.arm = engines.ChArm('ch', f'http://127.0.0.1:{port}', table='events', threads=THREADS, os_cache=engines.OsCache(paths=[os.path.join(self.dir, 'data')]))

    def start(self):
        config = os.path.join(self.dir, 'config.xml')
        if binary := server_binary():
            cmd = [binary, 'server', f'--config-file={config}'] if os.path.basename(binary) == 'clickhouse' else [binary, f'--config-file={config}']
            self.proc = subprocess.Popen(cmd, cwd=self.dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            with open(os.path.join(self.dir, 'pid'), 'w') as f:
                f.write(str(self.proc.pid))
        else:
            self.container = f'sift-scope-ch-{self.port}'
            subprocess.run(['docker', 'run', '-d', '--rm', '--name', self.container, '--network', 'host', '--user', f'{os.getuid()}:{os.getgid()}', '-v', f'{self.dir}:{self.dir}',
                            os.environ['STORE_CH_IMAGE'], 'clickhouse-server', f'--config-file={config}'], check=True, stdout=subprocess.DEVNULL)
        for _ in range(300):
            try:
                self.arm.request('SELECT 1')
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError(f'clickhouse did not start: see {self.dir}/error.log')

    def stop(self):
        if self.container:
            subprocess.run(['docker', 'stop', self.container], check=True, stdout=subprocess.DEVNULL)
        elif self.proc:
            self.proc.terminate()
            self.proc.wait(timeout=120)
        self.proc = self.container = None

    def sql(self, text, **params):
        return self.arm.request(text, **params)[0].decode()

    def counted(self, text):
        """(query id, server ms, TSV) of one statement"""
        qid = f'q{time.time_ns()}'
        data, r = self.arm.request(text, wait_end_of_query=1, query_id=qid, metrics_perf_events_enabled=1)
        return qid, int(re.search(r'"elapsed_ns":"?(\d+)', r.getheader('X-ClickHouse-Summary')).group(1)) / 1e6, data.decode()

    def log(self, qid):
        """(instructions, read blocks of 8 KiB, written bytes, projections used, read rows) of a finished query"""
        self.sql('SYSTEM FLUSH LOGS')
        row = self.sql("SELECT ProfileEvents['PerfInstructions'], read_bytes, written_bytes, arrayStringConcat(projections, '+'), read_rows FROM system.query_log "
                       f"WHERE type = 'QueryFinish' AND query_id = '{qid}'").rstrip('\n').split('\t')
        return int(row[0]), int(row[1]) // 8192, int(row[2]), row[3], int(row[4])

    def load(self, table, path):
        self.arm.threads = 4
        with open(path, 'rb') as f:
            self.arm.request(f'INSERT INTO {table} FORMAT CSV', body=f)
        self.arm.threads = THREADS

    def cold(self):
        os.sync()
        self.arm.evict()
        self.arm.os_cache.drop()


def export(cl, base, outdir):
    """the events and the distinct pairs of the fixture database as CSV files the arms load; made once per run"""
    os.makedirs(outdir, exist_ok=True)
    paths = {}
    for table, select in (('events', EVENT_SQL), ('tenant_pairs', PAIR_SQL)):
        paths[table] = os.path.join(outdir, table + '.csv')
        if not os.path.exists(paths[table]):
            cl.psql(base, '-c', f"\\copy ({select}) TO '{paths[table]}' CSV")
    return paths


def plan_note(spec_mv, cls, used, read_rows):
    source = 'view ev_mm' if spec_mv and cls in MV_CLASSES else 'events'
    return f"{source}; projections: {used or 'none'}; read_rows {read_rows}"


def run_once(srv, sql):
    """(instructions, ms, blocks read, (projections, rows read), TSV) of one run"""
    qid, ms, tsv = srv.counted(sql)
    ins, blocks, _, used, read_rows = srv.log(qid)
    return ins, ms, blocks, (used, read_rows), tsv


def measure(srv, sql, runs=3):
    """(warm, cold) as scope_stock_arms.measure: warm the median by instructions of `runs`, cold after the caches are emptied; each (instr, ms, hit, read, (projections, rows), TSV)"""
    srv.sql(sql)
    warm = sorted((run_once(srv, sql) for _ in range(runs)), key=lambda r: r[0])[runs // 2]
    srv.cold()
    cold = run_once(srv, sql)
    return (warm[0], warm[1], warm[2], 0, warm[3], warm[4]), (cold[0], cold[1], 0, cold[2], cold[3], cold[4])


def timed(srv, sql):
    """(instructions, ms, bytes written to parts) of one statement"""
    qid, ms, _ = srv.counted(sql)
    ins, _, written, _, _ = srv.log(qid)
    return ins, ms, written


def write_costs(srv):
    out = {'insert 10k events': [timed(srv, INSERT_10K)]}
    medians = [int(m) for m in srv.sql("SELECT dim_a_id FROM (SELECT dim_a_id, count() n FROM events WHERE id < 100000000 GROUP BY 1) m WHERE n BETWEEN 350 AND 450 "
                                      f"AND dim_a_id NOT IN (SELECT member FROM tenant_pairs WHERE tenant_id = {EDIT_TENANT}) ORDER BY 1 LIMIT 10").split()]
    big = [int(srv.sql("SELECT dim_a_id FROM (SELECT dim_a_id, count() n FROM events WHERE id < 100000000 GROUP BY 1) m "
                       f"WHERE dim_a_id NOT IN (SELECT member FROM tenant_pairs WHERE tenant_id = {EDIT_TENANT}) ORDER BY n DESC LIMIT 1"))]
    for label, ms_ in (('median member', medians), ('largest member', big)):
        grants = [timed(srv, f'INSERT INTO tenant_pairs VALUES ({EDIT_TENANT}, {m})') for m in ms_]
        revokes = [timed(srv, f'DELETE FROM tenant_pairs WHERE tenant_id = {EDIT_TENANT} AND member = {m} SETTINGS lightweight_deletes_sync = 2') for m in ms_]
        out[f'pair grant, {label}'], out[f'pair revoke, {label}'] = grants, revokes
    return out


def run_arm(srv, a, name, classes, oracle, paths, quiet):
    """the result dict of scope_stock_arms.run_arm for a ClickHouse arm"""
    spec = SPECS[name]
    quiet()
    for table in ('ev_mm_mv', 'ev_mm', 'tenant_pairs', 'events'):
        srv.sql(f'DROP TABLE IF EXISTS {table} SYNC')
    t0 = time.time()
    for stmt in spec['create']:
        srv.sql(stmt)
    for table, path in paths.items():
        srv.load(table, path)
    for stmt in spec['after']:
        srv.sql(stmt)
    srv.sql('OPTIMIZE TABLE events FINAL')
    build_s = time.time() - t0
    size = int(srv.sql(size_sql(name)))
    cells = []
    for t in a.tenants:
        quiet()
        for cname, tmpl in classes.items():
            sql = tmpl.format(t=t)
            warm, cold = measure(srv, sql)
            digest, ref = rows_md5(warm[5]), oracle[(t, cname)]
            note = plan_note(spec['mv'], cname, *warm[4])
            cells.append((name, t, cname, warm[:4], cold[:4], 'ok' if digest == ref else 'DIFF', digest, note))
            print(f'{name} t{t} {cname}: {warm[0] / 1e6:.2f} M instr {warm[1]:.1f} ms read {cold[3]} {"ok" if digest == ref else "DIFF"}', file=sys.stderr, flush=True)
    writes = write_costs(srv)
    dirty = []
    for t in (a.tenants[0], a.tenants[-1]):
        for cname, tmpl in classes.items():
            sql = tmpl.format(t=t)
            warm, _ = measure(srv, sql, 1)
            dirty.append((t, cname, warm[0], warm[1], rows_md5(warm[5])))
    return dict(name=name, build_s=build_s, build_wal=0, size=size, note=spec['note'], cells=cells, parallel=[], writes=writes, dirty=dirty)
