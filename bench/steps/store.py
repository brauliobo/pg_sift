#!/usr/bin/env python3
"""The store benchmark: ten questions about an online store's orders on PostgreSQL, PostgreSQL with pg_sift, Citus columnar, Elasticsearch and ClickHouse.
Called by steps/store.sh, which starts the containers (one per server, equal CPU and memory limits). stdlib only; `python3 -I`.

    store.py gen   --rows N --data DIR            the seeded CSV every system loads (bench/sql/store/gen.sql)
    store.py load  --rows N --data DIR --out DIR  load it into every engine: load time, size
    store.py run   --rows N --out DIR             warm latency of the ten queries on every engine, result equality, plans
    store.py cold  --rows N --out DIR             latency of the first run after a restart of the server
    store.py updates --rows N --out DIR           updates, deletes, read-after-write, reads under an update load, space (changes the data: runs last)
    store.py merge OUT... -o bench/results/store.json
    store.py render [--results FILE] [--readme FILE] [--benchmarks FILE]    the Markdown tables, rewritten between the bench:store markers

Engines: pg (heap + btree + GIN), sift (heap + sift_col + sift), sift_table (column-format table + sift), es (Elasticsearch, request
cache off), es_cached (the same with the shard request cache on), ch (ClickHouse), citus (Citus on one node, the table in the
`columnar` access method: no UPDATE, DELETE or GIN, so it is not in the update tables). A latency is the wall time of one request from this
process to the server on 127.0.0.1 and back, one client, the response read to its end (psql / HTTP keep-alive connection). The first run of
a query on an engine is not timed: its result is compared with the result of PostgreSQL (the reference) and the run stops on a difference.
"""
import argparse, csv, hashlib, http.client, itertools, json, math, os, queue, re, statistics, subprocess, sys, threading, time, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from stats import number, summary  # noqa: E402  (steps/stats.py)
SQL = os.path.join(HERE, '..', 'sql', 'store')
NAME = os.environ.get('STORE_NAME', 'sift-store')
PORT = int(os.environ.get('STORE_PORT', 56280))
START_LOAD = open('/proc/loadavg').read().split()[:3]     # the load average when this command started: only recorded, for the older tables
QUIET = os.path.join(HERE, '..', '..', 'scripts', 'quiet.sh')
NULL = '\\N'
TOL = 1e-9                      # relative tolerance of numbers (sums of money are exact in all engines; averages differ in the last digits)
CH_AUTH = {'X-ClickHouse-User': 'bench', 'X-ClickHouse-Key': 'bench'}


def quiet(*args):
    """scripts/quiet.sh COMMAND: its stdout, stripped; the exit status is the helper's (0 quiet, 1 timeout, 3 no PSI)"""
    p = subprocess.run(['bash', QUIET, *args], capture_output=True, text=True)
    return p.returncode, p.stdout.strip() or p.stderr.strip()


def quiet_batch(what):
    """before a timed batch: wait for a quiet host; a batch that runs anyway is logged as NOISY and kept in the meta of the results"""
    rc, msg = quiet('wait')
    if rc:
        NOISY.append(what)
        log(f'{what}: NOISY, {msg} ({quiet("report")[1]})')


START_PRESSURE = quiet('report')[1]                       # the pressure when this command started
NOISY = []                                                # the batches that ran after SIFT_QUIET_TIMEOUT without a quiet host

COMPRESSION = {'sift_none': 'none', 'sift_enc': 'for+delta+rle+dict+scale+alp', 'sift_auto': 'auto', 'sift_lz4': 'lz4', 'sift_zstd': 'zstd:3'}   # the `compression` option of the sift_col index, one database each

# id -> (label, container, port offset, database or kind)
LANES = {
    'pg':         ('PostgreSQL', 'pg', 0, 'store'),
    'sift':       ('pg_sift `sift_col`', 'sift', 1, 'store_col'),
    'sift_table': ('pg_sift `sift_table`', 'sift', 1, 'store_tbl'),
    'citus':      ('Citus columnar', 'citus', 4, 'store'),
    'es':         ('Elasticsearch', 'es', 2, None),
    'es_cached':  ('Elasticsearch, request cache on', 'es', 2, None),
    'ch':         ('ClickHouse', 'ch', 3, None),
    'ch_mut':     ('ClickHouse, ALTER UPDATE', 'ch', 3, None),
    'ch_rmt':     ('ClickHouse, ReplacingMergeTree', 'ch', 3, None),
    **{e: (f'pg_sift `sift_col`, compression `{p}`', 'sift', 1, 'store_col_' + e[5:]) for e, p in COMPRESSION.items()},
}
DEFAULT_ENGINES = ['pg', 'sift', 'sift_table', 'citus', 'es', 'es_cached', 'ch']
PG_ENGINES = ['pg', 'sift', 'sift_table', 'citus', *COMPRESSION]
UPDATE_ENGINES = ['pg', 'sift', 'sift_table', 'es', 'ch', 'ch_mut', 'ch_rmt']
CH_TABLE = {'ch': 'store.orders', 'ch_mut': 'store.orders_mut', 'ch_rmt': 'store.orders_rmt'}
is_ch = lambda eid: eid in CH_TABLE
is_es = lambda eid: eid in ('es', 'es_cached')
DDL = {'pg': 'pg.sql', 'sift': 'sift.sql', 'sift_table': 'sift_table.sql', 'citus': 'citus.sql', **{e: 'sift.sql' for e in COMPRESSION}}


def die(msg):
    sys.exit(f'store: {msg}')


def log(msg):
    print(f'[{time.strftime("%T")}] {msg}', file=sys.stderr, flush=True)


def container(eid):
    return f'{NAME}-{LANES[eid][1]}'


def port(eid):
    return PORT + LANES[eid][2]


def sections(text):
    """`--@ name` lines cut a file into sections (text before the first one is the header)"""
    out, cur = {'': []}, ''
    for line in text.split('\n'):
        m = re.match(r'--@ (\w+)$', line)
        if m:
            cur = m.group(1)
            out[cur] = []
        else:
            out[cur].append(line)
    return {k: '\n'.join(v).strip() for k, v in out.items()}


def read(name):
    return open(os.path.join(SQL, name)).read()


# --- the queries

class Query:
    def __init__(self, path):
        self.id, self.name = os.path.basename(path)[:-4].split('_', 1)
        text = read(os.path.join('queries', os.path.basename(path)))
        self.sec = sections(text)
        head = self.sec.pop('').split('\n')
        self.title = head[0][3:]
        self.label = next((m[10:] for m in head if m.startswith('-- label: ')), self.title)
        self.key = next(([int(k) - 1 for k in m[8:].split()] for m in head if m.startswith('-- key: ')), [])

    def statement(self, eid, n, cache=False):
        """the text sent to engine eid: SQL, or for Elasticsearch (method, path, body, rows spec, tolerance)"""
        sub = lambda s: s.replace('{point_id}', str(n * 42 // 100)).replace('{rc}', 'true' if cache else 'false')
        if is_es(eid):
            lines = sub(self.sec['es']).split('\n')
            method, path = lines[0].split()
            spec = [l for l in lines if l.startswith('rows:')][0][5:].strip()
            tol = [float(l[4:]) for l in lines if l.startswith('tol:')]
            body = '\n'.join(l for l in lines[1:] if not l.startswith(('rows:', 'tol:'))).strip()
            return method, path, body.encode() if body else None, spec, tol[0] if tol else TOL
        family = 'ch' if is_ch(eid) else 'sift' if eid.startswith('sift') else eid   # a `--@ sift` or `--@ citus` section replaces the PostgreSQL text for that system
        sql = sub(self.sec[family if family in self.sec else 'pg'])
        return sql.replace('store.orders', CH_TABLE[eid] + (' FINAL' if eid == 'ch_rmt' else '')) if is_ch(eid) else sql


def load_queries(only=None):
    names = sorted(f for f in os.listdir(os.path.join(SQL, 'queries')) if f.endswith('.sql'))
    qs = [Query(os.path.join(SQL, 'queries', f)) for f in names]
    return [q for q in qs if not only or q.id in only or q.name in only]


# --- the clients

class Psql:
    """one psql backend kept for the run, on the published port of the container; the end of a statement's output is a marker line"""
    def __init__(self, eid, timeout=600):
        db = LANES[eid][3] if eid in PG_ENGINES else 'postgres'
        self.p = subprocess.Popen(['psql', '-X', '-q', '-At', '-F', '\t', '-P', f'null={NULL}', '-h', '127.0.0.1', '-p', str(port(eid)), '-U', 'postgres', '-d', db],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        try:
            self.run(f'SET statement_timeout = {timeout * 1000}')
            self.run('\\o /dev/null')
        except BaseException:   # no backend (the server is starting): reap this psql
            self.p.kill()
            self.p.communicate()
            raise

    def run(self, sql, capture=False):
        w = self.p.stdin.write
        if capture:
            w('\\o\n')
        w(sql + ('' if sql.startswith('\\') else ';') + '\n')
        if capture:
            w('\\o /dev/null\n')
        w('\\echo __DONE__\n')
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == '':
                raise RuntimeError('psql ended: ' + ' '.join(out[-3:]))
            if line.strip() == '__DONE__':
                break
            out.append(line.rstrip('\n'))
        bad = [l for l in out if l.startswith(('ERROR', 'FATAL'))]
        if bad:
            raise RuntimeError(bad[0][:300])
        return out

    def close(self):
        self.p.stdin.close()
        self.p.wait()


class Http:
    def __init__(self, eid, headers=None, timeout=900):
        self.port, self.headers, self.timeout, self.conn = port(eid), headers or {}, timeout, None

    def request(self, method, path, body=None, headers=None):
        for attempt in (0, 1):
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=self.timeout)
                self.conn.request(method, path, body=body, headers={**self.headers, **(headers or {})})
                r = self.conn.getresponse()
                data = r.read()
                if r.status >= 400:
                    raise RuntimeError(f'{method} {path[:80]}: {r.status} {data[:300].decode(errors="replace")}')
                return data
            except (ConnectionError, http.client.HTTPException, OSError):
                self.conn = None
                if attempt:
                    raise

    def close(self):
        if self.conn:
            self.conn.close()
            self.conn = None


def dig(o, path):
    for k in path.split('.'):
        o = o[k]
    return o


def es_text(data, spec):
    """the rows an Elasticsearch response holds, as the tab separated text of the other engines: `PATH -> col col ...`"""
    path, _, cols = spec.partition('->')
    node = dig(json.loads(data), path.strip())
    cols = cols.split()
    rows = node if isinstance(node, list) else [node]
    return ''.join('\t'.join(NULL if v is None else str(v) for v in ([dig(r, c) for c in cols] if cols else [r])) + '\n' for r in rows)


class Client:
    """one engine: run(statement, capture) -> (milliseconds, text or None)"""
    def __init__(self, eid):
        self.eid = eid
        self.psql = Psql(eid) if eid in PG_ENGINES else None
        self.http = None if self.psql else Http(eid, CH_AUTH if is_ch(eid) else {'Content-Type': 'application/json'})

    def run(self, st, capture=False):
        t0 = time.perf_counter()
        if self.psql:
            out = self.psql.run(st, capture)
            ms = (time.perf_counter() - t0) * 1e3
            return ms, ('\n'.join(out) + '\n' if out else '') if capture else None
        if is_ch(self.eid):
            data = self.http.request('POST', '/?database=store&default_format=TSV', st.encode())
            ms = (time.perf_counter() - t0) * 1e3
            return ms, data.decode() if capture else None
        method, path, body, spec, _ = st
        data = self.http.request(method, path, body)
        ms = (time.perf_counter() - t0) * 1e3
        return ms, es_text(data, spec) if capture else None

    def explain(self, st):
        if not self.psql:
            return None
        text = '\n'.join(self.psql.run('EXPLAIN (COSTS OFF) ' + st, capture=True))
        nodes = list(dict.fromkeys(re.findall(r'Custom Scan \(\w+\)|(?:Parallel )?(?:Seq|Index Only|Index|Bitmap Heap) Scan', text)))
        return ', '.join(nodes)

    def close(self):
        (self.psql or self.http).close()


# --- docker

def docker(*args, check=True):
    return subprocess.run(['docker', *args], capture_output=True, text=True, check=check).stdout


def server_log(eid, lines=6):
    """the last error lines the server of the engine logged (kept next to a failure)"""
    r = subprocess.run(['docker', 'logs', '--since', '3m', container(eid)], capture_output=True, text=True)
    bad = [l.strip()[:250] for l in (r.stdout + r.stderr).split('\n') if re.search(r'ERROR|PANIC|FATAL|corrupt|exception', l, re.I)]
    return ' | '.join(bad[-lines:])


def wait_ready(eid, timeout=300):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if eid in PG_ENGINES:
                Psql(eid).close()
            elif is_ch(eid):
                Http(eid, CH_AUTH).request('GET', '/ping')
            else:
                Http(eid).request('GET', '/_cluster/health?wait_for_status=green&timeout=30s')
            return
        except Exception:
            time.sleep(0.5)
    die(f'{eid} is not ready after {timeout} s')


def restart(eid):
    docker('restart', '-t', '30', container(eid))
    wait_ready(eid)


# --- one engine at a time, idle background work before timing

IDLE_CPU = float(os.environ.get('SIFT_IDLE_CPU', 2))   # percent of one CPU, docker stats: a container below it is idle


def ask(eid, method, path, body=None):
    h = Http(eid, CH_AUTH if is_ch(eid) else {'Content-Type': 'application/json'})
    try:
        return h.request(method, path, body).decode()
    finally:
        h.close()


def merging(eid):
    """how many merges (ClickHouse: and mutations; Elasticsearch: segment merges; PostgreSQL: autovacuum workers) the server of the engine is running"""
    if is_ch(eid):
        return int(ask('ch', 'POST', '/', b'SELECT (SELECT count() FROM system.merges) + (SELECT count() FROM system.mutations WHERE NOT is_done)'))
    if is_es(eid):
        return sum(n['indices']['merges']['current'] for n in json.loads(ask('es', 'GET', '/_nodes/stats/indices/merge?filter_path=nodes.*.indices.merges.current'))['nodes'].values())
    return int(psql_value(eid, "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'autovacuum worker'"))


def wait_idle(engines, skip=()):
    """wait (SIFT_QUIET_TIMEOUT s, default 60) until every container of the engines but those of skip is under IDLE_CPU percent of a CPU (docker stats) and done merging;
    what was still running when the time ran out ('' when all were idle)"""
    reps = {container(e): e for e in reversed(engines) if container(e) not in skip}
    if not reps:
        return ''
    deadline = time.time() + float(os.environ.get('SIFT_QUIET_TIMEOUT', 60))
    while True:
        cpu = {n: float(c.rstrip('%')) for n, c in map(str.split, docker('stats', '--no-stream', '--format', '{{.Name}} {{.CPUPerc}}', *reps).splitlines())}
        busy = [f'{n} {c:.0f}% cpu' for n, c in cpu.items() if c >= IDLE_CPU] + [f'{n} merging' for n, e in reps.items() if merging(e)]
        if not busy or time.time() > deadline:
            return ', '.join(busy)
        time.sleep(2)


def batch(what, engines, skip=()):
    """before a timed batch: the other containers idle and their merges done (wait_idle), then the host quiet (scripts/quiet.sh); a batch that runs anyway is NOISY"""
    busy = wait_idle(engines, skip)
    quiet_batch(what)
    if busy:
        log(f'{what}: NOISY, still busy: {busy}')
        if what not in NOISY:
            NOISY.append(what)


# --- data

def pg_args(eid, db=None):
    return ['-h', '127.0.0.1', '-p', str(port(eid)), '-U', 'postgres', '-d', db or LANES[eid][3] or 'postgres']


def psql_file(eid, sql, db=None, variables=()):
    cmd = ['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', *[x for kv in variables for x in ('-v', kv)], *pg_args(eid, db), '-f', '-']
    r = subprocess.run(cmd, input=sql, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'psql: {r.stderr.strip()[:500]}')
    return r.stdout


def psql_value(eid, sql, db=None):
    r = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', *pg_args(eid, db), '-c', sql], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'psql: {r.stderr.strip()[:500]}')
    return r.stdout.strip()


def csv_path(args):
    return os.path.join(args.data, f'orders-{args.rows}.csv')


def stamp(args):
    return f'{args.rows} {hashlib.sha256(read("gen.sql").encode()).hexdigest()[:12]}'


def cmd_gen(args):
    path = csv_path(args)
    os.makedirs(args.data, exist_ok=True)
    if os.path.exists(path) and open(path + '.stamp').read().strip().split()[:2] == stamp(args).split():
        log(f'{path}: already generated ({open(path + ".stamp").read().split()[2][:12]})')
        return 0
    t0 = time.time()
    psql_file('pg', 'DROP DATABASE IF EXISTS gen WITH (FORCE); CREATE DATABASE gen', 'postgres')
    psql_file('pg', read('gen.sql'), 'gen', [f'n={args.rows}'])
    psql_file('pg', f"\\copy orders_src TO '{path}' CSV", 'gen')
    psql_file('pg', 'DROP DATABASE gen WITH (FORCE)', 'postgres')
    digest = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    open(path + '.stamp', 'w').write(f'{stamp(args)} {digest}\n')
    log(f'{path}: {args.rows} orders, {os.path.getsize(path) / 1e6:.0f} MB, generated in {time.time() - t0:.0f} s, sha256 {digest[:16]}')
    return 0


class Rows:
    def __init__(self, tier):
        self.tier, self.rows = tier, []

    def add(self, engine, cls, variant, metric, unit, value=None, st=None):
        r = dict(dataset='store', tier=self.tier, engine=engine, **{'class': cls}, variant=variant, metric=metric, unit=unit)
        r.update(st if st else dict(value=value, min=value, max=value, stderr=None, n=1, values=[value]))
        r['values'] = [round(v, 4) if isinstance(v, float) else v for v in r['values']]
        for k in ('value', 'min', 'max', 'stderr'):
            if isinstance(r[k], float):
                r[k] = round(r[k], 4)
        self.rows.append(r)


def mib(b):
    return round(float(b) / 1048576, 1)


def timed(rows, eid, phase, fn):
    t0 = time.time()
    out = fn()
    rows.add(eid, '-', phase, 'load_s', 's', value=round(time.time() - t0, 1))
    return out


CHUNK = 1000000
# Citus columnar keeps its stripe, chunk group and chunk metadata in tables of its own schema, shared by the columnar tables of the database (only `orders` here): on disk, so counted
COLUMNAR_META = " + (SELECT coalesce(sum(pg_total_relation_size(oid)), 0) FROM pg_class WHERE relnamespace = 'columnar_internal'::regnamespace AND relkind = 'r')"


def copy_chunks(eid, path):
    """COPY the file in transactions of CHUNK rows, a VACUUM after each (it merges the segments a sift_table closes), through one psql session"""
    p = subprocess.Popen(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', *pg_args(eid), '-f', '-'], stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with open(path) as f:
            while lines := list(itertools.islice(f, CHUNK)):
                p.stdin.write('COPY orders FROM STDIN CSV;\n')
                p.stdin.writelines(lines)
                p.stdin.write('\\.\nVACUUM orders;\n')
        p.stdin.close()
    except BrokenPipeError:   # psql stopped on an error: its message follows
        pass
    err = p.stderr.read()
    if p.wait():
        raise RuntimeError('psql: ' + err.strip()[:500])


def build_indexes(eid, text, rows):
    """each statement of the `indexes` section on its own, timed (`build NAME`, the seconds a user waits for that index); the `compression` option of the lane goes on the sift_col index"""
    total = 0
    for stmt in re.split(r';[ \t]*\n', re.sub(r'(?m)^--.*$', '', text)):
        if not stmt.strip():
            continue
        if eid in COMPRESSION:
            stmt = re.sub(r'(USING sift_col \(.*\))\s*$', rf"\1 WITH (compression = '{COMPRESSION[eid]}')", stmt.strip().rstrip(';'))
        name = m[1] if (m := re.search(r'INDEX (\w+)', stmt)) else 'primary key'
        t0 = time.time()
        psql_file(eid, stmt + ';')
        rows.add(eid, '-', 'build ' + name, 'build_s', 's', value=round(time.time() - t0, 1))
        total += time.time() - t0
    rows.add(eid, '-', 'indexes', 'load_s', 's', value=round(total, 1))


def load_pg(eid, args, rows):
    sec = sections(read(DDL[eid]))
    db = LANES[eid][3]
    def fresh():
        psql_file(eid, f'DROP DATABASE IF EXISTS {db} WITH (FORCE); CREATE DATABASE {db}', 'postgres')
        psql_file(eid, sec['table'])

    fresh()
    try:   # one COPY, one transaction: what a user does first
        timed(rows, eid, 'rows', lambda: psql_file(eid, f"\\copy orders FROM '{csv_path(args)}' CSV"))
    except RuntimeError as x:   # a failure is a result; the rows then go in transactions of CHUNK rows
        rows.add(eid, '-', 'rows', 'load_error', 'text', value=str(x)[:400])
        log(f'{eid}: {x}; loading in transactions of {CHUNK:,} rows')
        fresh()
        timed(rows, eid, 'rows', lambda: copy_chunks(eid, csv_path(args)))
        rows.add(eid, '-', 'rows', 'chunk_rows', 'rows', value=CHUNK)
    build_indexes(eid, sec['indexes'], rows)
    timed(rows, eid, 'vacuum', lambda: psql_file(eid, 'VACUUM (FREEZE, ANALYZE) orders'))
    psql_file(eid, 'CHECKPOINT')
    plus = COLUMNAR_META if eid == 'citus' else ''
    rows.add(eid, '-', '-', 'size_mib', 'MiB', value=mib(psql_value(eid, "SELECT pg_total_relation_size('orders')" + plus)))
    rows.add(eid, '-', 'table', 'size_mib', 'MiB', value=mib(psql_value(eid, "SELECT pg_table_size('orders')" + plus)))
    if eid == 'citus':   # the settings the table was created with, read back from the server (the defaults of columnar.*)
        rows.add(eid, '-', 'orders', 'columnar_options', 'text', value=psql_value(eid, "SELECT 'chunk_group_row_limit=' || chunk_group_row_limit || ', stripe_row_limit=' || stripe_row_limit || ', compression=' || "
                                                                                       "compression || ', compression_level=' || compression_level FROM columnar.options WHERE relation = 'orders'::regclass"))
    if eid == 'sift_table':   # blocks the table file holds and has free (the write pages of the load, sealed into columns): the file does not shrink
        rows.add(eid, '-', 'free', 'size_mib', 'MiB', value=mib(int(psql_value(eid, "SELECT free_blocks FROM sift.table_space('orders')")) * 8192))
    if eid == 'sift' or eid in COMPRESSION:   # what the index holds: the options it was built with and, per leaf, the chunks by encoding and their bytes
        rows.add(eid, '-', 'orders_col', 'reloptions', 'text', value=psql_value(eid, "SELECT coalesce(reloptions::text, '(not set)') FROM pg_class WHERE relname = 'orders_col'"))
        rows.add(eid, '-', 'orders_col', 'col_leaves', 'json', value=psql_value(eid, "SELECT json_agg(json_build_object('leaf', path, 'type', type, 'const', const_chunks, 'for', for_chunks, "
                                                                                 "'pack', pack_chunks, 'codec', codec_chunks, 'bytes', chunk_bytes, 'dict', dict_bytes) ORDER BY leaf) FROM sift.col_leaves('orders_col')"))
    for name, size in (l.split('|') for l in psql_value(eid, "SELECT indexrelid::regclass || '|' || pg_relation_size(indexrelid) FROM pg_index WHERE indrelid = 'orders'::regclass ORDER BY 1").split('\n')):
        rows.add(eid, '-', 'index ' + name, 'size_mib', 'MiB', value=mib(size))


def load_ch(args, rows):
    h = Http('ch', CH_AUTH)
    sec = sections(read('ch.sql'))
    h.request('POST', '/', b'DROP TABLE IF EXISTS store.orders')
    for stmt in sec['table'].split(';\n'):
        h.request('POST', '/', stmt.rstrip(';').encode())
    with open(csv_path(args), 'rb') as f:
        timed(rows, 'ch', 'rows', lambda: h.request('POST', '/?' + urllib.parse.urlencode({'query': 'INSERT INTO store.orders FORMAT CSV'}), f))
    timed(rows, 'ch', 'merge', lambda: h.request('POST', '/', sec['after'].rstrip(';').encode()))
    size = h.request('POST', '/', b"SELECT sum(bytes_on_disk) FROM system.parts WHERE database = 'store' AND table = 'orders' AND active").decode()
    rows.add('ch', '-', '-', 'size_mib', 'MiB', value=mib(size))
    h.close()


def bulk_docs(path, batch):
    with open(path, newline='') as f:
        buf = []
        for r in csv.reader(f):
            buf.append('{"index":{"_id":"%s"}}\n%s\n' % (r[0], json.dumps(dict(id=int(r[0]), customer_id=int(r[1]), country=r[2], category=r[3], status=r[4], amount=float(r[5]),
                                                                                quantity=int(r[6]), created_at=r[7], title=r[8]))))
            if len(buf) == batch:
                yield ''.join(buf).encode()
                buf = []
        if buf:
            yield ''.join(buf).encode()


def load_es(args, rows):
    h = Http('es')
    js = {'Content-Type': 'application/json'}
    h.request('DELETE', '/orders?ignore_unavailable=true')
    h.request('PUT', '/orders', read('es.json').encode(), js)
    q, errors = queue.Queue(maxsize=6), []

    def sender():
        c = Http('es')
        while True:
            body = q.get()
            if body is None:
                return
            try:
                if json.loads(c.request('POST', '/orders/_bulk', body, {'Content-Type': 'application/x-ndjson'}))['errors']:
                    errors.append('bulk reported errors')
            except Exception as x:
                errors.append(str(x))

    def bulk():
        threads = [threading.Thread(target=sender) for _ in range(3)]
        for t in threads:
            t.start()
        for body in bulk_docs(csv_path(args), 20000):
            q.put(body)
        for _ in threads:
            q.put(None)
        for t in threads:
            t.join()
        if errors:
            die('elasticsearch bulk: ' + errors[0])
        h.request('POST', '/orders/_refresh')

    timed(rows, 'es', 'rows', bulk)
    h.request('PUT', '/orders/_settings', json.dumps({'index': {'refresh_interval': '1s'}}).encode(), js)
    timed(rows, 'es', 'merge', lambda: h.request('POST', '/orders/_forcemerge?max_num_segments=1'))
    h.request('POST', '/orders/_flush')
    stats = json.loads(h.request('GET', '/orders/_stats/store'))
    rows.add('es', '-', '-', 'size_mib', 'MiB', value=mib(stats['_all']['primaries']['store']['size_in_bytes']))
    h.close()


def servers(engines):
    """the versions and settings of the servers that take part, as the servers say them"""
    v = {}
    settings = lambda eid: psql_value(eid, "SELECT string_agg(name || '=' || current_setting(name), ', ' ORDER BY name) FROM pg_settings WHERE name IN "
                                           "('shared_buffers', 'work_mem', 'jit', 'effective_cache_size')")
    for eid in [e for e in ('pg', 'sift', 'sift_table') if e in engines]:
        v[eid] = psql_value(eid, 'SHOW server_version')
        v[eid + '_sift'] = psql_value(eid, "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")
        v[eid + '_settings'] = settings(eid)
    if 'citus' in engines:
        v['citus'] = psql_value('citus', 'SHOW server_version')
        v['citus_columnar'] = psql_value('citus', "SELECT extversion FROM pg_extension WHERE extname = 'citus_columnar'")
        v['citus_image'] = docker('inspect', '-f', '{{.Config.Image}}', container('citus')).strip()
        v['citus_settings'] = settings('citus')
    if 'sift' in engines:
        v['sift_libs'] = docker('exec', container('sift'), 'sh', '-c', 'ldd $(find /usr/lib/postgresql -name pg_sift.so | head -1) | grep -Eo "lib(lz4|zstd)[^ ]*" | sort -u | paste -sd" "').strip()
        v['sift_table_compression'] = psql_value('sift', "LOAD 'pg_sift'; SELECT current_setting('sift.table_compression')") or "'' (unset)"
    if any(is_es(e) for e in engines):
        es = json.loads(Http('es').request('GET', '/'))
        v['es'] = es['version']['number']
        v['es_lucene'] = es['version']['lucene_version']
        opts = next(l.split('=', 1)[1] for l in docker('inspect', '-f', '{{range .Config.Env}}{{println .}}{{end}}', container('es')).split('\n') if l.startswith('ES_JAVA_OPTS'))
        v['es_heap'] = ' '.join(a for a in opts.split() if a.startswith('-Xm'))
    if any(is_ch(e) for e in engines):
        ch = Http('ch', CH_AUTH)
        v['ch'] = ch.request('POST', '/', b'SELECT version()').decode().strip()
        v['ch_threads'] = ch.request('POST', '/', b"SELECT value FROM system.settings WHERE name = 'max_threads'").decode().strip()
    return v


PG_THREADS = ('max_parallel_workers_per_gather', 'max_parallel_maintenance_workers', 'max_parallel_workers', 'max_worker_processes', 'parallel_setup_cost', 'parallel_tuple_cost',
              'min_parallel_table_scan_size', 'min_parallel_index_scan_size')


def effective(args, plan=True):
    """the thread settings of every server of the engines, as the servers report them, checked against the budget N (--cpus): PostgreSQL and pg_sift a leader plus N - 1 parallel
    workers, ClickHouse and Elasticsearch N threads / processors, the container a quota of N CPUs and no pinning. Dies on a difference; plan: also how many workers a count(*) plans"""
    n, out, wrong = args.cpus, {}, []

    def check(who, name, got, want):
        if str(got) != str(want):
            wrong.append(f'{who} {name} is {got}, expected {want}')

    for eid in {LANES[e][1]: e for e in reversed(args.engines)}.values():
        c = who = LANES[eid][1]
        inspect = lambda f: docker('inspect', '-f', '{{.HostConfig.' + f + '}}', container(eid)).strip()
        out[c] = dict(quota_cpus=int(inspect('NanoCpus')) / 1e9, cpuset=inspect('CpusetCpus'))
        check(who, 'container quota (--cpus)', out[c]['quota_cpus'], float(n))
        check(who, 'cpuset (the CPUs are not pinned)', out[c]['cpuset'] or 'none', 'none')
        if eid in PG_ENGINES:
            names = ', '.join(f"'{k}'" for k in PG_THREADS)
            got = dict(l.split('=') for l in psql_value(eid, f"SELECT string_agg(name || '=' || current_setting(name), E'\n') FROM pg_settings WHERE name IN ({names})", 'postgres').split('\n'))
            out[c].update(got)
            check(who, 'max_parallel_workers_per_gather', got['max_parallel_workers_per_gather'], n - 1)
            check(who, 'max_parallel_maintenance_workers', got['max_parallel_maintenance_workers'], n - 1)
            if int(got['max_parallel_workers']) < n - 1 or int(got['max_worker_processes']) < int(got['max_parallel_workers']):
                wrong.append(f'{who} max_parallel_workers {got["max_parallel_workers"]} / max_worker_processes {got["max_worker_processes"]} would cap the workers')
            if plan:   # the planner's own answer on a scan of the table: Workers Planned (absent: no Gather, the plan is not parallel)
                m = re.search(r'Workers Planned: (\d+)', psql_value(eid, 'EXPLAIN SELECT count(*) FROM orders'))
                out[c]['count_workers_planned'] = int(m[1]) if m else 0
        elif is_ch(eid):
            q = lambda t, ns: dict(l.split('\t') for l in ask('ch', 'POST', '/', f"SELECT name, value FROM system.{t} WHERE name IN ({ns}) FORMAT TSV".encode()).splitlines())
            out[c].update(q('settings', "'max_threads', 'max_insert_threads', 'max_final_threads'"))
            out[c].update(q('server_settings', "'background_pool_size', 'background_merges_mutations_concurrency_ratio'"))
            for k in ('max_threads', 'max_insert_threads', 'max_final_threads', 'background_pool_size'):
                check(who, k, out[c][k], n)
            check(who, 'background_merges_mutations_concurrency_ratio', out[c]['background_merges_mutations_concurrency_ratio'], 1)
        else:
            node = next(iter(json.loads(ask('es', 'GET', '/_nodes/os?filter_path=**.allocated_processors,**.available_processors'))['nodes'].values()))['os']
            jvm = next(iter(json.loads(ask('es', 'GET', '/_nodes/jvm?filter_path=**.input_arguments'))['nodes'].values()))['jvm']['input_arguments']
            pools = {r['name']: int(r['size']) for r in json.loads(ask('es', 'GET', '/_cat/thread_pool/search,write?format=json&h=name,size'))}
            out[c].update(node, jvm_processor_count=next((a.split('=')[1] for a in jvm if a.startswith('-XX:ActiveProcessorCount')), None), thread_pools=pools)
            check(who, 'allocated_processors (node.processors)', node['allocated_processors'], n)
            check(who, '-XX:ActiveProcessorCount', out[c]['jvm_processor_count'], n)
    if wrong:
        die('the thread budget is not in effect: ' + '; '.join(wrong))
    return out


def size_variants(args, rows):
    """the on-disk size of the systems with their other compression settings, the data copied inside each server: rows `codec NAME`"""
    if 'sift_table' in args.engines:
        for pol in ('none', 'auto', 'lz4', 'zstd:3'):
            t = 'orders_' + pol.split(':')[0]
            psql_file('sift_table', f"DROP TABLE IF EXISTS {t}; CREATE TABLE {t} (LIKE orders) USING sift_table; SET sift.table_compression = '{pol}'; INSERT INTO {t} SELECT * FROM orders; VACUUM {t}")
            rows.add('sift_table', '-', 'codec ' + pol, 'size_mib', 'MiB', value=mib(psql_value('sift_table', f"SELECT pg_table_size('{t}')")))
            rows.add('sift_table', '-', 'codec ' + pol + ' free', 'size_mib', 'MiB', value=mib(int(psql_value('sift_table', f"SELECT free_blocks FROM sift.table_space('{t}')")) * 8192))
            psql_file('sift_table', f'DROP TABLE {t}')
    if 'ch' in args.engines:
        h = Http('ch', CH_AUTH)
        ddl = lambda q: h.request('POST', '/', q.encode())
        cols = ddl("SELECT name FROM system.columns WHERE database = 'store' AND table = 'orders' ORDER BY position").decode().split()
        disk = lambda t: ddl(f"SELECT sum(bytes_on_disk) || ' ' || sum(data_uncompressed_bytes) FROM system.parts WHERE database = 'store' AND table = '{t}' AND active").decode().split()
        rows.add('ch', '-', 'codec default (LZ4)', 'size_mib', 'MiB', value=mib(disk('orders')[0]))
        rows.add('ch', '-', 'codec default (LZ4)', 'uncompressed_mib', 'MiB', value=mib(disk('orders')[1]))
        ddl('DROP TABLE IF EXISTS store.orders_zstd')
        ddl('CREATE TABLE store.orders_zstd AS store.orders')
        for c in cols:
            ddl(f'ALTER TABLE store.orders_zstd MODIFY COLUMN {c} CODEC(ZSTD(3))')
        ddl('INSERT INTO store.orders_zstd SELECT * FROM store.orders')
        ddl('OPTIMIZE TABLE store.orders_zstd FINAL')
        rows.add('ch', '-', 'codec ZSTD(3)', 'size_mib', 'MiB', value=mib(disk('orders_zstd')[0]))
        ddl('DROP TABLE store.orders_zstd')
    if 'es' in args.engines:
        h, js = Http('es'), {'Content-Type': 'application/json'}
        idx = json.loads(read('es.json'))
        idx['settings']['index']['codec'] = 'best_compression'
        h.request('DELETE', '/orders_best?ignore_unavailable=true')
        h.request('PUT', '/orders_best', json.dumps(idx).encode(), js)
        h.request('POST', '/_reindex?wait_for_completion=true&refresh=true', json.dumps({'source': {'index': 'orders'}, 'dest': {'index': 'orders_best'}}).encode(), js)
        h.request('POST', '/orders_best/_forcemerge?max_num_segments=1')
        h.request('POST', '/orders_best/_flush')
        rows.add('es', '-', 'codec best_compression', 'size_mib', 'MiB', value=mib(json.loads(h.request('GET', '/orders_best/_stats/store'))['_all']['primaries']['store']['size_in_bytes']))
        h.request('DELETE', '/orders_best')


def cmd_load(args):
    rows = Rows(args.tier)
    if not os.path.exists(csv_path(args)):
        die(f'{csv_path(args)} not found: run gen first')
    rows.add('-', '-', '-', 'orders', 'rows', value=args.rows)
    effective(args, plan=False)
    for eid in args.engines:
        if eid == 'es_cached':
            continue
        log(f'loading {eid}')
        if eid in PG_ENGINES:
            load_pg(eid, args, rows)
        elif eid == 'ch':
            load_ch(args, rows)
        else:
            load_es(args, rows)
        total = sum(r['value'] for r in rows.rows if r['engine'] == eid and r['metric'] == 'load_s')
        rows.add(eid, '-', '-', 'load_s', 's', value=round(total, 1))
        log(f'{eid}: loaded in {total:.0f} s, {next(r["value"] for r in rows.rows if r["engine"] == eid and r["metric"] == "size_mib" and r["variant"] == "-")} MiB')
    if args.compression:
        size_variants(args, rows)
    busy = wait_idle(args.engines)
    log(f'the servers are idle after the load' if not busy else f'still busy after the load (the timed batches wait again): {busy}')
    args.threads = effective(args)
    write(args, rows, 'load')
    return 0


def write(args, rows, part, extra=None):
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, f'store-{args.tier}-{part}.json')
    meta = dict(date=time.strftime('%FT%TZ', time.gmtime()), tier=args.tier, rows=args.rows, cpus=args.cpus, threads=args.threads, data_sha256=open(csv_path(args) + '.stamp').read().split()[2], loadavg=START_LOAD, loadavg_end=open('/proc/loadavg').read().split()[:3], pressure=START_PRESSURE, noisy=NOISY, **(extra or {}))
    with open(path, 'w') as f:
        f.write('{"meta": ' + json.dumps(meta) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows.rows) + '\n]}\n')
    log(f'written: {path}')


# --- the measurements

def rotated(items, i):
    k = i % len(items)
    return items[k:] + items[:k]


def difference(want, got, key, tol):
    """None when the results are equal, else what differs first: strings and integers exactly, numbers within tol (relative); rows sorted by the key columns when the query has them"""
    cut = lambda t: [[NULL if c == '' else c for c in l.split('\t')] for l in t.split('\n') if l != '']
    a, b = cut(want), cut(got)
    if key:
        a, b = sorted(a, key=lambda r: [r[k] for k in key]), sorted(b, key=lambda r: [r[k] for k in key])
    if len(a) != len(b):
        return f'{len(b)} rows, expected {len(a)}'
    for i, (ra, rb) in enumerate(zip(a, b)):
        if len(ra) != len(rb):
            return f'row {i}: {len(rb)} columns, expected {len(ra)}'
        for j, (x, y) in enumerate(zip(ra, rb)):
            nx, ny = number(x), number(y)
            if not (x == y or (nx is not None and ny is not None and abs(nx - ny) <= tol * max(1.0, abs(nx), abs(ny)))):
                return f'row {i} column {j + 1}: {y} expected {x} ({"|".join(ra)[:80]})'
    return None


def max_rel(want, got, key):
    """the largest relative difference between the numbers of two results (rows matched as in `difference`)"""
    cut = lambda t: sorted([l.split('\t') for l in t.split('\n') if l != ''], key=(lambda r: [r[k] for k in key]) if key else None)
    worst = 0.0
    for ra, rb in zip(cut(want), cut(got)):
        for x, y in zip(ra, rb):
            nx, ny = number(x), number(y)
            if nx is not None and ny is not None:
                worst = max(worst, abs(nx - ny) / max(1.0, abs(nx), abs(ny)))
    return worst


def tolerance(eid, st):
    return st[4] if is_es(eid) else TOL


def measure_query(q, clients, args, rows, cold_times=None):
    batch(q.id, list(clients))
    stmts = {e: q.statement(e, args.rows, e == 'es_cached') for e in clients}
    out, bad = {}, {}
    for e, c in clients.items():   # the first run of each engine: its result is checked, the plan is read
        try:
            out[e] = c.run(stmts[e], True)[1]
            plan = c.explain(stmts[e])
            if plan is not None:
                rows.add(e, q.id, '-', 'plan', 'node', value=plan)
        except Exception as x:
            bad[e] = str(x)
            log(f'{q.id} {e}: {x}')
    ref = out.get('pg', next(iter(out.values()), None))
    stop = []
    for e in out:
        if e == 'pg' or ref is None:
            verdict = 'reference' if e == 'pg' else 'unchecked'
        else:
            d = difference(ref, out[e], q.key, tolerance(e, stmts[e]))
            verdict = 'ok' if d is None else 'DIFF: ' + d
            if d is None:
                rows.add(e, q.id, '-', 'rel_diff', 'fraction', value=float(f'{max_rel(ref, out[e], q.key):.3g}'))
            if d is not None:
                stop.append(f'{q.id} {q.title}: {e} differs from PostgreSQL: {d}')
        rows.add(e, q.id, '-', 'result', 'check', value=verdict)
    for e, msg in bad.items():
        rows.add(e, q.id, '-', 'result', 'check', value='error: ' + msg)
    live = [e for e in clients if e in out]
    times = {e: [] for e in live}
    for _ in range(args.warmups - 1):
        for e in live:
            clients[e].run(stmts[e])
    for i in range(args.runs):
        for e in rotated(live, i):
            times[e].append(clients[e].run(stmts[e])[0])
    for e in live:
        rows.add(e, q.id, 'warm', 'latency_ms', 'ms', st=summary(times[e]))
    log(f'{q.id} {q.name}: ' + ' '.join(f'{e} {sorted(times[e])[len(times[e]) // 2]:.3g}' for e in live))
    return stop


def cmd_run(args):
    rows = Rows(args.tier)
    queries = load_queries(args.queries)
    args.threads = effective(args)
    clients = {e: Client(e) for e in args.engines}
    problems = []
    log(f'{len(queries)} queries, engines {" ".join(args.engines)}, {args.runs} runs after {args.warmups} warm-up(s), thread budget {args.cpus}')
    try:
        for q in queries:
            problems += measure_query(q, clients, args, rows)
            if problems and not args.keep_going:
                break
    finally:
        for c in clients.values():
            c.close()
        write(args, rows, 'warm', {'servers': servers(args.engines), 'runs': args.runs, 'warmups': args.warmups})
    for p in problems:
        print('DIFFERENT RESULT: ' + p, file=sys.stderr)
    return 3 if problems else 0


def cmd_cold(args):
    rows = Rows(args.tier)
    queries = load_queries(args.queries)
    stmts = {(q.id, e): q.statement(e, args.rows) for q in queries for e in args.engines if e != 'es_cached'}
    effective(args)
    for e in [e for e in args.engines if e != 'es_cached']:
        for q in queries:
            batch(f'cold {e} {q.id}', args.engines, [container(e)])
            ts = []
            for _ in range(args.cold_runs):
                restart(e)
                c = Client(e)
                try:
                    ts.append(c.run(stmts[(q.id, e)])[0])
                except RuntimeError as x:   # an error is a result, as in the warm run
                    log(f'cold {e} {q.id}: {x}')
                    break
                finally:
                    c.close()
            if len(ts) < args.cold_runs:
                continue
            rows.add(e, q.id, 'cold', 'latency_ms', 'ms', st=summary(ts))
            log(f'cold {e} {q.id} {q.name}: {sorted(ts)[len(ts) // 2]:.3g} ms')
    args.threads = effective(args)   # after the restarts: the settings are the container's, not the process's
    write(args, rows, 'cold', {'cold_runs': args.cold_runs})
    return 0


# --- updates, deletes and what they cost the readers (the `updates` command)

UPDATE_SET = "status = 'refunded', amount = amount - 1"
PHASES = [('update 1%', 'update', 'id % 100 = 0', lambda i: i % 100 == 0), ('update 10%', 'update', 'id % 10 = 1', lambda i: i % 10 == 1),
          ('delete 5%', 'delete', 'id % 20 = 3', lambda i: i % 20 == 3)]
PROBE_ID = lambda k: 20 * k + 5          # untouched by the phases (id % 20 = 5): its status becomes 'probe'
DIGEST = {'pg': "SELECT status, count(*), sum(amount) FROM orders GROUP BY status ORDER BY 1",
          'ch': "SELECT status, count(), sum(amount) FROM store.orders GROUP BY status ORDER BY 1"}
LOAD_QUERIES = ['03', '05', '09']        # the reads timed before, during and after the churn
LOAD_QUERY = '03'


class Writer:
    """one connection of an engine that changes rows: the update path its documentation recommends for the job"""
    def __init__(self, eid):
        self.eid, self.c = eid, Client(eid)
        self.t = CH_TABLE.get(eid)

    def close(self):
        self.c.close()

    def bulk(self, kind, ids):
        """one batch of rows by primary key"""
        lst = ','.join(map(str, ids))
        if self.eid in PG_ENGINES:
            self.c.psql.run(f"UPDATE orders SET {UPDATE_SET} WHERE id IN ({lst})" if kind == 'update' else f"DELETE FROM orders WHERE id IN ({lst})")
        elif self.eid == 'ch':
            self.c.http.request('POST', '/?database=store', (f"UPDATE {self.t} SET {UPDATE_SET} WHERE id IN ({lst})" if kind == 'update' else f"DELETE FROM {self.t} WHERE id IN ({lst})").encode())
        elif self.eid == 'ch_rmt':
            self.c.http.request('POST', '/?database=store', self.rmt_insert(f"id IN ({lst})", "'refunded'", 'amount - 1', 0) if kind == 'update' else self.rmt_insert(f"id IN ({lst})", 'status', 'amount', 1))
        elif self.eid == 'es':
            action = lambda i: ('{"update":{"_id":"%d"}}\n{"script":{"source":"ctx._source.status=\'refunded\'; ctx._source.amount-=1"}}\n' if kind == 'update' else '{"delete":{"_id":"%d"}}\n') % i
            r = json.loads(self.c.http.request('POST', '/orders/_bulk', ''.join(action(i) for i in ids).encode(), {'Content-Type': 'application/x-ndjson'}))
            if r['errors']:
                raise RuntimeError('bulk: ' + json.dumps(next(i for i in r['items'] if list(i.values())[0].get('error')))[:300])
        else:
            raise RuntimeError(f'{self.eid}: rows are changed by predicate')

    def rmt_insert(self, where, status, amount, deleted):
        """a new version of the rows, inserted: ReplacingMergeTree replaces the old one when the parts merge, FINAL hides it until then"""
        return (f"INSERT INTO {self.t} SELECT id, customer_id, country, category, {status}, {amount}, quantity, created_at, title, toUnixTimestamp64Micro(now64(6)), {deleted} "
                f"FROM {self.t} WHERE {where}").encode()

    def predicate(self, kind, pred):
        """ClickHouse mutation: one ALTER for all the rows of the predicate, waited for"""
        sql = f"ALTER TABLE {self.t} UPDATE {UPDATE_SET} WHERE {pred} SETTINGS mutations_sync = 1" if kind == 'update' else f"ALTER TABLE {self.t} DELETE WHERE {pred} SETTINGS mutations_sync = 1"
        self.c.http.request('POST', '/?database=store', sql.encode())

    def digest(self):
        if self.eid in PG_ENGINES:
            return '\n'.join(self.c.psql.run(DIGEST['pg'], True)) + '\n'
        if is_ch(self.eid):
            return self.c.http.request('POST', '/?database=store&default_format=TSV', DIGEST['ch'].replace('store.orders', self.t + (' FINAL' if self.eid == 'ch_rmt' else '')).encode()).decode()
        self.c.http.request('POST', '/orders/_refresh')
        body = {"size": 0, "aggs": {"s": {"terms": {"field": "status", "size": 20, "order": {"_key": "asc"}}, "aggs": {"a": {"sum": {"field": "amount"}}}}}}
        return es_text(self.c.http.request('POST', '/orders/_search?request_cache=false', json.dumps(body).encode()), 'aggregations.s.buckets -> key doc_count a.value')

    def probe_set(self, i, mode):
        """set one row's status to 'probe' the way the engine does a single-row change; returns when the engine acknowledged"""
        if self.eid in PG_ENGINES:
            self.c.psql.run(f"UPDATE orders SET status = 'probe' WHERE id = {i}")
        elif self.eid == 'es':
            self.c.http.request('POST', f'/orders/_update/{i}' + ('?refresh=wait_for' if mode == 'wait_for' else ''), b'{"doc": {"status": "probe"}}')
        elif self.eid == 'ch':
            self.c.http.request('POST', '/?database=store', f"UPDATE {self.t} SET status = 'probe' WHERE id = {i}".encode())
        elif self.eid == 'ch_rmt':
            self.c.http.request('POST', '/?database=store', self.rmt_insert(f'id = {i}', "'probe'", 'amount', 0))
        else:
            self.c.http.request('POST', '/?database=store', f"ALTER TABLE {self.t} UPDATE status = 'probe' WHERE id = {i} SETTINGS mutations_sync = 0".encode())

    def probe_count(self):
        if self.eid in PG_ENGINES:
            return int(self.c.psql.run("SELECT count(*) FROM orders WHERE status = 'probe'", True)[0])
        if is_ch(self.eid):
            return int(self.c.http.request('POST', '/?database=store&default_format=TSV', f"SELECT count() FROM {self.t}{' FINAL' if self.eid == 'ch_rmt' else ''} WHERE status = 'probe'".encode()))
        body = {"size": 0, "track_total_hits": True, "query": {"term": {"status": "probe"}}}
        return json.loads(self.c.http.request('POST', '/orders/_search?request_cache=false', json.dumps(body).encode()))['hits']['total']['value']

    def maintain(self):
        if self.eid in PG_ENGINES:
            self.c.psql.run('VACUUM (ANALYZE) orders')
        elif is_ch(self.eid):
            self.c.http.request('POST', '/?database=store', f'OPTIMIZE TABLE {self.t} FINAL'.encode())
        else:
            self.c.http.request('POST', '/orders/_forcemerge?max_num_segments=1')
            self.c.http.request('POST', '/orders/_flush')

    def free_mib(self):
        """sift_table: the free blocks inside the table file (None for the others)"""
        if self.eid != 'sift_table':
            return None
        return mib(int(self.c.psql.run("SELECT free_blocks FROM sift.table_space('orders')", True)[0]) * 8192)

    def size_mib(self):
        if self.eid in PG_ENGINES:
            self.c.psql.run('CHECKPOINT')
            return mib(self.c.psql.run("SELECT pg_total_relation_size('orders')", True)[0])
        if is_ch(self.eid):
            t = self.t.split('.')[1]
            return mib(self.c.http.request('POST', '/', f"SELECT sum(bytes_on_disk) FROM system.parts WHERE database = 'store' AND table = '{t}' AND active".encode()).decode())
        self.c.http.request('POST', '/orders/_refresh')
        return mib(json.loads(self.c.http.request('GET', '/orders/_stats/store'))['_all']['primaries']['store']['size_in_bytes'])


def prepare_copies(engines):
    """the tables the other ClickHouse update paths change: copies of the loaded one (the lightweight UPDATE engine changes the original)"""
    h = Http('ch', CH_AUTH)
    sql = []
    if 'ch_mut' in engines:
        sql += ['DROP TABLE IF EXISTS store.orders_mut', 'CREATE TABLE store.orders_mut AS store.orders', 'INSERT INTO store.orders_mut SELECT * FROM store.orders', 'OPTIMIZE TABLE store.orders_mut FINAL']
    if 'ch_rmt' in engines:
        sql += ['DROP TABLE IF EXISTS store.orders_rmt'] + sections(read('ch.sql'))['rmt'].rstrip(';').split(';\n') + [
            'INSERT INTO store.orders_rmt SELECT *, 0, 0 FROM store.orders', 'OPTIMIZE TABLE store.orders_rmt FINAL']
    for s in sql:
        h.request('POST', '/', s.encode())
    h.close()


def percentile(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, math.ceil(p / 100 * len(xs)) - 1)]


def read_latency(w, q, args, rows, variant):
    c, st = w.c, q.statement(w.eid, args.rows)
    for _ in range(args.warmups):
        c.run(st)
    rows.add(w.eid, q.id, variant, 'latency_ms', 'ms', st=summary([c.run(st)[0] for _ in range(args.runs)]))


def churn(eid, n, rate, seconds, stop, out):
    """the update load: batches of 100 rows (ids drawn from the live rows, amount + 1) paced to `rate` rows a second, as fast as the engine takes them when it cannot keep up"""
    import random
    rnd, w, batch = random.Random(rate), Writer(eid), 100
    done = errors = 0
    first = None
    t0 = time.perf_counter()
    try:
        while not stop.is_set():
            ids = []
            while len(ids) < batch:
                i = rnd.randrange(1, n + 1)
                if i % 20 != 3:
                    ids.append(i)
            try:
                lst = ','.join(map(str, ids))
                if eid in PG_ENGINES:
                    w.c.psql.run(f"UPDATE orders SET amount = amount + 1 WHERE id IN ({lst})")
                elif eid == 'ch':
                    w.c.http.request('POST', '/?database=store', f"UPDATE store.orders SET amount = amount + 1 WHERE id IN ({lst})".encode())
                elif eid == 'ch_rmt':
                    w.c.http.request('POST', '/?database=store', w.rmt_insert(f'id IN ({lst})', 'status', 'amount + 1', 0))
                else:
                    r = json.loads(w.c.http.request('POST', '/orders/_bulk', ''.join('{"update":{"_id":"%d"}}\n{"script":{"source":"ctx._source.amount+=1"}}\n' % i for i in ids).encode(),
                                                    {'Content-Type': 'application/x-ndjson'}))
                    if r['errors']:
                        raise RuntimeError('bulk errors')
                done += batch
            except Exception as x:
                errors += 1
                first = first or str(x)[:200]
                time.sleep(0.05)
            lag = t0 + (done / rate) - time.perf_counter() if rate else 0
            if lag > 0:
                stop.wait(lag)
    finally:
        out.update(done=done, errors=errors, seconds=time.perf_counter() - t0, first_error=first)
        w.close()


def with_load(w, q, args, rows, rate):
    import threading as th
    stop, out = th.Event(), {}
    t = th.Thread(target=churn, args=(w.eid, args.rows, rate, args.load_seconds, stop, out), daemon=True) if rate else None
    if t:
        t.start()
        time.sleep(1)
    st, lat, fail = q.statement(w.eid, args.rows), [], None
    t0 = time.perf_counter()
    try:
        while time.perf_counter() - t0 < args.load_seconds:
            lat.append(w.c.run(st)[0])
    except Exception as x:   # a reader that fails is a result: kept, the engine's other levels still run
        fail = str(x)[:300]
    finally:
        if t:
            stop.set()
            t.join()
    v = f'load {rate}/s'
    if fail:
        rows.add(w.eid, q.id, v, 'read_error', 'text', value=fail)
        rows.add(w.eid, q.id, v, 'server_log', 'text', value=server_log(w.eid))
        log(f'{w.eid} load {rate}/s: the query failed after {len(lat)} runs: {fail}')
    if not lat:
        return
    rows.add(w.eid, q.id, v, 'latency_ms', 'ms', value=percentile(lat, 50))
    rows.add(w.eid, q.id, v, 'latency_p95_ms', 'ms', value=percentile(lat, 95))
    rows.add(w.eid, q.id, v, 'queries', 'queries', value=len(lat))
    if t:
        rows.add(w.eid, '-', v, 'updates_rows_s', 'rows/s', value=round(out['done'] / out['seconds']))
        rows.add(w.eid, '-', v, 'update_errors', 'batches', value=out['errors'])
        if out['first_error']:
            rows.add(w.eid, '-', v, 'update_error', 'text', value=out['first_error'])
    log(f'{w.eid} load {rate}/s: query p50 {percentile(lat, 50):.3g} ms p95 {percentile(lat, 95):.3g} ms' + (f', {out["done"] / out["seconds"]:.0f} rows/s, {out["errors"]} failed batches' if t else ''))


def read_after_write(w, args, rows):
    modes = {'pg': ['commit'], 'sift': ['commit'], 'sift_table': ['commit'], 'es': ['default', 'wait_for'], 'ch': ['lightweight'], 'ch_mut': ['mutation'], 'ch_rmt': ['insert']}[w.eid]
    k0 = 0
    for mode in modes:
        ack, vis = [], []
        for _ in range(args.probes):
            k0 += 1
            t0 = time.perf_counter()
            w.probe_set(PROBE_ID(k0), mode)
            t1 = time.perf_counter()
            while w.probe_count() < k0:
                time.sleep(0.002)
            vis.append((time.perf_counter() - t1) * 1e3)
            ack.append((t1 - t0) * 1e3)
        rows.add(w.eid, '-', mode, 'ack_ms', 'ms', st=summary(ack))
        rows.add(w.eid, '-', mode, 'visible_ms', 'ms', st=summary(vis))
        log(f'{w.eid} {mode}: acknowledged in {sorted(ack)[len(ack) // 2]:.3g} ms, visible to a query {sorted(vis)[len(vis) // 2]:.3g} ms later')


def update_engine(eid, args, rows, ref):
    w = Writer(eid)
    qs = {q.id: q for q in load_queries(LOAD_QUERIES)}
    n = args.rows
    for q in qs.values():
        read_latency(w, q, args, rows, 'baseline')
    point = lambda v: (rows.add(eid, '-', v, 'size_mib', 'MiB', value=w.size_mib()), w.free_mib() is not None and rows.add(eid, '-', v, 'free_mib', 'MiB', value=w.free_mib()))
    point('before')
    for name, kind, pred, test in PHASES:
        ids = [i for i in range(1, n + 1) if test(i)]
        t0 = time.perf_counter()
        if eid == 'ch_mut':
            w.predicate(kind, pred)
        else:
            for k in range(0, len(ids), 1000):
                w.bulk(kind, ids[k:k + 1000])
        dt = time.perf_counter() - t0
        rows.add(eid, '-', name, 'phase_s', 's', value=round(dt, 2))
        rows.add(eid, '-', name, 'phase_rows_s', 'rows/s', value=round(len(ids) / dt))
        d = w.digest()
        if eid == 'pg':
            ref[name] = d
            verdict = 'reference'
        else:
            diff = difference(ref[name], d, [0], TOL)
            verdict = 'ok' if diff is None else 'DIFF: ' + diff
        rows.add(eid, 'digest', name, 'result', 'check', value=verdict)
        log(f'{eid} {name}: {len(ids)} rows in {dt:.2f} s ({len(ids) / dt:.0f} rows/s), state {verdict}')
    read_after_write(w, args, rows)
    if eid != 'ch_mut':
        for rate in (0, 1000, 10000):
            with_load(w, qs[LOAD_QUERY], args, rows, rate)
    point('after churn')
    for q in qs.values():
        read_latency(w, q, args, rows, 'after churn')
    t0 = time.perf_counter()
    w.maintain()
    rows.add(eid, '-', 'maintenance', 'maintain_s', 's', value=round(time.perf_counter() - t0, 1))
    point('after maintenance')
    for q in qs.values():
        read_latency(w, q, args, rows, 'after maintenance')
    w.close()


def cmd_updates(args):
    rows, ref = Rows(args.tier), {}
    problems = []
    args.threads = effective(args)
    prepare_copies(args.engines)
    try:
        for eid in [e for e in UPDATE_ENGINES if e in args.engines]:
            log(f'updates on {eid}')
            batch(f'updates {eid}', args.engines, [container(eid)])
            try:
                update_engine(eid, args, rows, ref)
            except Exception as x:   # recorded, and the other engines still run
                rows.add(eid, '-', 'run', 'error', 'text', value=str(x)[:300])
                rows.add(eid, '-', 'run', 'server_log', 'text', value=server_log(eid))
                log(f'{eid}: {x}')
    finally:
        write(args, rows, 'updates', {'load_seconds': args.load_seconds, 'probes': args.probes})
    problems = [r for r in rows.rows if r['metric'] == 'result' and str(r['value']).startswith('DIFF')]
    for r in problems:
        print(f'DIFFERENT STATE: {r["engine"]} {r["variant"]}: {r["value"]}', file=sys.stderr)
    return 3 if problems else 0


def cmd_verify(args):
    """the thread budget of every server, before anything is loaded or timed (store.sh, after the containers start or change budget)"""
    th = effective(args, plan=False)
    log(f'thread budget {args.cpus} in effect: ' + json.dumps(th))
    return 0


def host():
    cpu = next(l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name'))
    ram = next(int(l.split()[1]) for l in open('/proc/meminfo') if l.startswith('MemTotal')) / 1048576
    return dict(cpu=cpu, threads=os.cpu_count(), ram=f'{ram:.0f} GiB', kernel=os.uname().release, docker=docker('version', '-f', '{{.Server.Version}}').strip(),
                cpus=os.environ.get('STORE_CPUS', '?'), cpus_list=os.environ.get('STORE_CPUS_LIST', '?'), memory=os.environ.get('STORE_MEMORY', '?'))


def cmd_merge(args):
    metas, rows = {}, []
    for path in args.files:
        d = json.load(open(path))
        tier = d['meta']['tier']
        metas.setdefault(tier, {}).update({k: v for k, v in d['meta'].items() if k not in ('date', 'loadavg', 'loadavg_end', 'pressure', 'noisy')})
        metas[tier].setdefault('parts', {})[os.path.basename(path).split('-')[-1][:-5]] = dict(date=d['meta']['date'], loadavg=d['meta']['loadavg'], loadavg_end=d['meta'].get('loadavg_end'), pressure=d['meta'].get('pressure'), noisy=d['meta'].get('noisy', []))
        metas[tier].pop('loadavg_end', None)
        rows += d['rows']
    with open(args.o, 'w') as f:
        f.write('{"meta": ' + json.dumps(dict(tiers=metas, host=host())) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows) + '\n]}\n')
    log(f'written: {args.o}')
    return 0


# --- the Markdown tables

class Results:
    def __init__(self, path):
        d = json.load(open(path))
        self.meta, self.rows = d['meta'], d['rows']
        tm = self.meta['tiers']
        self.all_tiers = sorted(tm, key=lambda t: (tm[t]['rows'], self.cpus(t)))   # a result set per size and thread budget (`1M-c3`; older results: one per size)
        self.tiers = [t for t in self.all_tiers if 'load' in tm[t]['parts']]       # one per size: the set that holds the load, the plans and the updates
        self.sets = {t: [x for x in self.all_tiers if tm[x]['rows'] == tm[t]['rows']] for t in self.tiers}   # all the sets of that size, the smallest budget first

    def cpus(self, tier):
        old = str(self.meta.get('host', {}).get('cpus', ''))   # results made before the thread budget: the one quota of the host record
        return self.meta['tiers'][tier].get('cpus') or (int(old) if old.isdigit() else 0)

    def get(self, tier, engine, cls='-', variant='-', metric=None):
        for r in self.rows:
            if (r['tier'], r['engine'], r['class'], r['variant'], r['metric']) == (tier, engine, cls, variant, metric):
                return r
        return None

    def num(self, *a, **k):
        r = self.get(*a, **k)
        return r['value'] if r else None

    def engines(self, tier, among):
        return [e for e in among if any(r['tier'] == tier and r['engine'] == e and r['metric'] in ('latency_ms', 'load_s') for r in self.rows)]

    def queries(self, tier):
        out = {}
        for r in self.rows:
            if r['tier'] == tier and r['metric'] == 'latency_ms':
                out[r['class']] = None
        return sorted(out)


def ms(v):
    if v is None:
        return 'n/a'
    if v >= 1000:
        return f'{v:,.0f}'
    return f'{v:.3g}' if v < 100 else f'{v:.0f}'


def spread(r):
    return f'{ms(r["value"])} ({ms(r["min"])}-{ms(r["max"])})' if r else 'n/a'


def md_table(head, body, align=None):
    align = align or ['---'] + ['---:'] * (len(head) - 1)
    return ['| ' + ' | '.join(head) + ' |', '|' + '|'.join(align) + '|'] + ['| ' + ' | '.join(r) + ' |' for r in body]


def seconds(v):
    return 'n/a' if v is None else f'{v:.0f} s' if v >= 10 else f'{v:.1f} s'


def size(v, free=None):
    s = 'n/a' if v is None else f'{v / 1024:.1f} GiB' if v >= 10240 else f'{v:,.0f} MiB'
    return s + (f' ({free:,.0f} free)' if free else '')


def best_bold(values, cells):
    """bold the cells whose value is within 5% of the best (lowest) one"""
    present = [v for v in values if v is not None]
    best = min(present) if present else None
    return [f'**{c}**' if v is not None and v <= best * 1.05 else c for v, c in zip(values, cells)]


def bench_table(R, tier, engines, labels, detail=False, variant='warm'):
    head = ['question' if detail else 'query'] + [labels[e] for e in engines]
    body = []
    for qid in R.queries(tier):
        title = (QUERY_TITLES if detail else QUERY_LABELS)[qid]
        rs = [R.get(tier, e, qid, variant, 'latency_ms') for e in engines]
        vals = [r['value'] if r else None for r in rs]
        cells = [spread(r) if detail else ms(r['value'] if r else None) for r in rs]
        body.append([title] + best_bold(vals, cells))
    return head, body


def load_rows(R, tier, engines):
    load = [R.num(tier, e, metric='load_s') for e in engines]
    disk = [R.num(tier, e, metric='size_mib') for e in engines]
    free = [R.num(tier, e, variant='free', metric='size_mib') for e in engines]
    return [['load time (data, indexes, merge)'] + best_bold(load, [seconds(v) for v in load]),
            ['size on disk (data and indexes; free blocks inside the file in parentheses)'] + best_bold(disk, [size(v, f) for v, f in zip(disk, free)])]


QUERY_TITLES = {q.id: q.title for q in load_queries()}
QUERY_LABELS = {q.id: q.label for q in load_queries()}
SHOWN = ['pg', 'sift', 'sift_table', 'citus', 'es', 'ch']   # the columns of the README table, in order; a system without rows in the results has no column


def losses(R, tier, engines, labels):
    """where pg_sift (the faster of its two layouts) is not the fastest system of the question, from the rows"""
    out = []
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    others = [e for e in engines if e not in ('sift', 'sift_table', 'es_cached')]
    for qid in R.queries(tier):
        m = {e: R.num(tier, e, qid, 'warm', 'latency_ms') for e in mine}
        m = {e: v for e, v in m.items() if v is not None}
        o = {e: R.num(tier, e, qid, 'warm', 'latency_ms') for e in others}
        o = {e: v for e, v in o.items() if v is not None}
        if not m or not o:
            continue
        me, ov = min(m, key=m.get), min(o, key=o.get)
        if o[ov] < m[me] * 0.95:
            out.append((qid, QUERY_TITLES[qid], labels[me], m[me], labels[ov], o[ov]))
    return out


def ratio(a, b):
    r = a / b
    return f'{r:.0f}x' if r >= 10 else f'{r:.1f}x'


def load_notes(R, tier):
    """the loads that failed as one COPY (a result) and were done in transactions of CHUNK rows"""
    out = []
    for e in PG_ENGINES:
        r = R.get(tier, e, '-', 'rows', 'load_error')
        if r:
            msg = ' '.join(re.findall(r'(?:ERROR|DETAIL):\s+([^\n]*?)(?= DETAIL:| HINT:| CONTEXT:|$)', r['value'].replace('\n', ' ')))
            out.append(f'Loading the {R.meta["tiers"][tier]["rows"]:,} rows into {LANES[e][0]} with one `COPY` failed (`{msg[:220]}`); they were loaded in transactions of '
                       f'{int(R.num(tier, e, "-", "rows", "chunk_rows") or CHUNK):,} rows with a `VACUUM` after each instead, and only that load is timed.')
    return out + error_notes(R, tier)


def error_notes(R, tier):
    """the questions a system answered with an error (their cells are n/a): the message, once per system"""
    out = []
    for e in LANES:
        bad = [(q, r['value'][7:]) for q in R.queries(tier) if (r := R.get(tier, e, q, '-', 'result')) and r['value'].startswith('error: ')]
        if bad:
            ids = ', '.join(str(int(q)) for q, _ in bad)
            out.append(f'{LANES[e][0]} failed on question{"s" * (len(bad) > 1)} {ids} at {R.meta["tiers"][tier]["rows"]:,} orders (`{bad[0][1][:160]}`); the cells are n/a.')
    return out


def cpu_name(n):
    return f'{n} CPU' + 's' * (n != 1)


def join_and(names):
    return ', '.join(names[:-1]) + (' and ' if len(names) > 1 else '') + names[-1]


def thread_line(R, tier):
    """what the servers of a result set reported for its thread budget (steps/store.py effective)"""
    th = R.meta['tiers'][tier].get('threads', {})
    pg = th.get('pg') or th.get('sift')
    who = [name for name, k in (('PostgreSQL', 'pg'), ('pg_sift', 'sift'), ('Citus', 'citus')) if k in th]
    out = [f'{join_and(who)}: a leader and {pg["max_parallel_workers_per_gather"]} parallel worker{"s" * (pg["max_parallel_workers_per_gather"] != "1")}'] * bool(pg)
    out += [f'Elasticsearch: `node.processors` {th["es"]["allocated_processors"]}'] * ('es' in th)
    out += [f'ClickHouse: `max_threads` {th["ch"]["max_threads"]}'] * ('ch' in th)
    return '; '.join(out)


def set_heading(R, tier):
    """the line above a table of one thread budget"""
    return f'{cpu_name(R.cpus(tier))} per container' + (f' ({thread_line(R, tier)})' if R.meta['tiers'][tier].get('threads') else '')


def citus_options(R, tier):
    r = R.get(tier, 'citus', '-', 'orders', 'columnar_options')
    return r['value'] if r else '?'


def citus_notes(R, tier):
    """what the Citus column is, from the rows of the tier: columnar only, one node, the settings it ran with"""
    if not R.engines(tier, ['citus']):
        return []
    s = R.meta['tiers'][tier]['servers']
    return [f'Citus {s["citus_columnar"]} (PostgreSQL {s["citus"].split()[0]}) is measured as one node with the table in its `columnar` access method and its default settings ({citus_options(R, tier)}), a primary key and no other index '
            '(columnar tables take btree and hash indexes only, so the three searches scan the title column). Columnar tables are append-only, so Citus is not in the update table.']


def readme_block(R):
    tier = R.tiers[-1]
    t = R.meta['tiers'][tier]
    s = t['servers']
    labels = {e: LANES[e][0] for e in LANES}
    engines = R.engines(tier, SHOWN)
    sets = R.sets[tier]
    host = R.meta.get('host', {})
    n = f'{t["rows"]:,}'
    quota = ' and '.join(str(R.cpus(x)) for x in sets)
    fair = ' and the same thread budget for every engine (a [fair comparison](docs/BENCHMARKS.md#fair-comparison-the-same-threads-for-every-engine)' + (f', one table for each of {quota} CPUs' if len(sets) > 1 else '') + ')'
    out = [f'An online store: a table of {n} orders (country, category, status, amount, date, a product title) and ten questions anyone can ask of it. Every system holds the same seeded rows, '
           f'runs one node in a docker container limited to {quota} CPUs (a quota, no pinning) and {host.get("memory", "?")} of memory{fair}, and answers over a local connection from one client. '
           f'Cells are the median of {t["runs"]} warm runs in milliseconds (lower is better; the fastest of a row, and anything within 5% of it, in bold). '
           f'Measured {t["parts"]["warm"]["date"][:10]} on {host.get("cpu", "?")} ({host.get("threads", "?")} threads), PostgreSQL {s["pg"]}, pg_sift {s["sift_sift"]}' + (f', Citus {s["citus_columnar"]} columnar' if 'citus' in engines else '') + f', Elasticsearch {s["es"]}, ClickHouse {s["ch"]}.', '']
    for x in sets:
        head, body = bench_table(R, x, R.engines(x, SHOWN), labels)
        if x == tier:
            body += load_rows(R, tier, engines)
        out += ([f'**{set_heading(R, x)}**', ''] if len(sets) > 1 else []) + md_table(head, body) + ['']
    if len(sets) > 1:
        out += [f'Load time and size on disk are those of the load at {cpu_name(R.cpus(tier))}; the updates ran at that budget too.', '']
    rd = R.get(tier, 'es', '06', '-', 'rel_diff')
    approx = f' (HyperLogLog++: its monthly counts were off by at most {rd["value"]:.2%} here)' if rd else ''
    out += [f'- Every result is compared with PostgreSQL\'s before it is timed, and the run stops on a difference. Elasticsearch counts distinct customers approximately{approx}; every other result matched exactly or within rounding.',
            f'- Elasticsearch runs with its request cache off (a repeated aggregation would otherwise be answered from memory); with it on, the medians are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md#analytics-an-online-store).',
            '- ClickHouse is a column store built for exactly these aggregations and Elasticsearch for exactly this search; PostgreSQL (heap, btree and GIN indexes) is what most teams run today.']
    out += ['- ' + n for n in citus_notes(R, tier)] + ['- ' + n for n in load_notes(R, tier)]
    plain = [q for q in R.queries(tier) if 'Seq Scan' in (R.get(tier, 'sift_table', q, '-', 'plan') or {}).get('value', '') and R.num(tier, 'sift_table', q, 'warm', 'latency_ms') and R.num(tier, 'pg', q, 'warm', 'latency_ms')]
    if plain and 'sift_table' in engines:
        ratios = [R.num(tier, 'sift_table', q, 'warm', 'latency_ms') / R.num(tier, 'pg', q, 'warm', 'latency_ms') for q in plain]
        out += [f'- `sift_table` ran {len(plain)} of the {len(R.queries(tier))} questions with a plain `Seq Scan` plan (no aggregate pushdown: [docs/USAGE.md](docs/USAGE.md#limits) lists what `SiftAgg` does not read) and took '
                f'{ratio(min(ratios), 1)} to {ratio(max(ratios), 1)} the time of PostgreSQL\'s heap on those; the other layout, `sift_col`, is an index over the ordinary table.']
    for x in sets:
        lo = losses(R, x, R.engines(x, SHOWN), labels)
        at = f' at {cpu_name(R.cpus(x))}' if len(sets) > 1 else ''
        if lo:
            out += [f'- **Where pg_sift is not the fastest{at}:** ' + '; '.join(f'{QUERY_LABELS[qid]}: {lb} {ratio(v1, v2)} slower than {lb2} ({ms(v1)} ms against {ms(v2)} ms)' for qid, _, lb, v1, lb2, v2 in lo) + '.']
        else:
            out += [f'- pg_sift was the fastest system, or within 5% of it, on every question{at}.']
    out += [f'- The same questions at {R.meta["tiers"][x]["rows"]:,} orders, cold runs after a restart, load times and plans: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#analytics-an-online-store).' for x in R.tiers[:-1]]
    out += ['', 'Reproduce with `bench/bench store all` (docker; `--docs` rewrites this section and docs/BENCHMARKS.md from the new numbers); the data generator, the table layout of every system and the ten queries are in `bench/sql/store/`.']
    return out


def host_noise(part):
    """how busy the host was at the start of a part: its pressure (scripts/quiet.sh), the load average for results made before; the NOISY batches"""
    out = part['pressure'] if part.get('pressure') else 'load average ' + ' '.join(part['loadavg'])
    return out + (f' (NOISY: {", ".join(part["noisy"])})' if part.get('noisy') else '')


def thread_table(R, tiers):
    """the settings each server reported for each result set (steps/store.py effective), one row per set"""
    pg_names = 'PostgreSQL, pg_sift' + ', Citus' * any('citus' in R.meta['tiers'][x].get('threads', {}) for x in tiers)
    head = ['result set', 'container `--cpus`', f'{pg_names}: workers per gather (maintenance)', 'ClickHouse: `max_threads` (background pool)', 'Elasticsearch: `node.processors`, JVM processors (search / write threads)']
    body = []
    for x in tiers:
        th = R.meta['tiers'][x].get('threads')
        if not th:
            continue
        pg, ch, es = th.get('pg') or th.get('sift'), th.get('ch'), th.get('es')
        quota = next(iter(th.values()))['quota_cpus']
        body.append([f'{R.meta["tiers"][x]["rows"]:,} orders, {cpu_name(R.cpus(x))}', f'{quota:g}',
                     f'{pg["max_parallel_workers_per_gather"]} ({pg["max_parallel_maintenance_workers"]})' if pg else 'n/a',
                     f'{ch["max_threads"]} ({ch["background_pool_size"]})' if ch else 'n/a',
                     f'{es["allocated_processors"]}, {es["jvm_processor_count"]} ({es["thread_pools"]["search"]} / {es["thread_pools"]["write"]})' if es else 'n/a'])
    return md_table(head, body, ['---'] + ['---:'] * 4) if body else []


def latency_tables(R, tier, shown, labels, heading):
    """the warm, request-cache-on and first-run tables of one result set (a size at one thread budget)"""
    t = R.meta['tiers'][tier]
    out = (heading + ['']) if heading else []
    out += [f'Seeded data (sha256 `{t["data_sha256"][:16]}`), measured {t["parts"]["warm"]["date"]}, {host_noise(t["parts"]["warm"])} at the start of the runs (the host is shared). '
            f'Median of {t["runs"]} warm runs, minimum and maximum in parentheses, milliseconds; the fastest of a row and anything within 5% of it in bold.', '']
    head, body = bench_table(R, tier, shown, labels, detail=True)
    out += md_table(head, body) + ['']
    out += ['Elasticsearch with the shard request cache on (the same request repeated is answered from the cache; the numbers above are with it off):', '']
    head, body = bench_table(R, tier, ['es', 'es_cached'], labels, detail=True)
    out += md_table(head, body) + ['']
    if any(R.get(tier, e, variant='cold', cls=q, metric='latency_ms') for e in shown for q in R.queries(tier)):
        out += ['First run after a restart of the server (process caches empty; the operating system\'s page cache is not dropped, which needs root): median of '
                f'{t.get("cold_runs", "?")} restarts, minimum and maximum in parentheses.', '']
        head, body = bench_table(R, tier, shown, labels, detail=True, variant='cold')
        out += md_table(head, body) + ['']
    return out


def citus_config(R, tier):
    """the Configuration bullet of the Citus column (nothing when the results have none): the image, the version, the table's settings as the server reports them"""
    if not R.engines(tier, ['citus']):
        return []
    s = R.meta['tiers'][tier]['servers']
    w = (R.meta['tiers'][tier].get('threads') or {}).get('citus', {}).get('count_workers_planned')
    return [f'- Citus {s["citus_columnar"]} (image `{s["citus_image"]}`, PostgreSQL {s["citus"].split()[0]}): one node, no workers added, `citus` preloaded as the image ships it and only the `columnar` access method of `citus_columnar` used; '
            f'the table `USING columnar` with the default settings ({citus_options(R, tier)}), a primary key and no other index (columnar tables take btree and hash indexes only: no GIN for the search, no `UPDATE` or `DELETE`, so it is not in the update tables). '
            f'The same server flags as the other PostgreSQL containers: {s["citus_settings"]}; `max_parallel_workers_per_gather` N - 1' + (f', and the planner planned {w} workers for `count(*)` on the table' if w is not None else '') + '.']


def benchmarks_block(R):
    labels = {e: LANES[e][0] for e in LANES}
    host = R.meta.get('host', {})
    out = []
    for tier in reversed(R.tiers):
        t = R.meta['tiers'][tier]
        s = t['servers']
        allq = ['pg', 'sift', 'sift_table', 'citus', 'es', 'es_cached', 'ch']
        engines = R.engines(tier, allq)
        shown = [e for e in engines if e != 'es_cached']
        out += [f'### {t["rows"]:,} orders', '']
        sets = R.sets[tier]
        for x in sets:   # the questions at each thread budget, the smallest first
            out += latency_tables(R, x, [e for e in R.engines(x, allq) if e != 'es_cached'], labels, [f'#### {set_heading(R, x)}'] if len(sets) > 1 else [])
        if len(sets) > 1:
            out += [f'The load, the sizes, the plans and the updates are those of the load at {cpu_name(R.cpus(tier))}.', '']
        phases = [('rows', 'data'), ('indexes', 'indexes'), ('vacuum', 'vacuum, analyze'), ('merge', 'merge to one segment')]
        body = []
        for key, label in phases:
            vals = [R.num(tier, e, variant=key, metric='load_s') for e in shown]
            if any(v is not None for v in vals):
                body.append([label] + [seconds(v) for v in vals])
        body.append(['**total**'] + [seconds(R.num(tier, e, metric='load_s')) for e in shown])
        body.append(['**size on disk**'] + [size(R.num(tier, e, metric='size_mib'), R.num(tier, e, variant='free', metric='size_mib')) for e in shown])
        out += ['Load time (from the same CSV file, one client) and size:', ''] + md_table(['phase'] + [labels[e] for e in shown], body) + ['']
        out += [n for n in load_notes(R, tier)] + [''] * bool(load_notes(R, tier))
        parts = []
        for e in PG_ENGINES:
            for r in R.rows:
                if r['tier'] == tier and r['engine'] == e and r['metric'] == 'size_mib' and r['variant'].startswith('index '):
                    parts.append([labels[e], r['variant'][6:], size(r['value'])])
        if parts:
            out += ['Indexes of the PostgreSQL layouts:', ''] + md_table(['layout', 'index', 'size'], parts, ['---', '---', '---:']) + ['']
        plans = [[QUERY_TITLES[q]] + [(R.get(tier, e, q, '-', 'plan') or {}).get('value', 'n/a') for e in PG_ENGINES if e in engines] for q in R.queries(tier)]
        out += ['Scan nodes of the PostgreSQL plans (`EXPLAIN`):', ''] + md_table(['question'] + [labels[e] for e in PG_ENGINES if e in engines], plans, ['---'] * (1 + len([e for e in PG_ENGINES if e in engines]))) + ['']
    last = R.tiers[-1]
    t = R.meta['tiers'][last]
    s = t['servers']
    quota = ' and '.join(str(R.cpus(x)) for x in R.sets[last])
    th = t.get('threads')
    pg = (th.get('pg') or th.get('sift')) if th else None
    pg_text = (f'`max_parallel_workers_per_gather` N - 1 (a leader and N - 1 workers are N processes; `max_parallel_maintenance_workers` the same), `max_worker_processes` {pg["max_worker_processes"]} and `max_parallel_workers` '
               f'{pg["max_parallel_workers"]} (far above the budget), `parallel_setup_cost` {pg["parallel_setup_cost"]}, `parallel_tuple_cost` {pg["parallel_tuple_cost"]}, `min_parallel_table_scan_size` {pg["min_parallel_table_scan_size"]}, '
               'the same flags for both containers') if pg else '`max_parallel_workers_per_gather` and the other settings are the same for both containers'
    out += ['### Configuration', '',
            f'- Host: {host.get("cpu", "?")}, {host.get("threads", "?")} hardware threads, {host.get("ram", "?")} of memory, {host.get("kernel", "?")}; docker {host.get("docker", "?")}. '
            f'Each server runs alone in a container with `--cpus N --memory {host.get("memory", "?")}` (N = {quota}: a CPU quota, the CPUs are not pinned), no swap; the servers not under test sit idle.',
            ]
    if th:
        out += ['- Thread budget: every engine is told the same N as the quota, because they start different numbers of threads on their own. What each server reported after it started (checked against N before anything is timed):', '']
        out += thread_table(R, [x for tier in R.tiers for x in R.sets[tier]]) + ['']
    out += [f'- PostgreSQL {s["pg"]} (the same image for both layouts, pg_sift {s["sift_sift"]} built from this checkout' + (f', linked with `{s["sift_libs"]}`, `compression` unset on the index = `auto`, `sift.table_compression` {s["sift_table_compression"]}' if 'sift_libs' in s else '') + f'): {s["pg_settings"]}; {pg_text}.',
            *citus_config(R, last),
            f'- Elasticsearch {s["es"]} (Lucene {s["es_lucene"]}): one node, one shard, no replica, JVM heap `{s["es_heap"]}`, {"`node.processors` and `-XX:ActiveProcessorCount` set to N, " if th else ""}security and machine learning off; `scaled_float` (factor 100) for the amount, `keyword` for country, category and status, `date` for the time, `text` for the title; '
            'merged to one segment after the load; aggregations of the search API with `size: 0`; the shard request cache off (`request_cache=false`) except in the table that says otherwise; distinct counts are `cardinality` (HyperLogLog++, `precision_threshold` 40000), the only approximate result.',
            f'- ClickHouse {s["ch"]}: `MergeTree`, `PARTITION BY` month, `ORDER BY (country, created_at, id)` (`status` and `amount` stay out of the key so that they can be updated), `LowCardinality` strings, `Decimal(10, 2)` for the amount, a text index on the title, a Bloom filter on the id, the settings lightweight updates need; {"`max_threads`, `max_insert_threads`, `max_final_threads` and `background_pool_size` set to N" if th else f"`max_threads` {s['ch_threads']} (what the container limit gives it)"}; optimized to one part per partition after the load.',
            '- pg_sift: `sift_col` over the columns the reports read plus the month of the order, a `sift` BM25 index on the title; `sift_table` is the table itself in column format with a `sift` index on the title and the primary key. PostgreSQL: primary key, btree on the date and on country and status, GIN on `to_tsvector(\'simple\', title)`.',
            '- Method: one client, one request at a time, wall time from this process to the end of the response over a persistent local connection; the engines are timed in turn within each run so a drift of the host load hits all of them; one untimed run per engine first (its result is checked against PostgreSQL\'s), then the timed runs. The numbers of one table are comparable with each other, not with another host.']
    return out


# --- the tables of the update benchmark

UPD_LABELS = {'pg': 'PostgreSQL', 'sift': 'pg_sift `sift_col`', 'sift_table': 'pg_sift `sift_table`', 'es': 'Elasticsearch', 'ch': 'ClickHouse, lightweight UPDATE',
              'ch_mut': 'ClickHouse, ALTER UPDATE', 'ch_rmt': 'ClickHouse, ReplacingMergeTree + FINAL'}
UPD_PATH = {'pg': '`UPDATE` / `DELETE` ... `WHERE id IN (...)`, 1,000 ids per statement', 'sift': 'the same statements; the heap is updated, the `sift_col` index follows',
            'sift_table': 'the same statements on the column-format table', 'es': '`_bulk` `update` (script) and `delete` actions, 1,000 per request, default refresh',
            'ch': '`UPDATE` / `DELETE FROM` ... `WHERE id IN (...)` (patch parts), 1,000 ids per statement',
            'ch_mut': 'one `ALTER TABLE ... UPDATE` / `DELETE` with the predicate of the rows, `mutations_sync = 1`',
            'ch_rmt': '`INSERT ... SELECT` of a new version of the rows, 1,000 ids per statement; reads use `FINAL`'}
UPD_DOCS = [
    ('PostgreSQL', [('Routine vacuuming', 'https://www.postgresql.org/docs/18/routine-vacuuming.html'), ('UPDATE', 'https://www.postgresql.org/docs/18/sql-update.html')]),
    ('pg_sift', [('Column indexes, aggregates and sift_table', 'USAGE.md#column-indexes-aggregates-and-sift_table')]),
    ('Elasticsearch', [('Update API', 'https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-update'),
                       ('Bulk API', 'https://www.elastic.co/guide/en/elasticsearch/reference/8.19/docs-bulk.html'),
                       ('The refresh parameter', 'https://www.elastic.co/docs/reference/elasticsearch/rest-apis/refresh-parameter'),
                       ('Force a merge', 'https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-indices-forcemerge')]),
    ('ClickHouse', [('Updates in ClickHouse (which method for which job)', 'https://clickhouse.com/docs/concepts/features/operations/update/overview'),
                    ('UPDATE (lightweight updates)', 'https://clickhouse.com/docs/reference/statements/update'),
                    ('ALTER TABLE ... UPDATE (mutations)', 'https://clickhouse.com/docs/reference/statements/alter/update'),
                    ('ReplacingMergeTree', 'https://clickhouse.com/docs/reference/engines/table-engines/mergetree-family/replacingmergetree')]),
]


def upd_tier(R):
    tiers = [t for t in R.tiers if any(r['tier'] == t and r['metric'] in ('phase_rows_s', 'ack_ms') for r in R.rows)]
    return tiers[-1] if tiers else None


def upd_engines(R, tier):
    return [e for e in UPDATE_ENGINES if any(r['tier'] == tier and r['engine'] == e and r['metric'] in ('phase_rows_s', 'ack_ms', 'error') for r in R.rows)]


def rate(v):
    return 'n/a' if v is None else f'{v:,.0f}'


def best_high(values, cells):
    present = [v for v in values if v is not None]
    best = max(present) if present else None
    return [f'**{c}**' if v is not None and v >= best / 1.05 else c for v, c in zip(values, cells)]


def upd_rows(R, tier, engines, detail):
    """(label, cells) rows of the update table; the fastest cell of a row in bold"""
    g = lambda e, v, m, c='-': R.num(tier, e, c, v, m)
    out = []
    for v, label in (('update 1%', 'update 1% of the rows (rows/s, higher is better)'), ('update 10%', 'update 10% of the rows (rows/s)'), ('delete 5%', 'delete 5% of the rows (rows/s)')):
        vals = [g(e, v, 'phase_rows_s') for e in engines]
        out.append([label] + best_high(vals, [rate(x) for x in vals]))
    modes = {'pg': 'commit', 'sift': 'commit', 'sift_table': 'commit', 'es': 'default', 'ch': 'lightweight', 'ch_mut': 'mutation', 'ch_rmt': 'insert'}
    ack = [g(e, modes[e], 'ack_ms') for e in engines]
    vis = [g(e, modes[e], 'visible_ms') for e in engines]
    out.append(['single-row change: acknowledged after (ms)'] + best_bold(ack, [ms(x) for x in ack]))
    out.append(['single-row change: seen by the next query (ms after the acknowledgement)'] + best_bold(vis, [ms(x) for x in vis]))
    if 'es' in engines:
        w = [f'{ms(g(e, "wait_for", "ack_ms"))} (then {ms(g(e, "wait_for", "visible_ms"))})' if e == 'es' else '' for e in engines]
        out.append(['same with `refresh=wait_for`: acknowledged after (ms)'] + [x or '' for x in w])
    for r in (0, 1000, 10000):
        v = f'load {r}/s'
        p50 = [g(e, v, 'latency_ms', LOAD_QUERY) for e in engines]
        p95 = [g(e, v, 'latency_p95_ms', LOAD_QUERY) for e in engines]
        cells = [(f'{ms(a)}' + (f' / {ms(b)}' if detail else '')) if a is not None else ('error' if R.get(tier, e, LOAD_QUERY, v, 'read_error') else 'n/a') for e, a, b in zip(engines, p50, p95)]
        out.append([f'revenue per month while {r:,} rows/s are updated: p50' + (' / p95' if detail else '') + ' (ms)'] + best_bold(p50, cells))
        if r:
            ach = [g(e, v, 'updates_rows_s') for e in engines]
            out.append([f'rows/s the update load actually reached (asked {r:,})'] + [rate(x) if x is not None else 'n/a' for x in ach])
    for v, label in (('before', 'size before'), ('after churn', 'size after the changes'), ('after maintenance', 'size after VACUUM / merge / OPTIMIZE')):
        vals = [g(e, v, 'size_mib') for e in engines]
        out.append([label] + best_bold(vals, [size(x, g(e, v, 'free_mib')) for e, x in zip(engines, vals)]))
    return out


def upd_read_rows(R, tier, engines):
    out = []
    for qid in LOAD_QUERIES:
        for v in ('baseline', 'after churn', 'after maintenance'):
            vals = [R.num(tier, e, qid, v, 'latency_ms') for e in engines]
            out.append([f'{QUERY_TITLES[qid]}, {v}'] + best_bold(vals, [ms(x) for x in vals]))
    vals = [R.num(tier, e, '-', 'maintenance', 'maintain_s') for e in engines]
    out.append(['time of the maintenance step'] + [seconds(x) for x in vals])
    return out


def upd_losses(R, tier, engines):
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    others = [e for e in engines if e not in ('sift', 'sift_table')]
    out = []
    for v, label in (('update 1%', 'updating 1% of the rows'), ('update 10%', 'updating 10% of the rows'), ('delete 5%', 'deleting 5% of the rows')):
        m = {e: R.num(tier, e, '-', v, 'phase_rows_s') for e in mine}
        o = {e: R.num(tier, e, '-', v, 'phase_rows_s') for e in others}
        m, o = {e: x for e, x in m.items() if x}, {e: x for e, x in o.items() if x}
        if m and o and max(o.values()) > max(m.values()) * 1.05:
            a, b = max(m, key=m.get), max(o, key=o.get)
            out.append(f'{label}: {UPD_LABELS[b]} {ratio(o[b], m[a])} faster than {UPD_LABELS[a]} ({rate(o[b])} against {rate(m[a])} rows/s)')
    for e in mine:
        for r in (1000, 10000):
            v = f'load {r}/s'
            a, b = R.num(tier, e, LOAD_QUERY, v, 'latency_ms'), R.num(tier, e, LOAD_QUERY, 'load 0/s', 'latency_ms')
            ach = R.num(tier, e, '-', v, 'updates_rows_s')
            if R.get(tier, e, LOAD_QUERY, v, 'read_error'):
                out.append(f'{UPD_LABELS[e]} under {r:,} rows/s of updates: the query failed ({R.get(tier, e, LOAD_QUERY, v, "read_error")["value"][:80]})')
            elif a and b and a > b * 1.5:
                out.append(f'{UPD_LABELS[e]} under {r:,} rows/s of updates: the query is {ratio(a, b)} slower than with no updates ({ms(b)} to {ms(a)} ms)')
            if ach and ach < r * 0.9:
                out.append(f'{UPD_LABELS[e]} reached {rate(ach)} of the {r:,} rows/s asked for')
    return out


def upd_sentences(R, tier, engines):
    """the claims of the README, each computed from the rows of the tier"""
    g = lambda e, v, m, c='-': R.num(tier, e, c, v, m)
    out = []
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    ranked = sorted(((g(e, 'update 10%', 'phase_rows_s'), e) for e in engines if g(e, 'update 10%', 'phase_rows_s')), reverse=True)
    if ranked:
        out.append('Updating 10% of the rows, in rows per second: ' + ', '.join(f'{UPD_LABELS[e]} {rate(v)}' for v, e in ranked) + '.')
    vis = {e: g(e, {'es': 'default'}.get(e, {'pg': 'commit', 'sift': 'commit', 'sift_table': 'commit', 'ch': 'lightweight', 'ch_mut': 'mutation', 'ch_rmt': 'insert'}.get(e)), 'visible_ms') for e in engines}
    es_v, pg_v, mut_v = vis.get('es'), vis.get('pg'), vis.get('ch_mut')
    parts = []
    if es_v is not None:
        parts.append(f'Elasticsearch showed a change to a query {ms(es_v)} ms after the acknowledgement with the default refresh')
    if mut_v is not None:
        parts.append(f'a ClickHouse `ALTER UPDATE` {ms(mut_v)} ms after it returned')
    if pg_v is not None:
        parts.append(f'PostgreSQL and pg_sift {ms(pg_v)}-{ms(max(vis[e] for e in mine if vis.get(e) is not None))} ms' if mine else f'PostgreSQL {ms(pg_v)} ms')
    if parts:
        out.append('Read-after-write (median of the single-row changes): ' + '; '.join(parts) + '.')
    sz = lambda e, v: g(e, v, 'size_mib')
    grow = [(e, sz(e, 'after churn') / sz(e, 'before')) for e in engines if sz(e, 'before') and sz(e, 'after churn')]
    if grow:
        out.append('Size after the changes against before: ' + ', '.join(f'{UPD_LABELS[e]} {x:.2f}x' for e, x in grow) + ' (after the maintenance step: ' + ', '.join(
            f'{UPD_LABELS[e]} {sz(e, "after maintenance") / sz(e, "before"):.2f}x' for e, _ in grow if sz(e, 'after maintenance')) + ').')
    return out


def updates_readme(R):
    tier = upd_tier(R)
    if not tier:
        return ['No update numbers yet: `bench/bench store all` measures them.']
    engines = upd_engines(R, tier)
    t = R.meta['tiers'][tier]
    rows = [r for r in upd_rows(R, tier, engines, False) if not r[0].startswith(('single-row change: acknowledged', 'rows/s the update', 'same with'))]
    out = [f'On the {t["rows"]:,}-order table, after the read tests, each system changes its rows the way its documentation recommends for the job (below); the state after each step is compared with PostgreSQL\'s. '
           f'Query: revenue per month, timed while a second connection updates rows at the rate shown. Medians; the best of a row in bold.', '']
    out += md_table(['{:,} orders'.format(t['rows'])] + [UPD_LABELS[e] for e in engines], rows) + ['']
    out += ['- ' + x for x in upd_sentences(R, tier, engines)]
    loss = upd_losses(R, tier, engines)
    if loss:
        out += ['- **Where pg_sift loses:** ' + '; '.join(loss) + '.']
    out += ['- Method, the update path of each system and why, the first-run and latency spread tables, and the official pages: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#updates-and-deletes).']
    return out


def updates_benchmarks(R):
    tier = upd_tier(R)
    if not tier:
        return []
    engines = upd_engines(R, tier)
    t = R.meta['tiers'][tier]
    part = t['parts']['updates']
    out = [f'Measured on the {t["rows"]:,}-order table of the section above, {part["date"]}, {host_noise(part)} at the start, after the read tests (the data is changed from here on). '
           f'Same containers, same limits. Each system changes rows through the path its documentation recommends for the job:', '']
    out += md_table(['system', 'how the rows are changed'], [[UPD_LABELS[e], UPD_PATH[e]] for e in engines], ['---', '---']) + ['']
    out += ['Steps, in order: update 1% of the rows (`id % 100 = 0`: status becomes `refunded`, amount drops by 1), update 10% (`id % 10 = 1`), delete 5% (`id % 20 = 3`), '
            f'{t.get("probes", "?")} single-row changes timed until a query sees them (status `probe`, seen by `count(*) WHERE status = \'probe\'`), the revenue-per-month query timed for '
            f'{t.get("load_seconds", "?")} s under 0, 1,000 and 10,000 updated rows per second (a second connection sends batches of 100 rows, `amount = amount + 1`, ids drawn from the live rows), '
            'then a maintenance step per system. After each of the first three steps the state (rows and sum of the amount per status) is compared with PostgreSQL\'s; the run stops on a difference. '
            'Rows per second is the rows of the step over the wall time until the last statement was acknowledged.', '']
    out += md_table(['{:,} orders'.format(t['rows'])] + [UPD_LABELS[e] for e in engines], upd_rows(R, tier, engines, True)) + ['']
    out += ['The three reads, before the changes, after them (before maintenance) and after the maintenance step (milliseconds, median of the runs):', '']
    out += md_table(['query'] + [UPD_LABELS[e] for e in engines], upd_read_rows(R, tier, engines)) + ['']
    errs = [(e, R.get(tier, e, '-', 'run', 'error')) for e in engines]
    errs = [(e, r['value']) for e, r in errs if r]
    errs += [(e, R.get(tier, e, LOAD_QUERY, f'load {r}/s', 'read_error')['value']) for e in engines for r in (1000, 10000) if R.get(tier, e, LOAD_QUERY, f'load {r}/s', 'read_error')]
    errs += [(e, R.get(tier, e, '-', f'load {r}/s', 'update_error')['value']) for e in engines for r in (1000, 10000) if R.get(tier, e, '-', f'load {r}/s', 'update_error')]
    if errs:
        out += ['Errors the systems raised during the run (kept as results):', ''] + [f'- {UPD_LABELS[e]}: {m}' for e, m in errs] + ['']
    out += ['Where each system\'s recommended path comes from:', '']
    for name, links in UPD_DOCS:
        out += [f'- {name}: ' + '; '.join(f'[{label}]({url})' for label, url in links)]
    out += ['', 'What the paths mean for the numbers: ClickHouse documents lightweight `UPDATE` for frequent small changes (up to about 10% of a table) and `ALTER ... UPDATE` mutations for infrequent large ones, '
            'and says `ReplacingMergeTree` needs `FINAL` at read time and cannot change sorting-key values (`status` and `amount` are not in this table\'s key for that reason); Elasticsearch reindexes the whole document on every update '
            'and shows it to searches at the next refresh. The rows above are what those paths did on this table; they say nothing about other sizes, hardware or concurrency.']
    return out


# --- the markers of README.md and docs/BENCHMARKS.md

# --- compression

CZ_LANES = ['sift_none', 'sift_enc', 'sift_auto', 'sift', 'sift_lz4', 'sift_zstd']
CZ_NAMES = {'sift_none': '`none`', 'sift_enc': 'encodings only', 'sift_auto': '`auto` (explicit)', 'sift': 'unset (the default)', 'sift_lz4': '`lz4`', 'sift_zstd': '`zstd:3`'}


def compression_block(R):
    tier = R.tiers[-1]
    n = R.meta['tiers'][tier]['rows']
    s = R.meta['tiers'][tier]['servers']
    lanes = [e for e in CZ_LANES if R.engines(tier, [e])]
    sz = lambda e, v='index orders_col': R.num(tier, e, variant=v, metric='size_mib')
    leaves = {e: {l['leaf']: l for l in json.loads(R.get(tier, e, '-', 'orders_col', 'col_leaves')['value'])} for e in lanes}
    out = [f'The same {n:,} orders; every line is read back from the server that ran it. pg_sift {s["sift_sift"]} in the container image is linked with `{s["sift_libs"]}` '
           f'(the image installs the LZ4 and zstd development packages and the Makefile links each library it finds), `sift.table_compression` is `{s["sift_table_compression"]}`, and the `sift_col` index '
           f'created without the `compression` option has `reloptions` {R.get(tier, "sift", "-", "orders_col", "reloptions")["value"]}. Index = the `sift_col` index alone; build = its `CREATE INDEX`.', '']
    head = ['`compression` of the `sift_col` index', '`reloptions`', 'index size', 'build time', 'vs `none`']
    body = [[CZ_NAMES[e], f'`{R.get(tier, e, "-", "orders_col", "reloptions")["value"]}`', f'{sz(e):,.1f} MiB', f'{R.num(tier, e, variant="build orders_col", metric="build_s"):.1f} s',
             f'{sz(e) / sz("sift_none"):.2f}x'] for e in lanes]
    out += md_table(head, body, ['---', '---', '---:', '---:', '---:']) + ['']
    out += ['Chunk bytes per column of the index (`sift.col_leaves`; every chunk of every column is written by the column codec, none stayed in the older formats):', '']
    names = [l['leaf'] for l in leaves[lanes[0]].values()]
    body = [[nm] + [f'{next(l for l in leaves[e].values() if l["leaf"] == nm)["bytes"] / 1048576:.2f}' for e in lanes] for nm in names]
    out += md_table(['column (MiB)'] + [CZ_NAMES[e] for e in lanes], body) + ['']
    if 'sift_auto' in lanes and 'sift' in lanes:
        same = all(leaves['sift'][k]['bytes'] == leaves['sift_auto'][k]['bytes'] for k in leaves['sift'])
        enc = 'sift_enc' in lanes and all(leaves['sift'][k]['bytes'] == leaves['sift_enc'][k]['bytes'] for k in leaves['sift'])
        lz = 'sift_lz4' in lanes and all(leaves['sift'][k]['bytes'] == leaves['sift_lz4'][k]['bytes'] for k in leaves['sift'])
        out += [f'- The default is `auto`: the chunk bytes of every column are {"identical" if same else "NOT identical"} with an explicit `compression = \'auto\'`, and {sz("sift"):,.1f} MiB against {sz("sift_none"):,.1f} MiB for `none`. '
                f'`auto` wrote the same bytes as the encodings alone ({"identical" if enc else "different"}) and as `lz4` ({"identical" if lz else "different"}) on every column here, so it layered no LZ4 or zstd over the encodings '
                '(the cost rule of [docs/perf/COMPRESSION.md](perf/COMPRESSION.md) accepts a layer only when its decode time is worth the bytes); `zstd:3` shrinks the text and low-cardinality columns further. '
                'No SQL function reports the general-purpose layer chunk by chunk (`sift.col_leaves` counts chunks by encoding class), so the layer is read from these byte counts.', '']
    tbl = [(p, R.num(tier, 'sift_table', variant='codec ' + p, metric='size_mib'), R.num(tier, 'sift_table', variant='codec ' + p + ' free', metric='size_mib')) for p in ('none', 'auto', 'lz4', 'zstd:3')]
    others = [['pg_sift `sift_table`, `sift.table_compression` = ' + p, size(v, f)] for p, v, f in tbl if v is not None]
    others += [[f'ClickHouse, {k}', size(R.num(tier, 'ch', variant=f'codec {k}', metric='size_mib'))] for k in ('default (LZ4)', 'ZSTD(3)') if R.num(tier, 'ch', variant=f'codec {k}', metric='size_mib') is not None]
    others += [[f'Elasticsearch, {k}', size(R.num(tier, 'es', variant=f'codec {k}', metric='size_mib') if k != 'default (LZ4)' else R.num(tier, 'es', metric='size_mib'))]
               for k in ('default (LZ4)', 'best_compression') if R.engines(tier, ['es'])]
    others += [['PostgreSQL (heap, btree, GIN)', size(R.num(tier, 'pg', metric='size_mib'))]] if R.engines(tier, ['pg']) else []
    others += [[f'Citus columnar, default settings ({citus_options(R, tier)}), primary key', size(R.num(tier, 'citus', metric='size_mib'))]] if R.engines(tier, ['citus']) else []
    out += ['Size of the other layouts and systems with their own compression settings (table and indexes):', ''] + md_table(['system and setting', 'size on disk'], others, ['---', '---:']) + ['']
    qs = R.queries(tier)
    cols = [e for e in lanes] + [e for e in ('pg', 'citus', 'ch', 'es') if R.engines(tier, [e])]
    labels = {**{e: 'sift_col ' + CZ_NAMES[e] for e in lanes}, 'pg': 'PostgreSQL', 'citus': 'Citus columnar', 'ch': 'ClickHouse', 'es': 'Elasticsearch'}
    body = [[QUERY_TITLES[q]] + best_bold([R.num(tier, e, q, 'warm', 'latency_ms') for e in cols], [ms(R.num(tier, e, q, 'warm', 'latency_ms')) for e in cols]) for q in qs]
    spread = [max(v) / min(v) for q in qs if len(v := [R.num(tier, e, q, 'warm', 'latency_ms') for e in lanes]) and all(v)]
    out += [f'Warm medians (ms) of the ten questions on each `compression` variant of the `sift_col` index, with the other systems of the same run (the slowest of the {len(lanes)} variants against the fastest, per question: '
            f'{min(spread):.2f}x to {max(spread):.2f}x; the order of the variants changes from question to question, so differences of that size are run-to-run noise):', ''] + md_table(['question'] + [labels[e] for e in cols], body)
    return out


def replace_blocks(args):
    import readme  # steps/readme.py: replace_block
    R = Results(args.results)
    if args.readme:
        readme.replace_block(args.readme, 'store', readme_block(R))
        readme.replace_block(args.readme, 'store-updates', updates_readme(R))
    if args.benchmarks:
        readme.replace_block(args.benchmarks, 'store', benchmarks_block(R))
        readme.replace_block(args.benchmarks, 'store-updates', updates_benchmarks(R))
        if os.path.exists(args.compression):
            readme.replace_block(args.benchmarks, 'store-compression', compression_block(Results(args.compression)))


def cmd_render(args):
    if args.stdout:
        R = Results(args.results)
        print('\n'.join(readme_block(R) + [''] + updates_readme(R) + [''] + benchmarks_block(R) + [''] + updates_benchmarks(R)))
    else:
        replace_blocks(args)
    return 0


def cmd_stats(args):
    """what ClickHouse and Elasticsearch read and spend per question, from their own accounting (system.query_log, `took`) and clickhouse-client --time"""
    ch, es = Http('ch', CH_AUTH), Http('es_cached')
    out = dict(rows=args.rows, ch={}, es={})
    for q in load_queries(args.queries):
        sql, tag = q.statement('ch', args.rows), f'gap-{q.id}'
        for _ in range(args.runs):
            ch.request('POST', f'/?database=store&default_format=TSV&log_comment={tag}', sql.encode())
        ch.request('POST', '/', b'SYSTEM FLUSH LOGS')
        cols = ("read_rows, read_bytes, memory_usage, query_duration_ms, peak_threads_usage, ProfileEvents['RealTimeMicroseconds'] AS real_us, "
                "ProfileEvents['UserTimeMicroseconds'] AS user_us, ProfileEvents['SelectedParts'] AS parts, ProfileEvents['SelectedMarks'] AS marks")
        rows = [json.loads(l) for l in ch.request('POST', f"/?default_format=JSONEachRow&output_format_json_quote_64bit_integers=0", (
            f"SELECT {cols} FROM system.query_log WHERE type = 'QueryFinish' AND log_comment = '{tag}' ORDER BY event_time_microseconds DESC LIMIT {args.runs}").encode()).decode().splitlines()]
        cli = subprocess.run(['docker', 'exec', container('ch'), 'clickhouse-client', '--user', 'bench', '--password', 'bench', '--database', 'store', '--time', '--format', 'Null', '--query', sql],
                             capture_output=True, text=True)
        if cli.returncode:
            raise RuntimeError('clickhouse-client: ' + cli.stderr.strip()[:300])
        out['ch'][q.id] = dict(median={k: statistics.median(r[k] for r in rows) for k in rows[0]}, client_time_s=float(cli.stderr.strip().splitlines()[-1]))
        method, path, body, _, _ = q.statement('es_cached', args.rows)
        took = [json.loads(es.request(method, path.replace('request_cache=true', 'request_cache=false'), body))['took'] for _ in range(args.runs)]
        out['es'][q.id] = dict(took_ms=statistics.median(took))
        log(f'{q.id} {q.name}: clickhouse {out["ch"][q.id]["median"]}; elasticsearch took {out["es"][q.id]["took_ms"]} ms')
    with open(args.o, 'w') as f:
        json.dump(out, f, indent=1)
        f.write('\n')
    log(f'written: {args.o}')
    return 0


def all_lanes(R, tier):
    return R.engines(tier, list(LANES))


def set_title(R, tier):
    return f'{R.meta["tiers"][tier]["rows"]:,} orders' + (f', {cpu_name(R.cpus(tier))}' if R.cpus(tier) else '')


def cmd_table(args):
    """the warm medians of every result set (size and thread budget), as a Markdown table (stdout)"""
    R = Results(args.file)
    for tier in R.all_tiers:
        engines = all_lanes(R, tier)
        head, body = bench_table(R, tier, engines, {e: LANES[e][0] for e in LANES})
        print(f'\n{set_title(R, tier)}, warm median ms\n')
        print('\n'.join(md_table(head, body)))
    return 0


def cmd_compare(args):
    """two result files side by side: the warm median of every question and system in each result set both have, and the ratio B / A (below 1: B is faster)"""
    A, B = Results(args.a), Results(args.b)
    body = []
    for tier in [t for t in A.all_tiers if t in B.all_tiers]:
        for q in A.queries(tier):
            for e in all_lanes(A, tier):
                a, b = (X.num(tier, e, q, 'warm', 'latency_ms') for X in (A, B))
                if a and b:
                    body.append([tier, q, QUERY_TITLES[q][:44], LANES[e][0], ms(a), ms(b), f'{b / a:.2f}' + (' *' if abs(b / a - 1) > 0.1 else '')])
    print('\n'.join(md_table(['set', 'q', 'question', 'system', 'A ms', 'B ms', 'B / A'], body, ['---'] * 4 + ['---:'] * 3)))
    print('\n* changed by more than 10%')
    return 0


def gap_rows(R, tier):
    """one row per question: the warm medians of sift_col, sift_table, PostgreSQL, ClickHouse and Elasticsearch (request cache off, on), and sift_col / ClickHouse, sift_col / Elasticsearch"""
    out = []
    for q in R.queries(tier):
        v = {e: R.num(tier, e, q, 'warm', 'latency_ms') for e in ('sift', 'sift_table', 'pg', 'ch', 'es', 'es_cached')}
        rel = lambda e: f'**{v["sift"] / v[e]:.2f}**' if v['sift'] and v[e] and v['sift'] > v[e] else f'{v["sift"] / v[e]:.2f}' if v['sift'] and v[e] else 'n/a'
        out.append([q, QUERY_TITLES[q], *(ms(v[e]) for e in v), rel('ch'), rel('es'), rel('es_cached')])
    return out


def gap_table(R):
    head = ['q', 'question', 'sift_col', 'sift_table', 'PostgreSQL', 'ClickHouse', 'Elasticsearch', 'ES, cache on', 'sift_col / ClickHouse', 'sift_col / ES', 'sift_col / ES cache on']
    out = []
    for tier in R.all_tiers:
        out += [f'#### {set_title(R, tier)} (warm median, ms; a ratio above 1, in bold, means pg_sift is slower)', '']
        out += md_table(head, gap_rows(R, tier), ['---:', '---'] + ['---:'] * 9) + ['']
    return out


def cmd_gap(args):
    print('\n'.join(gap_table(Results(args.file))))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)

    def common(p):
        p.add_argument('--rows', type=int)
        p.add_argument('--tier', default='')
        p.add_argument('--cpus', type=int, default=int(os.environ.get('STORE_CPUS', 3)), help='the thread budget N of every engine and the CPU quota of the containers; the result set of a size is `<size>-c<N>`')
        p.add_argument('--data', default=os.environ.get('STORE_DATA', '/tmp/sift-store-data'))
        p.add_argument('--out', default='.')
        p.add_argument('--engines', type=lambda s: s.split(','))
        p.add_argument('--queries', type=lambda s: s.split(','), default=[])
        p.add_argument('--runs', type=int, default=7)
        p.add_argument('--warmups', type=int, default=3, help='untimed runs per query and engine before the timed ones (the first one is the checked result)')
        p.add_argument('--cold-runs', type=int, default=3)
        p.add_argument('--load-seconds', type=int, default=int(os.environ.get('STORE_LOAD_SECONDS', 15)), help='updates: how long a query is timed under each update rate')
        p.add_argument('--probes', type=int, default=20, help='updates: single-row changes timed until a query sees them')
        p.add_argument('--compression', action='store_true', help='load: also the size of each system with its other compression settings')
        p.add_argument('--keep-going', action='store_true', help='go on after a result that differs from PostgreSQL\'s (the exit status is still 3)')
    for name in ('gen', 'load', 'run', 'cold', 'updates', 'verify'):
        common(sub.add_parser(name))
    m = sub.add_parser('merge')
    m.add_argument('files', nargs='+')
    m.add_argument('-o', required=True)
    r = sub.add_parser('render')
    r.add_argument('--results', default=os.path.join(HERE, '..', '..', 'bench', 'results', 'store.json'))
    r.add_argument('--readme')
    r.add_argument('--benchmarks')
    r.add_argument('--compression', default=os.path.join(HERE, '..', '..', 'bench', 'results', 'store-compression.json'))
    r.add_argument('--stdout', action='store_true')
    for name in ('table', 'gap'):
        sub.add_parser(name).add_argument('file', nargs='?', default=os.path.join(HERE, '..', '..', 'bench', 'results', 'store.json'))
    t = sub.add_parser('stats')
    t.add_argument('--rows', type=int, required=True)
    t.add_argument('-o', required=True)
    t.add_argument('--runs', type=int, default=3)
    t.add_argument('--queries', type=lambda s: s.split(','), default=[])
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    args = ap.parse_args()
    if args.cmd in ('gen', 'load', 'run', 'cold', 'updates', 'verify') and not args.engines:
        args.engines = UPDATE_ENGINES if args.cmd == 'updates' else DEFAULT_ENGINES
    if getattr(args, 'rows', None) and not args.tier:
        args.tier = (f'{args.rows // 1000000}M' if args.rows % 1000000 == 0 else f'{args.rows // 1000}k') + f'-c{args.cpus}'
    try:
        sys.exit({'gen': cmd_gen, 'verify': cmd_verify, 'load': cmd_load, 'run': cmd_run, 'cold': cmd_cold, 'updates': cmd_updates, 'merge': cmd_merge, 'render': cmd_render, 'table': cmd_table, 'stats': cmd_stats, 'gap': cmd_gap, 'compare': cmd_compare}[args.cmd](args))
    except RuntimeError as x:
        die(str(x))


if __name__ == '__main__':
    main()
