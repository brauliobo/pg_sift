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
import argparse, csv, functools, glob, hashlib, http.client, inspect, itertools, json, math, os, queue, re, statistics, subprocess, sys, threading, time, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from stats import number, summary  # noqa: E402  (steps/stats.py)
import benchlib  # noqa: E402  (steps/benchlib.py: host fingerprint, cell cache, row replacement)
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


START_PRESSURE = quiet('report')[1]                       # the pressure when this command started
NOISY = []                                                # the cells whose window (still, after a second measurement) had other work in it: Window.judge

COMPRESSION = {'sift_none': 'none', 'sift_enc': 'for+delta+rle+dict+scale+alp', 'sift_auto': 'auto', 'sift_lz4': 'lz4', 'sift_zstd': 'zstd:3'}   # the `compression` option of the sift_col index, one database each

# id -> (label, container, port offset, database or kind)
LANES = {
    'pg':         ('PostgreSQL', 'pg', 0, 'store'),
    'sift':       ('pg_sift `sift_col`', 'sift', 1, 'store_col'),
    'sift_table': ('pg_sift `sift_table`', 'sift', 1, 'store_tbl'),
    'es':         ('Elasticsearch', 'es', 2, None),
    'es_cached':  ('Elasticsearch, request cache on', 'es', 2, None),
    'ch':         ('ClickHouse', 'ch', 3, None),
    'ch_mut':     ('ClickHouse, ALTER UPDATE', 'ch', 3, None),
    'ch_rmt':     ('ClickHouse, ReplacingMergeTree', 'ch', 3, None),
    'citus':      ('Citus columnar', 'citus', 4, 'store'),
    **{e: (f'pg_sift `sift_col`, compression `{p}`', 'sift', 1, 'store_col_' + e[5:]) for e, p in COMPRESSION.items()},
}
DEFAULT_ENGINES = ['pg', 'sift', 'sift_table', 'es', 'es_cached', 'ch', 'citus']
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


@functools.lru_cache(maxsize=None)
def container_cgroup(eid):
    """cgroup directory of the container of the engine (the id does not change when the container restarts)"""
    return f'/sys/fs/cgroup/docker/{docker("inspect", "-f", "{{.Id}}", container(eid)).strip()}'


def cgroup_usage(path):
    """CPU seconds the container has used so far (a restarted container starts again at 0)"""
    return float(open(path + '/cpu.stat').readline().split()[1]) / 1e6


class Client:
    """one engine: run(statement, capture) -> (milliseconds, text or None)"""
    def __init__(self, eid):
        self.eid, self.busy, self.cpu = eid, 0.0, 0.0   # busy, cpu: wall seconds and container CPU seconds spent in this client's requests since the last reset
        self.cg = container_cgroup(eid)
        self.psql = Psql(eid) if eid in PG_ENGINES else None
        self.http = None if self.psql else Http(eid, CH_AUTH if is_ch(eid) else {'Content-Type': 'application/json'})

    def run(self, st, capture=False):
        u0 = cgroup_usage(self.cg)
        t0 = time.perf_counter()
        try:
            if self.psql:
                out = self.psql.run(st, capture)
                return (time.perf_counter() - t0) * 1e3, ('\n'.join(out) + '\n' if out else '') if capture else None
            if is_ch(self.eid):
                data = self.http.request('POST', '/?database=store&default_format=TSV', st.encode())
                return (time.perf_counter() - t0) * 1e3, data.decode() if capture else None
            method, path, body, spec, _ = st
            data = self.http.request(method, path, body)
            return (time.perf_counter() - t0) * 1e3, es_text(data, spec) if capture else None
        finally:
            self.busy += time.perf_counter() - t0
            self.cpu += cgroup_usage(self.cg) - u0

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


# --- external noise: what the host did during a timed window that was not the benchmark's own work
# The pressure stall information cannot tell the benchmark's own loads, restarts and neighbouring cells from the host's other users, so it is not used to
# decide anything (it is logged). A window is measured instead: the CPU the host spent, less the CPU of the containers of this run and of this process
# tree, is the work of somebody else (`external_cpu`, cores); the same for the disk of the volume the containers live on (`external_io`, MiB/s).

CLK = os.sysconf('SC_CLK_TCK')
NOISE_CPU = float(os.environ.get('SIFT_NOISE_CPU', 0.25))   # cores of other work above which a window is NOISY (and at least 0.1 CPU seconds: /proc/stat counts in ticks)
NOISE_IO = float(os.environ.get('SIFT_NOISE_IO', 2))        # MiB/s of other disk traffic on the volume of the containers above which it is (and at least 1 MiB)
OTHER_CPU = float(os.environ.get('SIFT_OTHER_CPU', 0.1))    # cores a container that is not under test may use during a window
MIN_WINDOW = 0.5                                             # seconds: the shortest window that is judged
BASE = dict(cpu=0.0, io=0.0)                                 # the other work of the host with the containers idle, measured when a stage starts (cores, MiB/s): a host that always has background jobs is judged by what they add


def host_busy():
    v = list(map(int, open('/proc/stat').readline().split()[1:9]))
    return (v[0] + v[1] + v[2] + v[5] + v[6] + v[7]) / CLK   # user nice system irq softirq steal (vm is in user); idle and iowait are not work


DOCKER_COMM = ('dockerd', 'containerd', 'containerd-shim', 'runc', 'docker-proxy')   # the daemon and what it starts: what a container start, stop or restart costs outside its cgroup


def own_cpu():
    """CPU seconds of this process and of every process below it (psql clients, docker), the children already reaped included, and of the docker daemon and its shims"""
    parent, cpu, mine = {}, {}, {os.getpid()}
    for d in filter(str.isdigit, os.listdir('/proc')):
        try:
            f = open(f'/proc/{d}/stat').read()
        except OSError:
            continue
        r = f[f.rindex(')') + 2:].split()   # state ppid ... utime stime cutime cstime
        parent[int(d)], cpu[int(d)] = int(r[1]), sum(map(int, r[11:15]))
        if f[f.index('(') + 1:f.rindex(')')].startswith(DOCKER_COMM):
            mine.add(int(d))
    grew = True
    while grew:
        grew = False
        for pid, pp in parent.items():
            if pp in mine and pid not in mine:
                mine.add(pid)
                grew = True
    return sum(cpu.get(pid, 0) for pid in mine) / CLK


def cgroups():
    """cgroup directory of every container of this run, by container name"""
    out = docker('ps', '--no-trunc', '--filter', f'label=sift-store={NAME}', '--format', '{{.ID}} {{.Names}}')
    return {n: f'/sys/fs/cgroup/docker/{i}' for i, n in map(str.split, out.splitlines())}


@functools.lru_cache(maxsize=None)
def volume_device():
    """the kernel name of the block device under docker's data directory, None when it cannot be told"""
    try:
        root = docker('info', '-f', '{{.DockerRootDir}}').strip()
        fstype, source = subprocess.run(['findmnt', '-no', 'FSTYPE,SOURCE', '-T', root], capture_output=True, text=True, check=True).stdout.split()[:2]
        if fstype == 'zfs':
            dev = next(l.split()[0] for l in subprocess.run(['zpool', 'status', '-P', source.split('/')[0]], capture_output=True, text=True, check=True).stdout.splitlines() if l.split() and l.split()[0].startswith('/dev/'))
            return os.path.basename(os.path.realpath(dev))
        st = os.stat(root).st_dev
        return os.path.basename(os.path.realpath(f'/sys/dev/block/{os.major(st)}:{os.minor(st)}'))
    except (OSError, StopIteration, subprocess.CalledProcessError, ValueError):
        return None


def disk_mib():
    for l in open('/proc/diskstats') if volume_device() else ():
        p = l.split()
        if p[2] == volume_device():
            return (int(p[5]) + int(p[9])) * 512 / 1048576
    return 0.0


class Utilization:
    """how much of the wall time of a command the host spent measuring: the seconds inside timed windows (the Window objects), a sample of the busy cores every second (/proc/stat)
    and the share of the wall time with fewer busy cores than `low`. A pass that waits (restarts, cache drops, loads, gates) between short windows shows it here"""
    def __init__(self, low):
        self.t0, self.timed, self.low, self.samples, self.stop = time.perf_counter(), 0.0, low, [], threading.Event()
        threading.Thread(target=self.sample, daemon=True).start()

    def sample(self):
        t, busy = time.perf_counter(), host_busy()
        while not self.stop.wait(1.0):
            now, b = time.perf_counter(), host_busy()
            self.samples.append((b - busy) / (now - t))
            t, busy = now, b

    def report(self):
        wall = time.perf_counter() - self.t0
        s = self.samples or [0.0]
        return dict(wall_s=round(wall, 1), timed_s=round(self.timed, 1), timed_pct=round(100 * self.timed / wall, 1) if wall else 0.0, busy_cores_mean=round(sum(s) / len(s), 2),
                    low_busy_pct=round(100 * sum(1 for x in s if x < self.low) / len(s), 1), low_busy_below=self.low)


UTIL = None   # set in main() for the commands that measure


class Window:
    """the numbers of one timed window: its length, the CPU of every container, the CPU and the disk traffic nobody of this run made"""
    def __init__(self):
        self.s = 0.0
        self.cont, self.ext_cpu, self.ext_io = {}, 0.0, 0.0

    def snap(self):
        return dict(t=time.perf_counter(), busy=host_busy(), own=own_cpu(), io=disk_mib(), cont={n: float(open(p + '/cpu.stat').readline().split()[1]) / 1e6 for n, p in self.cg.items()})

    def __enter__(self):
        self.cg = cgroups()
        self.a = self.snap()
        return self

    def __exit__(self, *exc):
        a, b = self.a, self.snap()
        self.s += b['t'] - a['t']
        if UTIL:
            UTIL.timed += b['t'] - a['t']
        used = {n: (lambda d: d if d >= 0 else b['cont'][n])(b['cont'].get(n, a['cont'][n]) - a['cont'][n]) for n in a['cont']}   # a restarted container has a new cgroup whose counters start at 0
        for n, u in used.items():
            self.cont[n] = self.cont.get(n, 0.0) + u
        self.ext_cpu += (b['busy'] - a['busy']) - sum(used.values()) - (b['own'] - a['own'])
        self.ext_io += b['io'] - a['io']

    def add(self, x):
        self.s, self.ext_cpu, self.ext_io = self.s + x.s, self.ext_cpu + x.ext_cpu, self.ext_io + x.ext_io
        for n, v in x.cont.items():
            self.cont[n] = self.cont.get(n, 0.0) + v

    def judge(self, under=(), io=True):
        """why the window is NOISY ('' when it is clean): other work above the thresholds, or a container that is not under test (`under`) and not idle"""
        why, cpu, mib = [], self.ext_cpu - BASE['cpu'] * self.s, self.ext_io - BASE['io'] * self.s
        if self.s < MIN_WINDOW:   # /proc/stat counts in ticks of 10 ms: a shorter window says nothing about the host (its numbers are recorded, it is not judged)
            return ''
        if cpu > 0.1 and cpu / self.s > NOISE_CPU:
            why.append(f'{cpu / self.s:.2f} cores of other work above the {BASE["cpu"]:.2f} of the idle host')
        if io and mib > 1 and mib / self.s > NOISE_IO:
            why.append(f'{mib / self.s:.1f} MiB/s of other disk traffic above the {BASE["io"]:.1f} of the idle host')
        busy = [f'{n[len(NAME) + 1:]} {c / self.s:.2f} cores' for n, c in self.cont.items() if n not in under and under and c / self.s > OTHER_CPU]
        if busy:
            why.append('still busy: ' + ', '.join(busy))
        return '; '.join(why)

    def record(self, rows, cls, variant, why):
        rows.add('-', cls, variant, 'window_s', 's', value=round(self.s, 3))
        rows.add('-', cls, variant, 'external_cpu', 'cores', value=round(max(self.ext_cpu, 0) / self.s, 3))   # all of it, the idle host's own (meta `baseline`) included
        rows.add('-', cls, variant, 'external_io', 'MiB/s', value=round(max(self.ext_io, 0) / self.s, 2))
        if why:
            rows.add('-', cls, variant, 'noisy', 'text', value=why)
            log(f'{cls} {variant}: NOISY, {why} ({quiet("report")[1]}, for information)')
        return why


def quiet_gate(what):
    """the one wait for a quiet host of a stage, before it loads or times anything (pressure stall information, 10%, at most SIFT_QUIET_TIMEOUT s); only logged, the cells are judged by their windows"""
    rc, msg = quiet('wait')
    if rc:
        log(f'{what}: the host was not quiet when the stage started ({msg}; {quiet("report")[1]})')


def gate(what, engines):
    """quiet_gate, the containers of the engines done merging, and the other work the idle host has (BASE: the window's own measurement with nothing of the run in it, STORE_BASELINE_SEC seconds,
    10 by default; one measured less than 15 minutes ago in this pass (STORE_BASELINE, a file) is used again: a job that started since shows as NOISY against it, never as quiet)"""
    quiet_gate(what)
    busy = wait_idle(engines)
    if busy:
        log(f'{what}: still busy: {busy}')
    path = os.environ.get('STORE_BASELINE')
    try:
        with open(path) as f:
            old = json.load(f)
        if time.time() - old['time'] < 900:
            BASE.update(cpu=old['cpu'], io=old['io'])
            log(f'{what}: the idle host has {BASE["cpu"]} cores and {BASE["io"]} MiB/s of other work (measured {int(time.time() - old["time"])} s ago)')
            return
    except (TypeError, OSError, ValueError, KeyError):
        pass
    with Window() as w:
        time.sleep(float(os.environ.get('STORE_BASELINE_SEC', 10)))
    BASE.update(cpu=round(w.ext_cpu / w.s, 3), io=round(w.ext_io / w.s, 2))
    if path:
        benchlib.write_json(path, dict(BASE, time=time.time()))
    log(f'{what}: the idle host has {BASE["cpu"]} cores and {BASE["io"]} MiB/s of other work (the cells are judged by what they add to it)')


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
    h.request('PUT', '/orders', read('es.json').replace('"number_of_shards": 1', f'"number_of_shards": {args.cpus}').encode(), js)   # one shard per CPU of the budget the load runs at: one shard is searched by one thread, whatever node.processors says
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
    quiet_gate('load')   # the one wait for a quiet host of the stage: nothing is waited for between the cells
    keys, reused = dict(kv.split('=') for kv in args.keys), {}
    for c in args.reuse:   # the data of these containers is already in their volume (same key): their load times and sizes are those of the load that put it there, dated
        m = json.load(open(marker_path(args.data, c, keys[c])))
        rows.rows += [dict(r, tier=args.tier) for r in m['rows']]
        reused[c] = m['date']
        log(f'{c}: the loaded data is reused (loaded {m["date"]}, key {keys[c]})')
    for eid in args.engines:
        if eid == 'es_cached' or LANES[eid][1] in args.reuse:
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
    for c, key in keys.items():   # what is loaded now stays in the volume: a pass that finds the same key there loads nothing
        lanes = [e for e in args.engines if LANES[e][1] == c and e != 'es_cached']
        if c not in reused and lanes:
            benchlib.write_json(marker_path(args.data, c, key), dict(date=benchlib.now_iso(), key=key, rows=[r for r in rows.rows if r['engine'] in lanes]))
    busy = wait_idle(args.engines)
    log(f'the servers are idle after the load' if not busy else f'still busy after the load (the timed batches wait again): {busy}')
    args.threads = effective(args)
    write(args, rows, 'load', {'reused': reused})
    return 0


def marker_path(data, container_, key):
    """the record of a load that finished: its rows and date, next to the CSV; the data volume of the container under the same key holds the data it describes"""
    return os.path.join(data, 'loaded', f'{container_}-{key}.json')


LOAD_CODE = None   # set in main(): the functions that decide what a loaded server holds


def cmd_datakey(args):
    """one line per container: `CONTAINER KEY VOLUME reuse|new`. The key says what its data volume holds: the seeded rows, the image, the table definitions, the load code and settings;
    `reuse`: a volume of that key exists and the load that filled it finished (store.sh keeps the volume under this name and mounts it, and loads nothing)"""
    images = {c: image_ids(c)[1] for c in ('pg', 'citus', 'es', 'ch')}
    images['sift'] = docker('image', 'inspect', '-f', '{{.Id}}', f'{NAME}-pg:latest').strip()   # PostgreSQL with pg_sift built from this checkout
    for c in args.containers:
        lanes = [e for e in args.engines if LANES[e][1] == c and e != 'es_cached']
        files = sorted({DDL[e] for e in lanes if e in DDL} | {{'ch': 'ch.sql', 'es': 'es.json'}.get(c, '')} - {''})
        key = benchlib.key_of(dict(container=c, lanes=lanes, image=images[c], dataset=dataset_id(args), files={f: benchlib.key_of(read(f), 12) for f in files}, code=LOAD_CODE,
                                   server_flags=os.environ.get('STORE_PG_FLAGS', '') if c in ('pg', 'sift', 'citus') else '', shards=args.cpus if c == 'es' else 0, compression=args.compression), 12)
        vol = f'{NAME}-data-{c}-{key}'
        have_volume = subprocess.run(['docker', 'volume', 'inspect', vol], capture_output=True).returncode == 0
        print(c, key, vol, 'reuse' if have_volume and os.path.exists(marker_path(args.data, c, key)) else 'new')
    return 0


PARTS = {'run': 'warm', 'cold': 'cold', 'updates': 'updates'}   # the command and the part file it writes


def part_path(args, part):
    return os.path.join(args.out, f'store-{args.tier}-{part}.json')


def part_key(args, part):
    """what a part file's numbers depend on: the code, the host, the data and every setting of the command (a part with this key is not measured again by `resume`)"""
    return benchlib.key_of(dict(part=part, code=MEASURE_CODE, fp=host_fp()['id'], data=dataset_id(args), rows=args.rows, cpus=args.cpus, engines=args.engines, untimed=args.untimed,
                                queries=args.queries, runs=args.runs, warmups=args.warmups, cold=args.cold_runs, restarts=args.restart_runs, check=args.cold_check, rates=args.rates,
                                load=args.load_seconds, probes=args.probes, parallel=args.parallel), 16)


def part_done(args):
    """whether the part file of this command exists and holds this command's key: its cells were measured before and are kept"""
    path = part_path(args, PARTS[args.cmd])
    try:
        with open(path) as f:
            return json.load(f)['meta'].get('key') == part_key(args, PARTS[args.cmd])
    except (OSError, ValueError):
        return False


def write(args, rows, part, extra=None):
    os.makedirs(args.out, exist_ok=True)
    path = part_path(args, part)
    rows.rows = [r for r in rows.rows if r['engine'] not in args.untimed]   # the reference of the result checks is not a result of this pass
    util = UTIL.report() if UTIL else None
    if util and part != 'load' and util['wall_s'] >= 60 and util['timed_pct'] < 70:
        log(f'WARN: {part}: timed windows are {util["timed_pct"]}% of the {util["wall_s"]} s of the command (below 70%): the rest was waiting for restarts, cache drops, gates; '
            f'{util["low_busy_pct"]}% of the time fewer than {util["low_busy_below"]} cores were busy')
    meta = dict(key=part_key(args, part), date=time.strftime('%FT%TZ', time.gmtime()), baseline=BASE, utilization=util, parallel=getattr(args, 'parallel', 1), tier=args.tier, rows=args.rows, cpus=args.cpus, threads=args.threads, data_sha256=open(csv_path(args) + '.stamp').read().split()[2], loadavg=START_LOAD, loadavg_end=open('/proc/loadavg').read().split()[:3], pressure=START_PRESSURE, noisy=NOISY, **(extra or {}))
    benchlib.write_text(path, '{"meta": ' + json.dumps(meta) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows.rows) + '\n]}\n')
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


def measure_query(q, clients, args, rows):
    """the cell of one question on every engine, in a window whose external noise is recorded; (the results that differ, why the window was NOISY)"""
    for c in clients.values():
        c.busy = c.cpu = 0.0
    with Window() as w:
        stop = run_query(q, clients, args, rows)
    for e, c in clients.items():   # the cores the container used while this client's requests ran: its CPU over the wall time of those requests (not the window: the other engines are asked in it)
        cores = c.cpu / c.busy if c.busy and e not in args.untimed else None
        quota = args.cpus * 1.05
        if cores is not None and cores > quota:
            log(f'{q.id} {e}: {cores:.2f} cores in the requests, more than the quota of {args.cpus}: not recorded')
        elif cores is not None:
            rows.add(e, q.id, 'warm', 'cpu_cores', 'cores', value=round(cores, 2))
    return stop, w.record(rows, q.id, 'warm', w.judge({container(e) for e in clients}, io=True))


def one_container(lanes, stmts, clients, args, times):
    """the untimed warm-ups, then the timed runs of the lanes of one container, the first lane rotating every run (the lanes of one container share its CPUs, so they never run together)"""
    for _ in range(args.warmups - 1):
        for e in lanes:
            clients[e].run(stmts[e])
    for i in range(args.runs):
        for e in rotated(lanes, i):
            times[e].append(clients[e].run(stmts[e])[0])


def time_cell(live, stmts, clients, args):
    """the timed runs of one question: one engine at a time, the engines interleaved run by run (the method of the published tables), or with --parallel-engines N the containers in
    waves of N at the same time, each on cores of its own (time_wave)"""
    times = {e: [] for e in live}
    boxes = list(dict.fromkeys(container(e) for e in live))
    if args.parallel <= 1 or len(boxes) < 2:
        for _ in range(args.warmups - 1):
            for e in live:
                clients[e].run(stmts[e])
        for i in range(args.runs):
            for e in rotated(live, i):
                times[e].append(clients[e].run(stmts[e])[0])
        return times
    cores = physical_cores()
    n = min(args.parallel, benchlib.max_parallel(cores, args.cpus))
    boxes = rotated(boxes, args.cell)   # which container is in which wave moves from question to question
    for w in range(0, len(boxes), n):
        time_wave(boxes[w:w + n], live, stmts, clients, args, times, cores, args.cell + w)
    return times


@functools.lru_cache(maxsize=None)
def physical_cores():
    return benchlib.physical_cores()


def pin(box, cores):
    """docker --cpuset-cpus: the container runs on these CPUs only; None frees it again (all CPUs of the host)"""
    docker('update', '--cpuset-cpus', ','.join(map(str, cores)) if cores else f'0-{os.cpu_count() - 1}', box)


def time_wave(boxes, live, stmts, clients, args, times, cores, rotation):
    """the containers of a wave run their timed requests at the same time, each on `--cpus` physical cores of its own (equal sets, moved round the cores from wave to wave so that no engine
    always gets the same ones; the other container threads of the host are not pinned). The cores are freed again afterwards"""
    sets, errors = benchlib.cpuset_plan(cores, args.cpus, len(boxes), rotation), []

    def work(box):
        try:
            one_container([e for e in live if container(e) == box], stmts, clients, args, times)
        except Exception as x:
            errors.append(x)

    try:
        for box, s in zip(boxes, sets):
            pin(box, s)
        threads = [threading.Thread(target=work, args=(box,)) for box in boxes]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        for box in boxes:
            pin(box, None)
    if errors:
        raise errors[0]


def run_query(q, clients, args, rows):
    stmts = {e: q.statement(e, args.rows, e == 'es_cached') for e in clients}
    out, bad = {}, {}
    for e, c in clients.items():   # the first run of each engine: its result is checked, the plan is read (an untimed engine is only the reference: no rows of it)
        try:
            out[e] = c.run(stmts[e], True)[1]
            plan = c.explain(stmts[e])
            if plan is not None and e not in args.untimed:
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
        if e not in args.untimed:
            rows.add(e, q.id, '-', 'result', 'check', value=verdict)
    for e, msg in bad.items():
        rows.add(e, q.id, '-', 'result', 'check', value='error: ' + msg)
    live = [e for e in clients if e in out and e not in args.untimed]
    times = time_cell(live, stmts, clients, args)
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
    gate('run', args.engines)
    noisy, again = {}, []
    try:
        for i, q in enumerate(queries):
            args.cell = i
            stop, why = measure_query(q, clients, args, rows)
            problems += stop
            if why:
                noisy[q.id] = why
            if problems and not args.keep_going:
                break
        for q in [q for q in queries if q.id in noisy and not problems][:args.max_rerun]:   # a NOISY cell is measured again once, at the end of the stage, not waited on
            log(f'{q.id}: NOISY ({noisy[q.id]}), measured again')
            rows.rows = [r for r in rows.rows if r['class'] != q.id]
            args.cell = queries.index(q) + 1   # another wave order than the first time
            stop, why = measure_query(q, clients, args, rows)
            problems += stop
            again.append(q.id)
            if why:
                NOISY.append(f'{q.id}: {why}')
    finally:
        for c in clients.values():
            c.close()
        write(args, rows, 'warm', {'servers': servers(args.engines), 'runs': args.runs, 'warmups': args.warmups, 'rerun': again})
    for p in problems:
        print('DIFFERENT RESULT: ' + p, file=sys.stderr)
    return 3 if problems else 0


COLD_HOW = {'es': 'restart'}   # emptying the memory of Elasticsearch is not as cold as a restart, so its cold cells restart the container


def evict(e):
    """what a restart empties, without one: the memory the server keeps its data and caches in. The operating system's page cache stays, as after a restart (dropping it needs root)"""
    if e in PG_ENGINES:   # every shared buffer, from the maintenance database (a new backend starts each cell: no relation, catalog or plan caches)
        psql_value('pg' if e == 'pg' else e, 'SELECT pg_buffercache_evict_all()', 'postgres')
    elif is_ch(e):        # the mark, uncompressed-block, primary-key and skipping-index caches; the parts stay attached, as after a restart they are loaded again
        for what in ('MARK CACHE', 'UNCOMPRESSED CACHE', 'PRIMARY INDEX CACHE', 'INDEX MARK CACHE', 'INDEX UNCOMPRESSED CACHE', 'QUERY CACHE'):
            ask('ch', 'POST', '/', f'SYSTEM DROP {what}'.encode())
    else:                 # the node's caches, and the shards' readers (terms dictionaries, doc value and points structures) closed and opened again
        ask('es', 'POST', '/orders/_cache/clear')
        ask('es', 'POST', '/orders/_close')
        ask('es', 'POST', '/orders/_open?wait_for_active_shards=all')
        ask('es', 'GET', '/_cluster/health/orders?wait_for_status=green&timeout=60s')


def cold_cell(e, q, stmt, args, how='evict'):
    """the first run of a question after the server's memory was emptied (`evict`: its caches and buffers, `--cold-runs` times; `restart`: the container, `--restart-runs` times:
    a restart takes seconds, so it has a count of its own), each on a new connection: (the times, the windows of the runs added up); no times when the engine fails"""
    ts, w = [], Window()
    for _ in range(args.restart_runs if how == 'restart' else args.cold_runs):
        c = None
        try:
            with Window() as x:   # the emptying and the first run: the request alone is a few milliseconds, too short for the host's counters
                restart(e) if how == 'restart' else evict(e)
                c = Client(e)
                ts.append(c.run(stmt)[0])
            w.add(x)
        except RuntimeError as err:   # an error is a result, as in the warm run
            log(f'cold {e} {q.id}: {err}')
            return None, w
        finally:
            if c:
                c.close()
    return ts, w


def med(xs):
    return sorted(xs)[len(xs) // 2]


def cold_check(e, q, stmt, how, args, rows, cache):
    """the check that emptying the memory is the cold of a restart: the same question the other way. It is not run unless asked for (--cold-check), and its result is kept in the cache once
    per version of the engine (the image the container runs): (milliseconds of the other way, the date it was measured when it was read from the cache)"""
    other = 'restart' if how == 'evict' else 'evict'
    inputs = dict(engine=e, image=docker('inspect', '-f', '{{.Image}}', container(e)).strip(), rows=args.rows, cpus=args.cpus, question=q.id, how=other, runs=args.restart_runs if other == 'restart' else args.cold_runs)
    hit, why = cache.lookup('check', e, inputs) if cache else (None, '')
    if hit:
        rows.rows += hit['rows']
        return med([r for r in hit['rows'] if r['metric'] == 'latency_ms'][0]['values']), hit['date']
    ts, _ = cold_cell(e, q, stmt, args, other)
    if not ts:
        return None, None
    mine = Rows(args.tier)
    mine.add(e, q.id, 'cold-alt', 'latency_ms', 'ms', st=summary(ts))
    rows.rows += mine.rows
    if cache:
        cache.put('check', e, inputs, dict(rows=mine.rows))
    return med(ts), None


def cmd_cold(args):
    rows = Rows(args.tier)
    queries = load_queries(args.queries)
    engines = [e for e in args.engines if e != 'es_cached' and e not in args.untimed]
    stmts = {(q.id, e): q.statement(e, args.rows) for q in queries for e in engines}
    how = {e: COLD_HOW.get(e, 'evict') for e in engines}
    effective(args)
    for e in engines:   # one restart of each server, then every cell empties its memory without one (a server whose cells restart it needs no restart before)
        if how[e] != 'restart':
            restart(e)
    for e in engines:
        if e in PG_ENGINES:
            psql_value(e, 'CREATE EXTENSION IF NOT EXISTS pg_buffercache', 'postgres')
    gate('cold', args.engines)
    cells = [(e, q) for e in engines for q in queries]
    got, again = {}, []
    for e in engines:
        rows.add(e, '-', 'cold', 'cold_how', 'text', value=how[e])
    for e, q in cells:
        got[e, q.id] = cold_cell(e, q, stmts[q.id, e], args, how[e])
        if got[e, q.id][0]:
            log(f'cold {e} {q.id} {q.name}: {med(got[e, q.id][0]):.3g} ms')
    for e, q in cells:   # a NOISY cell is measured again once, at the end of the stage, not waited on (at most --max-rerun of them: a restart cell takes a minute)
        why = got[e, q.id][1].judge({container(e)})
        if got[e, q.id][0] and why and len(again) < args.max_rerun:
            log(f'cold {e} {q.id}: NOISY ({why}), measured again')
            got[e, q.id] = cold_cell(e, q, stmts[q.id, e], args, how[e])
            again.append(f'{e} {q.id}')
    for e, q in cells:
        ts, w = got[e, q.id]
        if not ts:
            continue
        why = w.record(rows, q.id, f'cold-{e}', w.judge({container(e)}))
        if why:
            NOISY.append(f'cold {e} {q.id}: {why}')
        rows.add(e, q.id, 'cold', 'latency_ms', 'ms', st=summary(ts))
    checked = {}
    for q in [q for q in queries if q.id in args.cold_check]:
        for e in engines:
            alt, date = cold_check(e, q, stmts[q.id, e], how[e], args, rows, args.cache)
            if alt is not None:
                checked[f'{e} {q.id}'] = date or 'measured'
                log(f'cold check {e} {q.id}: {alt:.3g} ms ({"restart" if how[e] == "evict" else "evict"}{", from the cache " + date if date else ""}), {med(got[e, q.id][0]):.3g} ms ({how[e]})')
    args.threads = effective(args)   # after the restarts: the settings are the container's, not the process's
    write(args, rows, 'cold', {'cold_runs': args.cold_runs, 'restart_runs': args.restart_runs, 'rerun': again, 'cold_check': args.cold_check, 'cold_check_dates': checked})
    return 0


def cmd_interference(args):
    """Does running engines at the same time change what one of them measures? The requests of the target are timed (1) unpinned and alone, the method of the published tables, (2) pinned to
    `--cpus` physical cores of its own and alone, and (3) pinned while `--concurrent` other containers loop the same question on disjoint cores of their own. Per question the median and p95
    of each, and the change of (3) against (2): what the neighbours' memory bandwidth and L3 traffic cost"""
    target, queries = args.target, load_queries(args.queries)
    others = [e for e in args.engines if container(e) != container(target) and e != 'es_cached'][:args.concurrent]
    boxes, cores = [container(target)] + [container(e) for e in others], physical_cores()
    clients = {e: Client(e) for e in [target] + others}
    effective(args)
    gate('interference', [target] + others)
    out = []

    def sample(stmt):
        for _ in range(args.warmups):
            clients[target].run(stmt)
        return [clients[target].run(stmt)[0] for _ in range(args.samples)]

    stats = lambda xs: dict(median=statistics.median(xs), p95=percentile(xs, 95), n=len(xs))
    for qi, q in enumerate(queries):
        stmts = {e: q.statement(e, args.rows) for e in clients}
        for e in clients:
            clients[e].run(stmts[e], True)
        for b in boxes:
            pin(b, None)
        free = stats(sample(stmts[target]))
        for b, s in zip(boxes, benchlib.cpuset_plan(cores, args.cpus, len(boxes), qi)):   # the target's cores move from question to question
            pin(b, s)
        stop = threading.Event()
        loops = [threading.Thread(target=lambda e=e: [clients[e].run(stmts[e]) for _ in iter(stop.is_set, True)]) for e in others]
        try:
            alone = stats(sample(stmts[target]))
            for t in loops:
                t.start()
            time.sleep(0.5)
            together = stats(sample(stmts[target]))
        finally:
            stop.set()
            for t in loops:
                if t.is_alive():
                    t.join()
            for b in boxes:
                pin(b, None)
        out.append(dict(question=q.id, title=q.title, unpinned=free, pinned=alone, concurrent=together, median_change=together['median'] / alone['median'] - 1, p95_change=together['p95'] / alone['p95'] - 1,
                        pinning_change=alone['median'] / free['median'] - 1))
        log(f'{q.id} {q.name}: {target} unpinned {free["median"]:.3g} ms, pinned {alone["median"]:.3g} ms, with {len(others)} others {together["median"]:.3g} ms '
            f'(median {out[-1]["median_change"]:+.1%}, p95 {out[-1]["p95_change"]:+.1%})')
    worst = max(abs(r['median_change']) for r in out)
    log(f'{target} with {" ".join(others)} running at the same time on cores of their own: the median changed by at most {worst:.1%}, the p95 by at most {max(abs(r["p95_change"]) for r in out):.1%}')
    benchlib.write_json(os.path.join(args.out, f'interference-{target}-{args.tier}.json'), dict(target=target, others=others, cpus=args.cpus, rows=args.rows, samples=args.samples, cells=out,
                                                                                            date=benchlib.now_iso(), host=host_fp(), threshold=args.threshold, ok=worst <= args.threshold))
    return 0 if worst <= args.threshold else 1


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
    reference = eid in args.untimed   # PostgreSQL when its numbers are read from the cache: it changes the rows (the state the others are compared with) and times nothing
    qs = {q.id: q for q in load_queries(LOAD_QUERIES)}
    n = args.rows
    for q in [] if reference else qs.values():
        read_latency(w, q, args, rows, 'baseline')
    point = lambda v: (rows.add(eid, '-', v, 'size_mib', 'MiB', value=w.size_mib()), w.free_mib() is not None and rows.add(eid, '-', v, 'free_mib', 'MiB', value=w.free_mib()))
    if not reference:
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
    if reference:
        w.close()
        return
    read_after_write(w, args, rows)
    if eid != 'ch_mut':
        for rate in args.rates:
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
    for c in {LANES[e][1] for e in args.engines}:   # the rows are about to change: what these containers hold is no longer what a load of the same key leaves
        for f in glob.glob(marker_path(args.data, c, '*')):
            os.remove(f)
    gate('updates', args.engines)
    prepare_copies(args.engines)
    try:
        for eid in [e for e in UPDATE_ENGINES if e in args.engines]:
            log(f'updates on {eid}')
            try:
                with Window() as w:
                    update_engine(eid, args, rows, ref)
                why = w.record(rows, '-', f'updates-{eid}', w.judge({container(eid)}, io=False))   # its own writes are the disk traffic; the CPU of the others is judged
                if why:
                    NOISY.append(f'updates {eid}: {why}')
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


def host_fp():
    return benchlib.host_fingerprint(docker_version=docker('version', '-f', '{{.Server.Version}}').strip())


def host():
    """the fingerprint of this host (steps/benchlib.py) and the budgets of the run"""
    return dict(host_fp(), cpus=os.environ.get('STORE_CPUS', '?'), cpus_list=os.environ.get('STORE_CPUS_LIST', '?'), memory=os.environ.get('STORE_MEMORY', '?'))


# --- the cache of the cells of the engines that are not under test (steps/benchlib.py has the rules)

def make_cache(args):
    return None if args.no_cache else benchlib.Cache(args.cache_dir, args.fingerprint, args.cache_max_age)


def image_ids(group):
    """(tag, image id) of the image of a server; pulled when it is not there, so that the key does not change with the first pull"""
    tag = os.environ[{'pg': 'STORE_PG_IMAGE', 'citus': 'STORE_CITUS_IMAGE', 'es': 'STORE_ES_IMAGE', 'ch': 'STORE_CH_IMAGE'}[group]]
    if subprocess.run(['docker', 'image', 'inspect', tag], capture_output=True).returncode:
        docker('pull', '-q', tag)
    return tag, docker('image', 'inspect', '-f', '{{.Id}}', tag).strip()


def dataset_id(args):
    """the seeded data is identified by what generates it: the rows, the generator and the image of the server it runs on"""
    return dict(rows=args.rows, generator=benchlib.key_of(read('gen.sql'), 12), image=image_ids('pg')[1])


MEASURE_CODE = None   # set in main(): the functions whose behaviour a cached number depends on


def group_inputs(group, lanes, args):
    """everything a cached cell of the group depends on: the key of its entry is the hash of this. Not the pg_sift lanes (they are measured every pass)"""
    tag, iid = image_ids(group)
    qs = load_queries(args.queries)
    method = dict(runs=args.runs, warmups=args.warmups, cold_runs=args.cold_runs, restart_runs=args.restart_runs, cold_check=args.cold_check, parts=args.parts, points=args.points, load_cpus=args.cpus, parallel=args.parallel)
    if 'updates' in args.parts:
        method['updates'] = dict(load_seconds=args.load_seconds, probes=args.probes, rates=args.rates, phases=[p[0] for p in PHASES], set=UPDATE_SET)
    files = sorted({DDL[e] for e in lanes if e in DDL} | {{'ch': 'ch.sql', 'es': 'es.json'}.get(group, '')} - {''})
    return dict(group=group, lanes=lanes, image=tag, image_id=iid, dataset=dataset_id(args), memory=os.environ.get('STORE_MEMORY', ''),
                server_flags=os.environ.get('STORE_PG_FLAGS', '') if group in ('pg', 'citus') else '', statements={e: benchlib.key_of([q.statement(e, args.rows, e == 'es_cached') for q in qs], 12) for e in lanes},
                files={f: benchlib.key_of(read(f), 12) for f in files}, code=MEASURE_CODE, method=method)


def cmd_plan(args):
    """which lanes this pass measures and which it reads from the cache (--remeasure-all: none), written to plan.json; the lanes that are measured include PostgreSQL as the (untimed)
    reference of the result checks. Prints the decision of every server"""
    cache, hits, groups = make_cache(args), {}, {}
    every = list(dict.fromkeys(args.engines + args.update_engines))
    for g in dict.fromkeys(benchlib.group_of(e) for e in every):
        lanes = [e for e in every if benchlib.group_of(e) == g]
        why = ('pg_sift: measured every pass' if not benchlib.cacheable(g) else 'the cache is off (--remeasure-all)' if args.remeasure_all or not cache else 'named in --remeasure' if g in args.remeasure
               else 'compression variants: sizes are measured' if args.compression else '')
        inputs = group_inputs(g, lanes, args) if benchlib.cacheable(g) else {}
        entry = None
        if not why:
            entry, why = cache.lookup('cells', g, inputs)
        groups[g] = dict(lanes=lanes, status='cached' if entry else 'measure', inputs=inputs, path=cache.path('cells', g, inputs) if entry else None)
        if entry:
            hits[g] = entry
            log(f'plan {g}: read from the cache, measured {entry["date"]}, key {entry["key"][:8]}, host {entry["fp"]}')
        else:
            log(f'plan {g}: measured in this pass ({why})')
    run, untimed = benchlib.plan_engines(args.engines, hits)
    upd, _ = benchlib.plan_engines(args.update_engines, hits)
    benchlib.write_json(args.plan, dict(groups=groups, run=run, untimed=untimed, update=upd, fingerprint=args.fingerprint))
    log(f'plan: lanes {" ".join(run) or "none"}' + (f' (reference only: {" ".join(untimed)})' if untimed else '') + f'; updates {" ".join(upd) or "none"}; written {args.plan}')
    return 0


def cmd_planget(args):
    """one field of plan.json as a comma separated list (run, untimed, update, cached, measured)"""
    p = json.load(open(args.plan))
    print(','.join({'cached': [g for g, v in p['groups'].items() if v['status'] == 'cached'], 'measured': [g for g, v in p['groups'].items() if v['status'] == 'measure']}.get(args.field) or p.get(args.field, [])))
    return 0


def sub_meta(group, meta):
    """the part of a result set's servers and threads that describes one group's servers"""
    return ({k: v for k, v in meta.get('servers', {}).items() if k == group or k.startswith(group + '_')}, {c: v for c, v in meta.get('threads', {}).items() if c == LANES[group][1]})


def cmd_cache_save(args):
    """put the cells of the cacheable servers this pass measured into the cache (not those with a window that was still NOISY after the second measurement)"""
    plan, merged, cache = json.load(open(args.plan)), json.load(open(args.results)), make_cache(args)
    for g, v in plan['groups'].items():
        if v['status'] != 'measure' or not cache or not benchlib.cacheable(g):
            continue
        rows = [r for r in merged['rows'] if benchlib.group_of(r['engine']) == g]
        if not rows:
            continue
        if any(r['metric'] == 'noisy' for r in rows):
            log(f'cache {g}: not kept, a window was still NOISY after the second measurement')
            continue
        tiers = {}
        for t in sorted({r['tier'] for r in rows}):
            tm = merged['meta']['tiers'][t]
            servers, threads = sub_meta(g, tm)
            tiers[t] = dict(meta={k: x for k, x in tm.items() if k not in ('parts', 'servers', 'threads', 'cached')}, parts=tm['parts'], servers=servers, threads=threads)
        log(f'cache {g}: kept {cache.put("cells", g, v["inputs"], dict(rows=rows, tiers=tiers))}')
    return 0


def restart_cold(args, metas, rows):
    """cold cells of some engines taken from another results file, whose cells restarted the server for every run: for the engines where emptying the memory is not as cold
    (the check of `cold`), the numbers of this run's emptied cells stay as the check (`cold-alt`) of the questions it was made for"""
    d = json.load(open(args.restart_cold))
    engines = args.restart_engines
    for tier in {r['tier'] for r in d['rows'] if r['variant'] == 'cold'} & {t for t, m in metas.items() if 'cold' in m['parts']}:   # the sets this run has cold cells for
        part = d['meta']['tiers'][tier]['parts']['cold']
        src = [r for r in d['rows'] if r['tier'] == tier and r['engine'] in engines and r['variant'] == 'cold' and r['metric'] == 'latency_ms']
        keep = []
        for r in rows:
            if r['tier'] == tier and r['engine'] in engines and r['variant'] == 'cold' and r['metric'] == 'latency_ms':
                if r['class'] in metas[tier].get('cold_check', []):
                    r = dict(r, variant='cold-alt')
                else:
                    continue
            if r['tier'] == tier and r['engine'] == '-' and r['variant'].startswith('cold-') and r['variant'][5:] in engines:
                continue
            if r['tier'] == tier and r['metric'] == 'cold_how' and r['engine'] in engines:
                continue
            keep.append(r)
        rows = keep + src + [dict(dataset='store', tier=tier, engine=e, **{'class': '-'}, variant='cold', metric='cold_how', unit='text', value='restart', min='restart', max='restart', stderr=None, n=1, values=['restart']) for e in engines]
        metas[tier]['restart_cold'] = dict(engines=engines, date=part['date'], runs=d['meta']['tiers'][tier]['cold_runs'], noisy=[n for n in part['noisy'] if n.split()[1] in engines])
    return rows


def cmd_merge(args):
    """the files of the parts of a pass (store-<set>-<part>.json), the entries of the cache its plan read, and the sets of an earlier results file (--keep) that this pass did not
    measure again, into one results file. Cells of another host are never mixed in"""
    metas, rows, fp = {}, [], args.fingerprint
    for path in args.files:
        d = json.load(open(path))
        tier = d['meta']['tier']
        metas.setdefault(tier, {}).update({k: v for k, v in d['meta'].items() if k not in ('date', 'loadavg', 'loadavg_end', 'pressure', 'noisy', 'baseline', 'rerun', 'utilization')})
        metas[tier].setdefault('parts', {})[os.path.basename(path).split('-')[-1][:-5]] = dict(date=d['meta']['date'], loadavg=d['meta']['loadavg'], loadavg_end=d['meta'].get('loadavg_end'), pressure=d['meta'].get('pressure'), noisy=d['meta'].get('noisy', []), baseline=d['meta'].get('baseline'), rerun=d['meta'].get('rerun', []), utilization=d['meta'].get('utilization'))
        metas[tier].pop('loadavg_end', None)
        rows += d['rows']
    entries = [json.load(open(v['path'])) for plan in args.plan for v in json.load(open(plan))['groups'].values() if v['status'] == 'cached']
    benchlib.check_homogeneous(entries, fp)
    for e in entries:   # what was measured in this pass wins; a column read from the cache says when and under which key it was measured
        rows += e['rows']
        for tier, t in e['tiers'].items():
            m = metas.setdefault(tier, dict(t['meta'], parts={}))
            for k in ('servers', 'threads'):
                m[k] = {**t[k], **m.get(k, {})}
            m.setdefault('cached', {})[e['name']] = dict(date=e['date'], key=e['key'], fp=e['fp'], parts={p: x['date'] for p, x in t['parts'].items()})
    for keep in args.keep:
        old = json.load(open(keep))
        if not benchlib.same_host(old['meta'].get('host', {}), fp):
            log(f'WARN {keep} was measured on another host ({old["meta"].get("host", {}).get("cpu", "?")}, {old["meta"].get("host", {}).get("kernel", "?")}): its sets are not carried over')
            continue
        for tier, t in old['meta']['tiers'].items():
            if tier not in metas and not any(t['rows'] == m['rows'] and t.get('cpus') == m.get('cpus') for m in metas.values()):
                metas[tier] = t
                rows += [r for r in old['rows'] if r['tier'] == tier]
    if args.restart_cold:
        rows = restart_cold(args, metas, rows)
    with open(args.o, 'w') as f:
        f.write('{"meta": ' + json.dumps(dict(tiers=metas, host=host())) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows) + '\n]}\n')
    log(f'written: {args.o}')
    return 0


def noisy_cells(meta):
    """the cells a part file reports as still NOISY: ('warm', question) / ('cold', engine, question) / ('updates', engine), from the `noisy` list of its meta"""
    out = []
    for s in meta.get('noisy', []):
        m = re.match(r'(?:(cold) (\w+) (\d+)|(updates) (\w+)|(\d+)): ', s)
        if m:
            out.append(('cold', m[2], m[3]) if m[1] else ('updates', m[5]) if m[4] else ('warm', m[6]))
    return out


def cmd_noisy(args):
    """the cells of a part file that are still NOISY, one per line: `warm QUESTION`, `cold ENGINE QUESTION`, `updates ENGINE` (bench/bench final measures them again)"""
    for c in noisy_cells(json.load(open(args.file))['meta']):
        print(' '.join(c))
    return 0


def cmd_patch(args):
    """replace in a part file the cells a second file measured again (same sets, engines and questions), and drop them from its list of NOISY cells"""
    old, new = json.load(open(args.file)), json.load(open(args.fresh))

    def measured_again(s):   # a NOISY entry of the old file whose cell the new file holds
        c = noisy_cells({'noisy': [s]})
        return bool(c) and any(r['class'] == c[0][1] if c[0][0] == 'warm' else (r['engine'], r['class']) == c[0][1:] for r in new['rows'] if c[0][0] != 'updates')

    old['rows'] = benchlib.replace_rows(old['rows'], new['rows'])
    old['meta']['noisy'] = [s for s in old['meta'].get('noisy', []) if not measured_again(s)] + new['meta'].get('noisy', [])
    old['meta']['rerun'] = sorted(set(old['meta'].get('rerun', [])) | set(new['meta'].get('rerun', [])) | {'patched'})
    benchlib.write_json(args.file, old)
    log(f'patched {args.file} with {args.fresh}')
    return 0


HEADLINE = [('sift', 'pg'), ('sift', 'citus'), ('sift', 'es'), ('sift', 'ch'), ('sift_table', 'pg')]   # the claims of the README: pg_sift against each system


def warm_by_cpus(path):
    """{cpus: {(engine, question): warm median}} of a results file"""
    R = Results(path)
    return {R.cpus(t): {(e, q): R.num(t, e, q, 'warm', 'latency_ms') for e in all_lanes(R, t) if e != 'es_cached' for q in R.queries(t)} for t in R.all_tiers}


def cmd_stable(args):
    """do two results files (two sizes) tell the same story? Per thread budget both have: the ratios of pg_sift to each system on every question must stay within a factor of
    1 + --tol, and no pair separated by more than --sep in both may change order. Exit status 1 when not (the ladder of presets is checked with this)"""
    a, b, bad = warm_by_cpus(args.a), warm_by_cpus(args.b), []
    for c in sorted(set(a) & set(b)):
        r = benchlib.stable({k: v for k, v in a[c].items() if v}, {k: v for k, v in b[c].items() if v}, args.tol, args.sep, HEADLINE)
        print(f'\n{c} CPUs: {"stable" if r["ok"] else "NOT stable"} (ratios within {1 + args.tol:g}x, order flips beyond {args.sep:g}x counted)')
        for q, i in r['queries'].items():
            print(f'  {q} {QUERY_TITLES.get(q, "")[:40]:40} pairs {i["pairs"]} flips {i["flips"]} worst drift {i["worst"][0]:.2f}x ({i["worst"][1]}/{i["worst"][2]})')
        bad += r['violations']
    return 1 if bad or not set(a) & set(b) else 0


# --- the Markdown tables

class Results:
    def __init__(self, path):
        d = json.load(open(path))
        self.meta, self.rows = d['meta'], d['rows']
        tm = self.meta['tiers']
        self.all_tiers = sorted(tm, key=lambda t: (tm[t]['rows'], self.cpus(t)))   # a result set per size and thread budget (`1M-c3`; older results: one per size)
        self.tiers = [t for t in self.all_tiers if 'load' in tm[t]['parts']]       # one per size: the set that holds the load, the plans and the updates
        self.sets = {t: [t] for t in self.tiers}   # published: the set of the load (3 CPUs, the budget of the search tables); the other budgets stay in the results file (`all_tiers`)

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


HEAP = ('pg', 'sift')   # the systems whose table is a heap; the others hold the rows in their own format


def size_split(R, tier, e):
    """MiB of a system: (heap and primary key, [the parts of the index or columnar data]); None for the first where there is no such part (the system holds everything itself)"""
    idx = {r['variant'][6:]: r['value'] for r in R.rows if (r['tier'], r['engine'], r['metric']) == (tier, e, 'size_mib') and r['variant'].startswith('index ')}
    table, free = R.num(tier, e, variant='table', metric='size_mib'), R.num(tier, e, variant='free', metric='size_mib') or 0
    if table is None:
        return None, [R.num(tier, e, metric='size_mib')]
    pk = idx.pop('orders_pkey')
    return (table + pk, list(idx.values())) if e in HEAP else (pk, [table - free, *idx.values()])


def parts_cell(parts):
    return f'{sum(parts):.1f} MiB' + (' (' + ' + '.join(f'{p:.1f}' for p in parts) + ')') * (len(parts) > 1)


def size_rows(R, tier, engines):
    """the three size rows: heap and primary key, index or columnar data, total on disk (the free blocks inside the file and what is live)"""
    split = [size_split(R, tier, e) for e in engines]
    disk = [R.num(tier, e, metric='size_mib') for e in engines]
    free = [R.num(tier, e, variant='free', metric='size_mib') for e in engines]
    total = [size(v) + (f' ({f:,.0f} free, {v - f:,.0f} live)' if f else '') for v, f in zip(disk, free)]
    return [['size: heap and primary key'] + [f'{h:.1f} MiB' if h else 'n/a' for h, _ in split],
            ['size: index or columnar data (without heap and primary key)'] + best_bold([sum(p) for _, p in split], [parts_cell(p) for _, p in split]),
            ['size on disk, total (free blocks inside the file in parentheses)'] + best_bold(disk, total)]


LIKE_FOR_LIKE = ('the seven columns of the `sift_col` index (the table without the title) take 9.6 MiB against 14.6 MiB for ClickHouse with its default `LZ4`, 10.4 MiB with `ZSTD(3)` and 9.3 MiB with a hand-tuned schema '
                 '(`system.columns`, measured on 2026-10-18; per column in [{where}]({page}#size-against-clickhouse-1000000-orders))')


def size_note(R, tier, engines, page='docs/BENCHMARKS.md'):
    """what the size rows add up (the parts are read from the rows of the tier); page: where the link to the per-column table starts from"""
    sp = {e: size_split(R, tier, e) for e in engines}
    mib = lambda e: f'{sum(sp[e][1]):.1f} MiB'
    plus = lambda e: ' + '.join(f'{p:.1f}' for p in sp[e][1])
    own = [LANES[e][0] for e in ('sift_table', 'es', 'ch', 'citus') if e in engines]
    return ('- **The size rows.** *Heap and primary key* is what the table itself costs: the heap and the primary key btree of PostgreSQL and `sift_col`; `sift_table` and Citus hold the rows in their own format, so it is the primary key alone, '
            'and Elasticsearch and ClickHouse have no such part. *Index or columnar data* is everything else: ' + '; '.join(
                ([f'PostgreSQL its secondary indexes (the btrees on the date and on country and status, the GIN on the title; {mib("pg")})'] * ('pg' in engines)
                 + [f'`sift_col` the column index and the BM25 title index ({plus("sift")} MiB)'] * ('sift' in engines)
                 + [f'`sift_table` the live bytes of its columns plus the title index ({plus("sift_table")} MiB)'] * ('sift_table' in engines)
                 + ['Elasticsearch the index'] * ('es' in engines) + ['ClickHouse the table with its text index'] * ('ch' in engines) + ['Citus the columnar table'] * ('citus' in engines)))
            + f'. {join_and(own)} store the data themselves, so there that row is the data, not an index in the PostgreSQL sense. The total is the file sizes (the parts add up to it within rounding); the free blocks of `sift_table` are inside its file after the load. '
            f'Like for like, {LIKE_FOR_LIKE.format(page=page, where='docs/BENCHMARKS.md' if page else 'Size against ClickHouse')}.')


def load_rows(R, tier, engines):
    load = [R.num(tier, e, metric='load_s') for e in engines]
    return [['load time (data, indexes, merge)'] + best_bold(load, [seconds(v) for v in load]), *size_rows(R, tier, engines)]


QUERY_TITLES = {q.id: q.title for q in load_queries()}
QUERY_LABELS = {q.id: q.label for q in load_queries()}
SHOWN = ['pg', 'sift', 'sift_table', 'es', 'ch', 'citus']   # the columns of the README table, in order; a system without rows in the results has no column


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


def dagger(R, tier, labels):
    """the labels of the systems whose cells in this result set were read from the cache get a dagger"""
    cached = R.meta['tiers'][tier].get('cached', {})
    return {e: lb + (' †' if benchlib.group_of(e) in cached else '') for e, lb in labels.items()}


def cache_note(R, tier):
    """one sentence for the systems whose cells were read from the cache: when and under which key each was measured, on which host, and when the rest was"""
    cached = R.meta['tiers'][tier].get('cached', {})
    if not cached:
        return []
    host_ = R.meta.get('host', {})
    when = ', '.join(f'{LANES[g][0]} {v["date"][:10]} (key `{v["key"][:8]}`)' for g, v in cached.items())
    done = R.meta['tiers'][tier]['parts'].get('warm', {}).get('date', '?')[:10]
    return [f'† Read from the cache of earlier passes, not measured in this one: {when}. Same host ({host_.get("cpu", "?")}, kernel {host_.get("kernel", "?")}, fingerprint `{host_.get("id", "?")}`), same data, '
            f'images, settings, statements and runs (the key); the other columns were measured {done}.']


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
    host = R.meta.get('host', {})
    n = f'{t["rows"]:,}'
    fair = ' and the same thread budget for every engine (a [fair comparison](docs/BENCHMARKS.md#fair-comparison-the-same-threads-for-every-engine))'
    out = [f'An online store: a table of {n} orders (country, category, status, amount, date, a product title) and ten questions anyone can ask of it. Every system holds the same seeded rows, '
           f'runs one node in a docker container limited to {cpu_name(R.cpus(tier))} (a quota, no pinning), the same {cpu_name(R.cpus(tier))} per server as the search tables, and {host.get("memory", "?")} of memory{fair}, and answers over a local connection from one client. '
           f'Cells are the median of {t["runs"]} warm runs in milliseconds (lower is better; the fastest of a row, and anything within 5% of it, in bold). '
           f'Measured {t["parts"]["warm"]["date"][:10]} on {host.get("cpu", "?")} ({host.get("threads", "?")} threads), PostgreSQL {s["pg"]}, pg_sift {s["sift_sift"]}' + (f', Citus {s["citus_columnar"]} columnar' if 'citus' in engines else '') + f', Elasticsearch {s["es"]}, ClickHouse {s["ch"]}.', '']
    head, body = bench_table(R, tier, engines, dagger(R, tier, labels))
    out += [f'**{set_heading(R, tier)}**', ''] + md_table(head, body + load_rows(R, tier, engines)) + ['']
    rd = R.get(tier, 'es', '06', '-', 'rel_diff')
    approx = f' (HyperLogLog++: its monthly counts were off by at most {rd["value"]:.2%} here)' if rd else ''
    out += [size_note(R, tier, engines), f'- Every result is compared with PostgreSQL\'s before it is timed, and the run stops on a difference. Elasticsearch counts distinct customers approximately{approx}; every other result matched exactly or within rounding.',
            f'- Elasticsearch runs with its request cache off (a repeated aggregation would otherwise be answered from memory); with it on, the medians are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md#analytics-an-online-store).',
            '- ClickHouse is a column store built for exactly these aggregations and Elasticsearch for exactly this search; PostgreSQL (heap, btree and GIN indexes) is what most teams run today.']
    out += ['- ' + n for n in cache_note(R, tier) + citus_notes(R, tier) + load_notes(R, tier)]
    plain = [q for q in R.queries(tier) if 'Seq Scan' in (R.get(tier, 'sift_table', q, '-', 'plan') or {}).get('value', '') and R.num(tier, 'sift_table', q, 'warm', 'latency_ms') and R.num(tier, 'pg', q, 'warm', 'latency_ms')]
    if plain and 'sift_table' in engines:
        ratios = [R.num(tier, 'sift_table', q, 'warm', 'latency_ms') / R.num(tier, 'pg', q, 'warm', 'latency_ms') for q in plain]
        out += [f'- `sift_table` ran {len(plain)} of the {len(R.queries(tier))} questions with a plain `Seq Scan` plan (no aggregate pushdown: [docs/USAGE.md](docs/USAGE.md#limits) lists what `SiftAgg` does not read) and took '
                f'{" to ".join(dict.fromkeys([ratio(min(ratios), 1), ratio(max(ratios), 1)]))} the time of PostgreSQL\'s heap on those; the other layout, `sift_col`, is an index over the ordinary table.']
    lo = losses(R, tier, engines, labels)
    if lo:
        out += ['- **Where pg_sift is not the fastest:** ' + '; '.join(f'{QUERY_LABELS[qid]}: {lb} {ratio(v1, v2)} slower than {lb2} ({ms(v1)} ms against {ms(v2)} ms)' for qid, _, lb, v1, lb2, v2 in lo) + '.']
    else:
        out += ['- pg_sift was the fastest system, or within 5% of it, on every question.']
    out += [f'- The same questions at {R.meta["tiers"][x]["rows"]:,} orders, cold runs after a restart, load times and plans: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#analytics-an-online-store).' for x in R.tiers[:-1]]
    out += [NOT_REMEASURED] if R.meta['tiers'][R.tiers[-1]]['rows'] < 10_000_000 else []
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
    labels = dagger(R, tier, labels)
    out = (heading + ['']) if heading else []
    out += [f'Seeded data (sha256 `{t["data_sha256"][:16]}`), measured {t["parts"]["warm"]["date"]}, {host_noise(t["parts"]["warm"])} at the start of the runs (the host is shared). '
            f'Median of {t["runs"]} warm runs, minimum and maximum in parentheses, milliseconds; the fastest of a row and anything within 5% of it in bold.', '']
    out += [n for n in cache_note(R, tier)] + [''] * bool(cache_note(R, tier))
    head, body = bench_table(R, tier, shown, labels, detail=True)
    out += md_table(head, body) + ['']
    out += ['Elasticsearch with the shard request cache on (the same request repeated is answered from the cache; the numbers above are with it off):', '']
    head, body = bench_table(R, tier, ['es', 'es_cached'], labels, detail=True)
    out += md_table(head, body) + ['']
    if any(R.get(tier, e, variant='cold', cls=q, metric='latency_ms') for e in shown for q in R.queries(tier)):
        out += ['First run after the server\'s memory was emptied (one restart of each server, then before every run: PostgreSQL and Citus every shared buffer, ClickHouse its mark, uncompressed-block, primary-key and index caches, '
                'Elasticsearch: a restart of the container, because emptying its memory was not as cold; every run on a new connection; the operating system\'s page cache is not dropped, which needs root, as after a restart): median of '
                f'{t.get("cold_runs", "?")} runs, minimum and maximum in parentheses.', '']
        head, body = bench_table(R, tier, shown, labels, detail=True, variant='cold')
        out += md_table(head, body) + ['']
        rc = t.get('restart_cold')
        if rc:
            out += [f'{", ".join(labels[e] for e in rc["engines"])}: restart of the container for every run (median of {rc["runs"]}), measured by an earlier pass of the same session on {rc["date"][:10]} (the same build, data and thread budget, before the harness emptied the memory instead of restarting)'
                    + (f'; NOISY: {", ".join(rc["noisy"])}' if rc['noisy'] else '') + '.', '']
        chk = [(q, e) for q in R.queries(tier) for e in shown if R.get(tier, e, variant='cold-alt', cls=q, metric='latency_ms')]
        if chk:
            how = lambda e: (R.get(tier, e, '-', 'cold', 'cold_how') or {}).get('value', 'evict')
            pair = lambda q, e: tuple(R.num(tier, e, q, v, 'latency_ms') for v in (('cold', 'cold-alt') if how(e) == 'evict' else ('cold-alt', 'cold')))   # (emptied, restarted)
            out += ['Emptying the memory against real restarts, the check that the first is as cold as the second (milliseconds, median of the same number of runs; a system whose ratio is beyond 20% is published with restarts: '
                    + (', '.join(labels[e] for e in shown if how(e) == 'restart') or 'none') + '):', '']
            out += md_table(['question', 'system', 'after emptying the memory', 'after a restart', 'ratio'], [[QUERY_TITLES[q], labels[e], ms(pair(q, e)[0]), ms(pair(q, e)[1]), f'{pair(q, e)[0] / pair(q, e)[1]:.2f}x'] for q, e in chk]) + ['']
    return out


NOT_REMEASURED = ('- **10,000,000 orders were not measured for the 3.1.0 pass** (`bench/bench final --size standard` measures this size only; `--size full` adds 10,000,000). The last 10,000,000 run (history), on 2026-10-18 with a 3.0 development build, is in the git history of `bench/results/store.json`: '
                  'there the data no longer fit the cache, ClickHouse was faster than pg_sift `sift_col` on 7 of the 10 questions (one of them a tie; the top 10 categories 17x, the distinct customers per month 4.0x) and `sift_table` failed on the two search questions with facets and the newest orders (a later check of the 3.0 build at 10,000,000 orders loaded in 1,000,000-row transactions answered all ten questions as PostgreSQL does). '
                  'Do not read the 1,000,000-order ranking above as the ranking at ten times the size.')


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
        allq = ['pg', 'sift', 'sift_table', 'es', 'es_cached', 'ch', 'citus']
        engines = R.engines(tier, allq)
        shown = [e for e in engines if e != 'es_cached']
        out += [f'### {t["rows"]:,} orders', '']
        out += latency_tables(R, tier, shown, labels, [f'#### {set_heading(R, tier)}'])
        phases = [('rows', 'data'), ('indexes', 'indexes'), ('vacuum', 'vacuum, analyze'), ('merge', 'merge to one segment')]
        body = []
        for key, label in phases:
            vals = [R.num(tier, e, variant=key, metric='load_s') for e in shown]
            if any(v is not None for v in vals):
                body.append([label] + [seconds(v) for v in vals])
        body.append(['**total**'] + [seconds(R.num(tier, e, metric='load_s')) for e in shown])
        body += size_rows(R, tier, shown)
        out += ['Load time (from the same CSV file, one client) and size:', ''] + md_table(['phase'] + [dagger(R, tier, labels)[e] for e in shown], body) + ['', size_note(R, tier, shown, ''), '']
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
    quota = R.cpus(last)
    th =t.get('threads')
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
    out += [f'- PostgreSQL {s["pg"]} (the same image for both layouts, pg_sift {s["sift_sift"]} (the build under test)' + (f', linked with `{s["sift_libs"]}`, `compression` unset on the index = `auto`, `sift.table_compression` {s["sift_table_compression"]}' if 'sift_libs' in s else '') + f'): {s["pg_settings"]}; {pg_text}.',
            *citus_config(R, last),
            f'- Elasticsearch {s["es"]} (Lucene {s["es_lucene"]}): one node, {R.cpus(last)} shards (one per CPU of the budget of the load: a shard is searched by one thread, so with one shard a search uses one core whatever `node.processors` says; the shard count is a choice of this harness), no replica, JVM heap `{s["es_heap"]}`, {"`node.processors` and `-XX:ActiveProcessorCount` set to N, " if th else ""}security and machine learning off; `scaled_float` (factor 100) for the amount, `keyword` for country, category and status, `date` for the time, `text` for the title; '
            'each shard merged to one segment after the load; aggregations of the search API with `size: 0`; the shard request cache off (`request_cache=false`) except in the table that says otherwise; distinct counts are `cardinality` (HyperLogLog++, `precision_threshold` 40000), the only approximate result.',
            f'- ClickHouse {s["ch"]}: `MergeTree`, `PARTITION BY` month, `ORDER BY (country, created_at, id)` (`status` and `amount` stay out of the key so that they can be updated), `LowCardinality` strings, `Decimal(10, 2)` for the amount, a text index on the title, a Bloom filter on the id, the settings lightweight updates need; {"`max_threads`, `max_insert_threads`, `max_final_threads` and `background_pool_size` set to N" if th else f"`max_threads` {s['ch_threads']} (what the container limit gives it)"}; optimized to one part per partition after the load.',
            '- pg_sift: `sift_col` over the columns the reports read plus the month of the order, a `sift` BM25 index on the title; `sift_table` is the table itself in column format with a `sift` index on the title and the primary key. PostgreSQL: primary key, btree on the date and on country and status, GIN on `to_tsvector(\'simple\', title)`.',
            f'- Method: every engine has {cpu_name(quota)} for its server, the budget of the search tables; one client, one request at a time, wall time from this process to the end of the response over a persistent local connection; the engines are timed in turn within each run so a drift of the host load hits all of them; one untimed run per engine first (its result is checked against PostgreSQL\'s), then the timed runs. The numbers of one table are comparable with each other, not with another host.']
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
    out += md_table(['{:,} orders'.format(t['rows'])] + [dagger(R, tier, UPD_LABELS)[e] for e in engines], rows) + ['']
    out += ['- ' + x for x in upd_sentences(R, tier, engines) + cache_note(R, tier)]
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
    out += md_table(['{:,} orders'.format(t['rows'])] + [dagger(R, tier, UPD_LABELS)[e] for e in engines], upd_rows(R, tier, engines, True)) + [''] + cache_note(R, tier) + [''] * bool(cache_note(R, tier))
    out += ['The three reads, before the changes, after them (before maintenance) and after the maintenance step (milliseconds, median of the runs):', '']
    out += md_table(['query'] + [dagger(R, tier, UPD_LABELS)[e] for e in engines], upd_read_rows(R, tier, engines)) + ['']
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
    cols = [e for e in lanes] + [e for e in ('pg', 'ch', 'es', 'citus') if R.engines(tier, [e])]
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
    ch, es = Http('ch', CH_AUTH), Http('es_cached', {'Content-Type': 'application/json'})
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
        if '/_doc/' in path:   # a real-time get reports no `took`: the same lookup as a search by id does
            method, path, body = 'POST', '/orders/_search?request_cache=false', json.dumps({'query': {'ids': {'values': [path.rsplit('/', 1)[1]]}}}).encode()
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
    """two result files side by side: the warm median of every question and system in each result set both have, the ratio B / A (below 1: B is faster) with the 95% interval of the
    ratio of the medians of the runs, and * where the interval excludes 1 and a Mann-Whitney test of the runs rejects equality (benchlib.changed)"""
    A, B = Results(args.a), Results(args.b)
    body, flagged = [], 0
    for tier in [t for t in A.all_tiers if t in B.all_tiers]:
        for q in A.queries(tier):
            for e in all_lanes(A, tier):
                ra, rb = (X.get(tier, e, q, 'warm', 'latency_ms') for X in (A, B))
                if not (ra and rb and ra.get('values') and rb.get('values')):
                    continue
                ratio, lo, hi, p, diff = benchlib.changed(ra['values'], rb['values'])
                flagged += diff
                body.append([tier, q, QUERY_TITLES[q][:44], LANES[e][0], ms(ra['value']), ms(rb['value']), f'{ratio:.2f} [{lo:.2f}, {hi:.2f}]' + (' *' if diff else ''), f'{p:.3f}'])
    print('\n'.join(md_table(['set', 'q', 'question', 'system', 'A ms', 'B ms', 'B / A [95%]', 'p'], body, ['---'] * 4 + ['---:'] * 4)))
    print(f'\n* {flagged} of {len(body)} cells changed (interval of the ratio excludes 1 and Mann-Whitney p < 0.05)')
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
        p.add_argument('--cold-runs', type=int, default=3, help='cold: runs of a question per server whose memory is emptied between them')
        p.add_argument('--restart-runs', type=int, help='cold: runs of a question per server that is restarted between them (Elasticsearch: a restart takes ~20 s); default --cold-runs')
        p.add_argument('--cold-check', type=lambda s: s.split(',') if s else [], default=[], help='cold: questions also timed the other way (emptied against restarted), to check that emptying the memory is as cold; '
                       'off unless named, and its result is kept in the cache once per version of the engine')
        p.add_argument('--load-seconds', type=int, default=int(os.environ.get('STORE_LOAD_SECONDS', 15)), help='updates: how long a query is timed under each update rate')
        p.add_argument('--probes', type=int, default=20, help='updates: single-row changes timed until a query sees them')
        p.add_argument('--rates', type=lambda s: [int(x) for x in s.split(',')], default=[0, 1000, 10000], help='updates: the update rates (rows/s) the query is timed under')
        p.add_argument('--compression', action='store_true', help='load: also the size of each system with its other compression settings')
        p.add_argument('--no-resume', action='store_true', help='measure a part again even when its file holds the key of this command (resume skips it)')
        p.add_argument('--keep-going', action='store_true', help='go on after a result that differs from PostgreSQL\'s (the exit status is still 3)')
        p.add_argument('--untimed', type=lambda s: s.split(',') if s else [], default=[], help='engines that only serve as the reference of the result checks: run, never recorded (the plan sets it)')
        p.add_argument('--max-rerun', type=int, default=6, help='most NOISY cells measured again at the end of a stage')
        p.add_argument('--parallel-engines', dest='parallel', type=int, default=1, help='run: containers timed at the same time, each on disjoint physical cores of its own (docker --cpuset-cpus; equal sets, '
                       'moved round the cores); 1: one engine at a time')
        p.add_argument('--busy-cores', type=int, default=2, help='the utilization report counts the seconds with fewer busy cores than this')
        p.add_argument('--keys', type=lambda s: [x for x in s.split(',') if x], default=[], help='load: container=key of the data volumes (datakey)')
        p.add_argument('--reuse', type=lambda s: [x for x in s.split(',') if x], default=[], help='load: containers whose volume already holds the data of that key')
        p.add_argument('--containers', type=lambda s: s.split(','), default=['pg', 'sift'], help='datakey: the containers to key')
        cache_args(p)

    def cache_args(p):
        p.add_argument('--cache-dir', default=os.environ.get('STORE_CACHE', '/tmp/sift-store-cache'), help='the cache of the cells of the servers that are not under test')
        p.add_argument('--no-cache', action='store_true', help='read nothing from the cache and write nothing to it')
        p.add_argument('--cache-max-age', type=int, default=30, help='days after which a cached cell is measured again')
    for name in ('gen', 'load', 'run', 'cold', 'updates', 'verify', 'plan', 'datakey', 'interference'):
        common(sub.add_parser(name))
    it = sub.choices['interference']
    it.add_argument('--target', default='sift', help='the engine whose requests are timed')
    it.add_argument('--concurrent', type=int, default=3, help='containers that loop the same question at the same time')
    it.add_argument('--samples', type=int, default=41)
    it.add_argument('--threshold', type=float, default=0.05, help='the largest change of a median that still counts as no interference')
    pl = sub.choices['plan']
    pl.add_argument('--parts', type=lambda s: s.split(','), default=['load', 'warm', 'cold', 'updates'], help='the parts of the pass: load, warm, cold, updates')
    pl.add_argument('--points', type=lambda s: [int(x) for x in s.split(',')], help='the thread budgets the questions are timed at')
    pl.add_argument('--update-engines', type=lambda s: s.split(','), default=UPDATE_ENGINES)
    pl.add_argument('--remeasure-all', action='store_true', help='measure every server again whatever the cache holds')
    pl.add_argument('--remeasure', type=lambda s: s.split(','), default=[], help='measure these servers again (pg, citus, es, ch)')
    pl.add_argument('--plan', help='where the plan is written (default OUT/plan.json)')
    pg = sub.add_parser('planget')
    pg.add_argument('plan')
    pg.add_argument('field', choices=['run', 'untimed', 'update', 'cached', 'measured'])
    cs = sub.add_parser('cache-save')
    cs.add_argument('--plan', required=True)
    cs.add_argument('--results', required=True)
    cache_args(cs)
    for name, doc in (('noisy', 'the cells of a part file that are still NOISY'), ('patch', 'replace in a part file the cells a second file measured again')):
        np = sub.add_parser(name, help=doc)
        np.add_argument('file')
        if name == 'patch':
            np.add_argument('fresh')
    st = sub.add_parser('stable', help='do two results files tell the same story?')
    st.add_argument('a')
    st.add_argument('b')
    st.add_argument('--tol', type=float, default=1.0, help='the ratios of pg_sift to each system may differ by a factor of 1 + TOL')
    st.add_argument('--sep', type=float, default=1.5, help='a pair is ordered when its ratio is beyond this in both files')
    m = sub.add_parser('merge')
    m.add_argument('files', nargs='*')
    m.add_argument('-o', required=True)
    m.add_argument('--restart-cold', help='results file whose cold cells restarted the server for every run: take its cold cells of --restart-engines')
    m.add_argument('--restart-engines', type=lambda s: s.split(','), default=[])
    m.add_argument('--plan', action='append', default=[], help='a plan of the pass (repeat per size): its cached groups are read from the cache and merged in (cells of another host are refused)')
    m.add_argument('--keep', action='append', default=[], help='an earlier results file: its sets that this pass did not measure are carried over (skipped with a WARN when it was measured on another host)')
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
    global MEASURE_CODE, LOAD_CODE, UTIL
    LOAD_CODE = benchlib.key_of([inspect.getsource(f) for f in (load_pg, build_indexes, copy_chunks, load_ch, load_es, bulk_docs)], 12)
    MEASURE_CODE = benchlib.key_of([LOAD_CODE] + [inspect.getsource(f) for f in (run_query, time_cell, time_wave, one_container, cold_cell, evict, restart, update_engine, with_load, churn, read_latency,
                                                                                   read_after_write, Client.run, Writer.bulk, Writer.probe_set)], 12)
    if args.cmd in ('gen', 'load', 'run', 'cold', 'updates', 'verify', 'plan', 'datakey', 'interference') and not args.engines:
        args.engines = UPDATE_ENGINES if args.cmd == 'updates' else DEFAULT_ENGINES
    args.cell = 0
    if args.cmd in ('load', 'run', 'cold', 'updates', 'interference'):
        UTIL = Utilization(args.busy_cores)
    if getattr(args, 'rows', None) and not getattr(args, 'tier', 'x'):
        args.tier = (f'{args.rows // 1000000}M' if args.rows % 1000000 == 0 else f'{args.rows // 1000}k') + f'-c{args.cpus}'
    if hasattr(args, 'restart_runs') and args.restart_runs is None:
        args.restart_runs = args.cold_runs
    if args.cmd == 'plan':
        args.plan = args.plan or os.path.join(args.out, 'plan.json')
        args.points = args.points or [args.cpus]
        args.update_engines = args.update_engines if 'updates' in args.parts else []
    if args.cmd in ('plan', 'merge', 'cache-save', 'cold'):
        args.fingerprint = host_fp()
    if args.cmd == 'cold':
        args.cache = make_cache(args)
    benchlib.resumable()
    if args.cmd in PARTS and not args.no_resume and part_done(args):
        log(f'{args.cmd}: {part_path(args, PARTS[args.cmd])} holds the key of this command: measured before, kept')
        sys.exit(0)
    try:
        sys.exit({'gen': cmd_gen, 'verify': cmd_verify, 'load': cmd_load, 'run': cmd_run, 'cold': cmd_cold, 'updates': cmd_updates, 'merge': cmd_merge, 'render': cmd_render, 'table': cmd_table,
                  'stats': cmd_stats, 'gap': cmd_gap, 'compare': cmd_compare, 'plan': cmd_plan, 'planget': cmd_planget, 'cache-save': cmd_cache_save, 'noisy': cmd_noisy, 'patch': cmd_patch,
                  'stable': cmd_stable, 'datakey': cmd_datakey, 'interference': cmd_interference}[args.cmd](args))
    except (RuntimeError, benchlib.ForeignHost) as x:
        die(str(x))


if __name__ == '__main__':
    main()
