"""The orders benchmark's servers: the clients of PostgreSQL, Elasticsearch and ClickHouse, the containers, and the windows that tell how busy the host was (steps/store.py)."""
import benchlib
import pgq
import functools
import http.client
import json
import os
import re
import subprocess
import threading
import time
from store_lib import CH_AUTH, LANES, NAME, NULL, PG_ENGINES, container, die, is_ch, is_es, log, port, quiet


class Psql(pgq.Backend):
    """one psql backend kept for the run, on the published port of the container; the end of a statement's output is a marker line"""
    def __init__(self, eid, timeout=600):
        db = LANES[eid][3] if eid in PG_ENGINES else 'postgres'
        super().__init__(['psql', '-X', '-q', '-At', '-F', '\t', '-P', f'null={NULL}', '-h', '127.0.0.1', '-p', str(port(eid)), '-U', 'postgres', '-d', db], setup=[f'SET statement_timeout = {timeout * 1000}'])


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
    return (v[0] + v[1] + v[2] + v[5] + v[6] + v[7]) / CLK   # user nice system irq softirq steal (guest is in user); idle and iowait are not work


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
