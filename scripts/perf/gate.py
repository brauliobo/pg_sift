#!/usr/bin/env python3
"""The black-box performance gate of a pull request: the canaries of scripts/perf/canaries.py on the release library, the stock planner and a cloned template cluster.

  python3 -I scripts/perf/gate.py --smoke | --area star | --areas star,jsonb | --all  [--no-perf] [--port P] [--root DIR]

It speaks SQL and EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON) only. What it asserts does not move with the load of the host: blocks, temp blocks, rows vetted in the heap,
workers launched, the balance of the workers, and instructions:u of the postmaster and every process it forks (perf stat), as a ratio to the same statement without sift.
Without a usable PMU the instructions are not asserted and everything else is.
"""
import argparse
import hashlib
import json
import math
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import replace
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import canaries  # noqa: E402

REPO = HERE.parent.parent
DB = 'sift_perf'
KEEP_TEMPLATES = 2
TPL_TTL = int(os.environ.get('SIFT_PERF_TPL_TTL', 7200))
SEED = 1
JITTER_FLOOR = 250_000
MIN_WORKER_SHARE, MAX_WORKER_SKEW = 0.50, 0.60
HEAP_COUNTERS = ('Heap Fetches', 'Heap Verifies', 'Heap Vet Rows', 'Rows Vetted Against The Heap')
STOCK = ('SET enable_seqscan = on; SET enable_indexscan = on; SET enable_bitmapscan = on; SET enable_indexonlyscan = on; SET seq_page_cost = 1.0; SET random_page_cost = 4.0; '
         "SET cpu_tuple_cost = 0.01; SET effective_cache_size = '4GB'; SET work_mem = '64MB'; SET jit = off; SET max_parallel_maintenance_workers = 0; SET statement_timeout = '120s'")
SIFT_ON = 'SET sift.enable_agg = on; SET sift.enable_col_scan = on; SET sift.enable_scan = on'
SIFT_OFF = 'SET sift.enable_agg = off; SET sift.enable_col_scan = off; SET sift.enable_scan = off'
WORKERS = {'1T': 0, '4T': 4}
EXPLAIN = 'ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON'
EXPLAIN_VERBOSE = EXPLAIN + ', VERBOSE'


class GateError(Exception):
    """The environment is wrong (no library, no cluster, no fixture): not a regression."""


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise GateError(f'{cmd[0]} failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-600:]}')
    return r.stdout


def settings(mode, sift_on=True, extra=''):
    return f'{STOCK}; SET max_parallel_workers_per_gather = {WORKERS[mode]}; {SIFT_ON if sift_on else SIFT_OFF}; {extra}'


# ---- what the log of the verdict says: pure functions, tested by scripts/perf/selftest.py ----

def walk(node):
    yield node
    for child in node.get('Plans', ()):
        yield from walk(child)


def custom_provider(node):
    return node.get('Custom Plan Provider', '') if node.get('Node Type') == 'Custom Scan' else ''


def has_family(root, family):
    nodes = list(walk(root))
    if family == 'none':
        return True
    if family == 'agg':
        return any(custom_provider(n).startswith(canaries.AGG_FAMILY) for n in nodes)
    if family == 'bm25':
        return any(custom_provider(n).startswith(canaries.BM25_FAMILY) for n in nodes)
    index = family.removeprefix('idx:')
    return any(n.get('Node Type', '').startswith(('Index Scan', 'Index Only Scan', 'Bitmap Index Scan')) and n.get('Index Name') == index for n in nodes)


def blocks(node):
    return node.get('Shared Hit Blocks', 0) + node.get('Shared Read Blocks', 0)


def prop_max(root, name):
    return max((n[name] for n in walk(root) if isinstance(n.get(name), (int, float))), default=None)


def worker_failures(root, want, balance):
    parallel = [n for n in walk(root) if 'Workers Planned' in n]
    out = [f'no parallel node: {want} workers expected under max_parallel_workers_per_gather = 4 with the stock costs'] if want and not parallel else []
    for n in parallel:
        planned, launched = n['Workers Planned'], n.get('Workers Launched', 0)
        if planned < want:
            out.append(f'{n["Node Type"]} plans {planned} workers, {want} expected')
        if launched != planned:
            out.append(f'{n["Node Type"]} launched {launched} of {planned} workers')
        if balance:
            out += [f for leaf in walk(n) if leaf.get('Parallel Aware') for f in balance_failures(leaf)]
    return out


def balance_failures(scan):
    """The workers of a parallel scan read the table: per-worker rows are in the scan node's Workers array, with EXPLAIN VERBOSE; Actual Rows is the average of the loops (leader and workers)."""
    rows = [w['Actual Rows'] * w['Actual Loops'] for w in scan.get('Workers', ()) if 'Actual Rows' in w]
    total = scan['Actual Rows'] * scan['Actual Loops']
    if not total or not rows:
        return []
    out = []
    if sum(rows) / total < MIN_WORKER_SHARE:
        out.append(f'workers read {sum(rows) / total:.0%} of the rows, at least {MIN_WORKER_SHARE:.0%} expected: the leader scans the table')
    if len(rows) > 1 and sum(rows) and max(rows) / sum(rows) > MAX_WORKER_SKEW:
        out.append(f'one worker read {max(rows) / sum(rows):.0%} of the rows of the workers, at most {MAX_WORKER_SKEW:.0%} expected')
    return out


def plan_failures(cn, explain):
    """The failures of the EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) document of a canary against its limits."""
    root = explain[0]['Plan']
    nodes = list(walk(root))
    out = []
    if not any(has_family(root, f) for f in cn.plan):
        out.append(f'no {" / ".join(cn.plan)} in the plan: ' + ', '.join(sorted({custom_provider(n) or n['Node Type'] for n in nodes})))
    if all(f.startswith('idx:') for f in cn.plan) and any(custom_provider(n).startswith(canaries.SIFT_FAMILY) for n in nodes):
        out.append('a sift scan took a lookup the B-tree answers')
    if cn.no_seq and any(n['Node Type'] == 'Seq Scan' for n in nodes):
        out.append('Seq Scan in the plan')
    if cn.blocks is not None and blocks(root) > cn.blocks:
        out.append(f'{blocks(root)} shared blocks, at most {cn.blocks}')
    temp = sum(n.get('Temp Read Blocks', 0) + n.get('Temp Written Blocks', 0) for n in nodes)
    if temp > cn.temp:
        out.append(f'{temp} temp blocks, at most {cn.temp}')
    if cn.heap_zero:
        out += [f'{k} = {v}, 0 expected' for k in HEAP_COUNTERS if (v := prop_max(root, k))]
    out += [f'{k} = {v}, at most {cap}' for k, cap in cn.extra if '_' not in k and (v := prop_max(root, k)) is not None and v > cap]
    if cn.mode == '4T':
        out += worker_failures(root, cn.workers, cn.balance)
    return out


def plan_summary(cn, explain):
    """The counters the verdict read from the EXPLAIN document, for the log."""
    root = explain[0]['Plan']
    nodes = list(walk(root))
    scans = sorted({custom_provider(n) or n['Node Type'] for n in nodes if custom_provider(n) or n['Node Type'].startswith(('Index', 'Seq', 'Parallel', 'Bitmap Heap', 'Gather'))})
    heap = {k: v for k in HEAP_COUNTERS if (v := prop_max(root, k)) is not None}
    workers = [f'{n.get("Workers Launched", 0)}/{n["Workers Planned"]}' for n in nodes if 'Workers Planned' in n]
    temp = sum(n.get('Temp Read Blocks', 0) + n.get('Temp Written Blocks', 0) for n in nodes)
    return f'plan={",".join(scans)} blocks={blocks(root)}/{cn.blocks} temp={temp}/{cn.temp} heap={heap or 0} workers={",".join(workers) or "-"}'


def parse_perf(text):
    """instructions:u of the `perf stat -x,` output, None when the counter was not counted or not supported."""
    for line in text.splitlines():
        f = line.split(',')
        if len(f) > 2 and f[2].startswith('instructions'):
            return int(f[0]) if f[0].isdigit() else None
    return None


def rows_equal(a, b, tol=1e-9):
    """The rows of two statements: text equal, or numbers equal to a relative 1e-9."""
    def same(x, y):
        try:
            return math.isclose(float(x), float(y), rel_tol=tol, abs_tol=tol)
        except ValueError:
            return x == y
    return len(a) == len(b) and all(len(r) == len(s) and all(same(x, y) for x, y in zip(r, s)) for r, s in zip(a, b))


def ratio_failure(sift, base, ceiling):
    """sift is over its share of the native plan (ceiling * base) by more than the jitter floor: a point query of a few hundred thousand instructions never flakes."""
    return sift - ceiling * base > JITTER_FLOOR


def number_failures(cn, sift, base, rows):
    out = []
    if cn.ratio is not None and base.get('instr') and sift.get('instr') and ratio_failure(sift['instr'], base['instr'], cn.ratio):
        out.append(f'instructions {sift["instr"] / base["instr"]:.2f}x of the native plan, at most {cn.ratio}')
    if cn.per_row is not None and sift.get('instr') and sift['instr'] / rows > cn.per_row:
        out.append(f'{sift["instr"] / rows:.0f} instructions per row, at most {cn.per_row:.0f}')
    if cn.total is not None and sift.get('instr') and sift['instr'] > cn.total:
        out.append(f'{sift["instr"]} instructions, at most {cn.total}')
    if cn.parity and base and not rows_equal(sift['rows'], base['rows']):
        out.append('the rows differ from the native plan')
    return out


def write_failures(cn, sift, base, rows):
    out = []
    if (cap := canaries.limit(cn, 'bytes_per_row')) and sift['rel'] / rows > cap:
        out.append(f'{sift["rel"] / rows:.0f} bytes per row on disk, at most {cap}')
    if (cap := canaries.limit(cn, 'wal_per_row')) and sift['wal'] / rows > cap:
        out.append(f'{sift["wal"] / rows:.0f} WAL bytes per row, at most {cap}')
    if (cap := canaries.limit(cn, 'wal_ratio')) and base['wal'] and sift['wal'] / base['wal'] > cap:
        out.append(f'WAL {sift["wal"] / base["wal"]:.2f}x of the untracked table, at most {cap}')
    return out


def gpu_device_used(explain):
    notes = [n['GPU'] for n in walk(explain[0]['Plan']) if 'GPU' in n]
    return any(not g.startswith('not used') for g in notes), '; '.join(notes) or 'no GPU line in the plan'


# ---- the cluster: a template built once per content hash, cloned for every run ----

class Install:
    def __init__(self, root):
        self.root = Path(root)
        self.sharedir = f'{self.root}{run(["pg_config", "--sharedir"]).strip()}'
        self.libdir = f'{self.root}{run(["pg_config", "--pkglibdir"]).strip()}'
        self.so = Path(self.libdir) / 'pg_sift.so'
        if not self.so.is_file() or not (Path(self.sharedir) / 'extension' / 'pg_sift.control').is_file():
            raise GateError(f'no pg_sift under {self.root}: build the release library (SIFT_TESTING= scripts/pgtest.sh install)')
        if int(run(['pg_config', '--version']).split()[1].split('.')[0]) < 18:
            raise GateError('the gate needs PostgreSQL 18 (extension_control_path)')

    def key(self):
        h = hashlib.sha256()
        for f in (self.so, *sorted(Path(self.sharedir, 'extension').glob('pg_sift*')), HERE / 'fixture.sql'):
            h.update(hashlib.sha256(f.read_bytes()).digest())
        h.update(run(['pg_config', '--version']).encode() + f'rows={canaries.ROWS}:seed={SEED}'.encode())
        return h.hexdigest()[:16]


class Cluster:
    def __init__(self, install, home, port):
        self.install, self.home, self.port = install, Path(home), port
        self.data, self.sock, self.log = self.home / 'data', self.home / 'sock', self.home / 'postgres.log'
        self.env = {**os.environ, 'PGHOST': str(self.sock), 'PGPORT': str(port), 'PGUSER': 'postgres', 'PGDATABASE': DB, 'LC_ALL': 'C'}

    def conf(self):
        return '\n'.join([f'port = {self.port}', "listen_addresses = ''", f"unix_socket_directories = '{self.sock}'", f"extension_control_path = '{self.install.sharedir}:$system'",
                          f"dynamic_library_path = '{self.install.libdir}:$libdir'", 'fsync = off', 'shared_buffers = 256MB', 'max_worker_processes = 12', 'max_parallel_workers = 8',
                          'autovacuum = off', "timezone = 'UTC'", 'max_wal_size = 1GB', 'sift.async_maintenance = off', 'pg_sift.install_notice = off', 'max_locks_per_transaction = 256', ''])

    def initdb(self):
        run(['initdb', '--no-sync', '-D', str(self.data), '-A', 'trust', '-U', 'postgres', '-E', 'UTF8', '--no-locale', '--no-instructions'])

    def start(self):
        self.sock.mkdir(parents=True, exist_ok=True)
        with open(self.data / 'postgresql.conf', 'a') as f:
            f.write(self.conf())
        run(['pg_ctl', '-D', str(self.data), '-l', str(self.log), '-w', 'start'])
        self.pm = int((self.data / 'postmaster.pid').read_text().split()[0])

    def stop(self):
        if (self.data / 'postmaster.pid').exists():
            run(['pg_ctl', '-D', str(self.data), '-m', 'fast', '-w', 'stop'])

    def psql(self, *args, db=DB):
        return run(['psql', '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1', '-d', db, *args], env=self.env)

    def query(self, sql):
        return self.psql('-c', sql).strip()


def build_template(install, cache, key, port):
    tmp = cache / f'tpl-{key}.tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    cl = Cluster(install, tmp, port)
    cl.initdb()
    cl.start()
    try:
        run(['createdb', DB], env=cl.env)
        cl.psql('-v', f'rows={canaries.ROWS}', '-v', f'seed={SEED}', '-f', str(HERE / 'fixture.sql'))
        cl.psql('-c', f"COPY tbl_store TO '{tmp / 'copy.tsv'}'")
    finally:
        cl.stop()
    (tmp / 'stamp.json').write_text(json.dumps({'key': key, 'rows': canaries.ROWS, 'seed': SEED, 'built': time.time()}))
    os.rename(tmp, cache / f'tpl-{key}')


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def pid_in(path):
    """The first number of the file (a lock's pid, a postmaster.pid), 0 when there is none."""
    try:
        return int(next(iter(path.read_text().split()), 0))
    except (OSError, ValueError):
        return 0


def acquire(path, ready):
    """mkdir lock with the pid of the holder: a lock of a dead holder is taken over. False when ready() turned true while another process held it."""
    deadline = time.time() + 600
    while not ready():
        try:
            path.mkdir()
            (path / 'pid').write_text(str(os.getpid()))
            return True
        except FileExistsError:
            holder = pid_in(path / 'pid')
            if holder and not pid_alive(holder):
                shutil.rmtree(path, ignore_errors=True)
            elif time.time() > deadline:
                raise GateError(f'template lock {path} is held by pid {holder}')
            else:
                time.sleep(0.2)
    return False


def ensure_template(install, cache, port, rebuild):
    key = install.key()
    final, lock = cache / f'tpl-{key}', cache / f'tpl-{key}.lock'
    if rebuild:
        shutil.rmtree(final, ignore_errors=True)
    built = acquire(lock, final.is_dir)
    if built:
        try:
            if not final.is_dir():
                build_template(install, cache, key, port)
        finally:
            shutil.rmtree(lock, ignore_errors=True)
        for old in sorted((p for p in cache.glob('tpl-*') if p.is_dir() and p.suffix not in ('.lock', '.tmp')), key=lambda p: p.stat().st_mtime)[:-KEEP_TEMPLATES]:
            shutil.rmtree(old, ignore_errors=True)
    final.touch()
    return final, built


def cache_root():
    """The template and the run clusters are scratch: memory, as every cluster of the repository (SIFT_PERF_CACHE names another tmpfs directory)."""
    base = Path(os.environ.get('SIFT_PERF_CACHE', '/dev/shm/sift-perf-gate'))
    try:
        base.mkdir(parents=True, exist_ok=True)
        free = shutil.disk_usage(base).free
    except OSError as e:
        raise GateError(f'no scratch directory {base}: {e}; set SIFT_PERF_CACHE to a tmpfs directory') from e
    if free < 2 << 30:
        raise GateError(f'{base} has {free >> 20} MiB free, 2 GiB expected (two templates and a run): set SIFT_PERF_CACHE to a larger tmpfs directory')
    return base


def live(d):
    """A run of a gate that runs, a lock that is held, or a postmaster still up in it."""
    pids = [pid_in(d / 'pid'), pid_in(d / 'data' / 'postmaster.pid')] + ([int(d.name.split('-')[1])] if d.name.startswith('run-') else [])
    return any(p and pid_alive(p) for p in pids)


def clean_stale(cache, tpl_ttl=TPL_TTL):
    """The runs of the gates that died; the templates (built or half built) unused for tpl_ttl seconds, while no gate runs: a run reads its template."""
    busy = False
    for d in cache.glob('run-*'):
        if live(d):
            busy = True
        else:
            shutil.rmtree(d, ignore_errors=True)
    busy = busy or any(live(d) for d in cache.glob('tpl-*'))
    cutoff = time.time() - tpl_ttl
    for d in cache.glob('tpl-*'):
        if not busy and d.stat().st_mtime < cutoff:
            shutil.rmtree(d, ignore_errors=True)


# ---- one measurement: a psql session with perf attached to the postmaster, the counters on for the statement only ----

class Perf:
    """`perf stat -p <postmaster>` counts only inside the window the session opens, in every process the postmaster forks after the attach: the backend and its workers."""
    def __init__(self, home):
        self.ctl, self.ack, self.out = home / 'perf.ctl', home / 'perf.ack', home / 'perf.out'
        for f in (self.ctl, self.ack, self.out):
            f.unlink(missing_ok=True)
        os.mkfifo(self.ctl)
        os.mkfifo(self.ack)

    @staticmethod
    def available():
        try:
            r = subprocess.run(['perf', 'stat', '-x,', '-e', 'instructions:u', 'true'], env={**os.environ, 'LC_ALL': 'C'}, capture_output=True, text=True)
        except FileNotFoundError:
            return False
        return r.returncode == 0 and parse_perf(r.stderr) is not None

    def attach(self, pm):
        self.proc = subprocess.Popen(['perf', 'stat', '-p', str(pm), '-e', 'instructions:u', '-x,', '-o', str(self.out), '--control', f'fifo:{self.ctl},{self.ack}', '--delay=-1'],
                                     env={**os.environ, 'LC_ALL': 'C'}, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        fd_ack = os.open(self.ack, os.O_RDONLY | os.O_NONBLOCK)
        try:
            deadline = time.time() + 15
            while True:
                try:
                    fd_ctl = os.open(self.ctl, os.O_WRONLY | os.O_NONBLOCK)
                    break
                except OSError:
                    if self.proc.poll() is not None or time.time() > deadline:
                        raise GateError('perf stat did not start: ' + (self.proc.stderr.read() if self.proc.poll() is not None else 'timeout'))
                    time.sleep(0.01)
            os.write(fd_ctl, b'ping\n')
            if not select.select([fd_ack], [], [], 15)[0]:
                raise GateError('perf stat did not acknowledge the control ping')
            os.read(fd_ack, 64)
            os.close(fd_ctl)
        finally:
            os.close(fd_ack)

    def detach(self):
        self.proc.send_signal(signal.SIGINT)
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stderr.close()
        return parse_perf(self.out.read_text()) if self.out.exists() else None


def session_script(home, sets, pre, sql, explain, window):
    """explain: the EXPLAIN options of the plan pass ('' for none)."""
    ctl = lambda cmd: f'\\! echo {cmd} > "$GATE_CTL" && read -r ack < "$GATE_ACK"'
    pre, sql = (x.rstrip(';') + ';' if x else '' for x in (pre, sql))
    lines = ['\\set ON_ERROR_STOP on', sets + ';', f'\\o {home}/rows.txt', pre, sql]
    if explain:
        lines += [f'\\o {home}/explain.json', f'EXPLAIN ({explain}) {sql}']
    lines += ['\\o /dev/null', pre, 'SELECT pg_current_wal_insert_lsn() AS l0 \\gset']
    lines += [ctl('enable')] if window else []
    lines += [sql] + ([ctl('disable')] if window else [])
    lines += [f'\\o {home}/wal.txt', "SELECT (pg_current_wal_insert_lsn() - :'l0'::pg_lsn)::bigint;", '\\o']
    return '\n'.join(x for x in lines if x) + '\n'


def measure(cl, home, mode, sql, pre='', rel='', sift_on=True, explain=False, extra='', pmu=True, verbose=False):
    """One statement of a fresh session: a warm-up that gives the rows, the plan, then the statement inside the perf window. Returns instr (None without a PMU), wal, rel, rows, explain."""
    for f in home.glob('*.txt'):
        f.unlink()
    script = home / 'session.sql'
    script.write_text(session_script(home, settings(mode, sift_on, extra), pre, sql, EXPLAIN_VERBOSE if verbose else EXPLAIN if explain else '', pmu))
    perf = Perf(home) if pmu else None
    try:
        env = cl.env
        if perf:
            perf.attach(cl.pm)
            env = {**env, 'GATE_CTL': str(perf.ctl), 'GATE_ACK': str(perf.ack)}
        run(['psql', '-X', '-q', '-A', '-t', '-F', '\x1f', '-v', 'ON_ERROR_STOP=1', '-d', DB, '-f', str(script)], env=env)
    finally:
        instr = perf.detach() if perf else None
    rows = [line.split('\x1f') for line in (home / 'rows.txt').read_text().splitlines()]
    out = {'instr': instr, 'rows': rows, 'wal': int((home / 'wal.txt').read_text().strip()), 'rel': int(cl.query(f"SELECT pg_relation_size('{rel}')") or 0) if rel else 0}
    if explain:
        out['explain'] = json.loads((home / 'explain.json').read_text())
    return out


def tpl_sql(text, tpl):
    return text.replace('{copy_tsv}', str(tpl / 'copy.tsv'))


def run_gpu(cl, home, cn, pmu):
    """sift.gpu = auto: the small slice stays on the CPU, the dense aggregate runs on the device. Without a hardware Vulkan device (or a build without it) the canary is skipped."""
    auto = "SET sift.gpu = 'auto'; SET sift.gpu_min_rows = 10000"
    dense = measure(cl, home, cn.mode, cn.sql, explain=True, extra=auto, pmu=pmu)
    used, why = gpu_device_used(dense['explain'])
    if not used:
        return {'skip': f'no usable hardware Vulkan device: {why}'}
    fails, small = [], measure(cl, home, cn.mode, canaries.GPU_SMALL, explain=True, extra=auto, pmu=pmu)
    if gpu_device_used(small['explain'])[0]:
        fails.append('the small slice was sent to the device, it belongs on the CPU')
    cpu = measure(cl, home, cn.mode, cn.sql, extra="SET sift.gpu = 'off'", pmu=pmu)
    small_off = measure(cl, home, cn.mode, canaries.GPU_SMALL, extra="SET sift.gpu = 'off'", pmu=pmu)
    fails += number_failures(replace(cn, ratio=None, per_row=None, total=None), dense, cpu, canaries.ROWS)
    if small['instr'] and small_off['instr'] and ratio_failure(small['instr'], small_off['instr'], cn.ratio):
        fails.append(f'the CPU fallback costs {small["instr"] / small_off["instr"]:.2f}x of sift.gpu = off, at most {cn.ratio}')
    if dense['instr'] and cpu['instr'] and dense['instr'] / cpu['instr'] > canaries.limit(cn, 'offload_ratio'):
        fails.append(f'the host CPU spends {dense["instr"] / cpu["instr"]:.2f}x of the CPU path on a device scan, at most {canaries.limit(cn, "offload_ratio")}')
    return {'fails': fails, 'sift': dense, 'base': cpu}


def run_canary(cl, home, tpl, cn, pmu, inject):
    if cn.kind == 'gpu':
        return run_gpu(cl, home, cn, pmu)
    sql, pre = tpl_sql(cn.sql, tpl), tpl_sql(cn.pre, tpl)
    sift = measure(cl, home, cn.mode, sql, pre, cn.rel, explain=cn.kind == 'read', extra=inject, pmu=pmu, verbose=cn.balance)
    base = measure(cl, home, cn.mode, tpl_sql(cn.native, tpl) if cn.native else sql, tpl_sql(cn.native_pre, tpl) if cn.native_pre else pre, cn.native_rel, sift_on=False, pmu=pmu) if cn.baseline else {}
    fails = plan_failures(cn, sift['explain']) if 'explain' in sift else []
    fails += number_failures(cn, sift, base, canaries.ROWS) + write_failures(cn, sift, base, canaries.ROWS)
    return {'fails': fails, 'sift': sift, 'base': base}


def describe(cn, r):
    s, b = r['sift'], r['base']
    parts = []
    if s.get('instr'):
        parts.append(f'instr={s["instr"] / 1e6:.2f}M' + (f' native={b["instr"] / 1e6:.2f}M ratio={s["instr"] / b["instr"]:.2f}' if b.get('instr') else '') + f' per_row={s["instr"] / canaries.ROWS:.0f}')
    if 'explain' in s:
        root = s['explain'][0]['Plan']
        parts.append(f'blocks={blocks(root)}')
        parts += [f'workers={n["Workers Launched"]}/{n["Workers Planned"]}' for n in walk(root) if 'Workers Planned' in n][:1]
    if s.get('wal'):
        parts.append(f'wal={s["wal"]}' + (f' native_wal={b["wal"]}' if b else ''))
    if s.get('rel'):
        parts.append(f'rel={s["rel"]}')
    return ' '.join(parts)


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--area', action='append', default=[], help='one area (repeatable)')
    p.add_argument('--areas', '--groups', default='', help='csv of areas: ' + ', '.join(canaries.AREAS))
    p.add_argument('--smoke', action='store_true', help='one fast canary per subsystem (of the areas given, else of all)')
    p.add_argument('--all', action='store_true', help='all 22 canaries')
    p.add_argument('--no-perf', action='store_true', help='EXPLAIN, buffer, worker and result invariants only')
    p.add_argument('--port', type=int, default=int(os.environ.get('SIFT_PORT', 56100)))
    p.add_argument('--root', default=os.environ.get('SIFT_ROOT') or f'{os.environ.get("SIFT_TMP", "/tmp/sift-arch")}/root', help='the staged install (scripts/pgtest.sh install)')
    p.add_argument('--allow-testing-build', action='store_true', help='run a SIFT_TESTING library (local debugging: its instructions are not the release ones)')
    p.add_argument('--rebuild-template', action='store_true')
    p.add_argument('--no-leakcheck', action='store_true')
    p.add_argument('--inject', default='', help='SQL settings for the sift side only: a negative control that the gate must fail, e.g. "SET sift.enable_agg = off"')
    p.add_argument('--verbose', action='store_true', help='print the EXPLAIN counters each verdict read')
    p.add_argument('--json', help='write the measurements to this file')
    a = p.parse_args(argv)
    names = a.area + [x for x in a.areas.split(',') if x]
    a.smoke = a.smoke or 'smoke' in names
    a.names = [x for x in names if x != 'smoke']
    if not (a.names or a.smoke or a.all):
        p.error('choose --area, --areas, --smoke or --all')
    return a


def check_library(cl, allow_testing):
    """The backend maps the library of the install under test, not another one on the library path, and it is the release library."""
    mapped = cl.psql('-c', "LOAD 'pg_sift'", '-c', "SELECT bool_or(l LIKE '%' || $$" + str(cl.install.so) + "$$) FROM regexp_split_to_table(pg_read_file('/proc/self/maps'), E'\\n') l").strip()
    if mapped != 't':
        raise GateError(f'the backend does not map {cl.install.so}')
    tests = cl.query("SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'sift' AND p.proname LIKE '\\_test\\_%'")
    if tests != '0' and not allow_testing:
        raise GateError('the library is a SIFT_TESTING build: the gate measures the release library (--allow-testing-build for a local run)')


def leak_end(cl, home):
    """The leak gate every cluster of the repository is under (scripts/leakcheck.sh): the cluster checks, the stop, the logs."""
    script = REPO / 'scripts' / 'leakcheck.sh'
    r = subprocess.run(['bash', '-c', '. "$1"; leak_end "$2" "$3" pg_ctl -D "$2" -m fast -w stop', '_', str(script), str(cl.data), str(home)],
                       env={**cl.env, 'SIFT_TMP': str(home), 'SIFT_ROOT': str(cl.install.root)}, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def main(argv):
    a = parse_args(argv)
    selected = canaries.select(None if a.all else a.names or None, a.smoke)
    t0 = time.time()
    install = Install(a.root)
    cache = cache_root()
    clean_stale(cache)
    tpl, built = ensure_template(install, cache, a.port, a.rebuild_template)
    t_tpl = time.time()
    home = cache / f'run-{os.getpid()}'
    shutil.rmtree(home, ignore_errors=True)
    home.mkdir()
    run(['cp', '-a', '--reflink=auto', str(tpl / 'data'), str(home / 'data')])
    cl = Cluster(install, home, a.port)
    leak = not a.no_leakcheck and (REPO / 'scripts' / 'leakcheck.sh').is_file()
    results, failed = {}, 0
    try:
        cl.start()
        if leak:
            run(['bash', '-c', '. "$1"; leak_baseline "$2"', '_', str(REPO / 'scripts' / 'leakcheck.sh'), str(cl.data)])
        check_library(cl, a.allow_testing_build)
        pmu = not a.no_perf and Perf.available()
        if not pmu:
            print(f'perf-gate: INFO perf stat instructions:u {"disabled" if a.no_perf else "unavailable"}; enforcing EXPLAIN + buffer + pruning + worker + parity invariants only')
        print(f'perf-gate: template {tpl.name} {"built" if built else "reused"} in {t_tpl - t0:.1f}s, cluster up in {time.time() - t_tpl:.1f}s, {len(selected)} canaries')
        work = home / 'work'
        work.mkdir()
        for area in dict.fromkeys(cn.area for cn in selected):
            ta = time.time()
            for cn in (x for x in selected if x.area == area):
                tc = time.time()
                r = run_canary(cl, work, tpl, cn, pmu, a.inject)
                if 'skip' in r:
                    print(f'SKIP {cn.id:26s} {r["skip"]}')
                    continue
                if pmu and r['sift']['instr'] is None:
                    print('perf-gate: INFO perf stat counted no instructions; enforcing the plan, buffer, worker and result invariants only')
                    pmu = False
                results[cn.id] = {'fails': r['fails'], 'seconds': round(time.time() - tc, 2), 'instr': r['sift'].get('instr'), 'native_instr': r['base'].get('instr'),
                                  'wal': r['sift'].get('wal'), 'rel': r['sift'].get('rel'), 'note': describe(cn, r)}
                failed += bool(r['fails'])
                print(f'{"FAIL" if r["fails"] else "ok  "} {cn.id:26s} {describe(cn, r)} [{time.time() - tc:.1f}s]')
                if a.verbose and 'explain' in r['sift']:
                    print(f'     {plan_summary(cn, r["sift"]["explain"])}')
                for f in r['fails']:
                    print(f'     {f}')
            print(f'perf-gate: area {area} {time.time() - ta:.1f}s')
        if leak:
            rc, out = leak_end(cl, home)
            if rc:
                print(f'FAIL leak gate\n{out}')
                failed += 1
            else:
                print('ok   leak gate')
    finally:
        cl.stop()
        shutil.rmtree(home, ignore_errors=True)
    if a.json:
        Path(a.json).write_text(json.dumps(results, indent=1))
    print(f'perf-gate: {"FAIL" if failed else "ok"}: {len(results) - failed} of {len(results)} canaries passed in {time.time() - t0:.1f}s')
    return 1 if failed else 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except GateError as e:
        print(f'perf-gate: error: {e}', file=sys.stderr)
        sys.exit(2)
