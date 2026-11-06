#!/usr/bin/env python3
"""DRY performance gate: proves a refactor commit does not regress performance (see USAGE.md; entry point dry-perf-gate.sh).

    scripts/dry-perf-gate.sh [run] [options] BASE [CAND]     BASE and CAND are commits (BASE defaults to HEAD~1, CAND to HEAD of --src)
    dry-perf-gate.sh run --worktree DIR BASE          CAND = HEAD of DIR plus its uncommitted diff
    dry-perf-gate.sh run --patch FILE BASE            CAND = BASE plus the patch
    dry-perf-gate.sh baseline BASE                    measure BASE against itself: the noise floor, stored under baselines/
    dry-perf-gate.sh select BASE [CAND]               which groups and cases the changed files select (no build)
    dry-perf-gate.sh list                             groups and cases

Both sides are built with identical flags (release library, the pg_config compiler and -O level, COPT adds -D__LINE__=0 so a commit that only moves lines leaves
byte-identical objects), installed into roots of their own, and run on two clusters copied from ONE data template made by the base build (same bytes, same
statistics). Stripped object files identical => PASS-IDENTICAL without a workload. Otherwise the selected cases run interleaved base/candidate, 1 discarded + N
measured fresh backends each, and the verdict compares the minima.
"""
import argparse
import concurrent.futures as cf
import fcntl
import hashlib
import importlib.util
import json
import os
import re
import select
import shutil
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent / 'dry-perf-gate'     # cases.py, area-map.tsv, fixture-gate.sql, ctl.sh, selftest.sh, USAGE.md
W = Path(os.environ.get('GATE_ROOT', '/dev/shm/sift-perf-gate'))    # builds, data templates, run directories: tmpfs only
STATE = Path(os.environ.get('GATE_STATE', str(Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'sift-perf-gate')))   # baselines and noise floors, kept out of git
DEFAULT_SRC = os.environ.get('GATE_SRC', str(Path(__file__).resolve().parent.parent))
DEFAULT_REF = os.environ.get('GATE_TIP', 'HEAD')
COPT = '-Wno-builtin-macro-redefined -D__LINE__=0'
MAKE = ['make', 'USE_PGXS=1', 'SIFT_TESTING=', 'SIFT_LTO=', 'SANITIZE=', 'SIFT_HARDEN=', 'SIFT_BUILD_ID=gate', 'COPT=' + COPT]
RECIPE = hashlib.sha256(' '.join(MAKE).encode()).hexdigest()[:8]
DB = 'gate'
BASE_SET = ("set statement_timeout='180s'; set extra_float_digits=-5; set max_parallel_workers_per_gather=0; set max_parallel_maintenance_workers=0; set jit=off; "
            "set work_mem='256MB'; set maintenance_work_mem='256MB'; set sift.enable_agg=on;")
CONF = """fsync = off
synchronous_commit = off
full_page_writes = off
autovacuum = off
shared_buffers = 512MB
max_wal_size = 8GB
min_wal_size = 80MB
checkpoint_timeout = 1h
wal_recycle = off
max_worker_processes = 8
max_locks_per_transaction = 256
jit = off
track_io_timing = off
log_min_messages = warning
sift.col_auto_merge = off
sift.async_maintenance = off
sift.table_auto_defrag = off
"""


def log(*a):
    print('gate:', *a, file=sys.stderr, flush=True)


def sh(cmd, cwd=None, env=None, check=True, **kw):
    r = subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True, **kw)
    if check and r.returncode:
        raise SystemExit(f'gate: failed ({r.returncode}): {" ".join(map(str, cmd))[:300]}\n{(r.stdout + r.stderr)[-2500:]}')
    return r.stdout


def require_tmpfs(p):
    p = Path(p)
    p.mkdir(parents=True, exist_ok=True)
    best = ''
    for line in open('/proc/mounts'):
        mnt = line.split()
        if str(p.resolve()).startswith(mnt[1]) and len(mnt[1]) >= len(best):
            best, fs = mnt[1], mnt[2]
    if fs != 'tmpfs':
        raise SystemExit(f'gate: {p} is on {fs}, scratch must be tmpfs (/dev/shm or /tmp)')


class Lock:
    def __init__(self, name):
        self.f = open(W / name, 'w')

    def __enter__(self):
        fcntl.flock(self.f, fcntl.LOCK_EX)

    def __exit__(self, *a):
        fcntl.flock(self.f, fcntl.LOCK_UN)


# --- repository ---------------------------------------------------------------------------------------------------------------------------------------------
class Repo:
    """A repository of our own that borrows the objects of the source repo (alternates, read only): nothing is written to the source."""

    def __init__(self, src):
        self.src = str(Path(src).resolve())
        self.git_src = self.src + '/.git' if (Path(self.src) / '.git').exists() else self.src
        self.dir = W / 'repo'
        with Lock('lock-repo'):
            if not (self.dir / 'HEAD').exists() and not (self.dir / '.git').exists():
                sh(['git', 'init', '-q', str(self.dir)])
            alt = self.dir / '.git/objects/info/alternates'
            have = alt.read_text().split() if alt.exists() else []
            want = str(Path(self.git_src) / 'objects')
            if want not in have:
                alt.parent.mkdir(parents=True, exist_ok=True)
                alt.write_text('\n'.join(have + [want]) + '\n')

    def rev(self, ref):
        return sh(['git', '-C', self.src, 'rev-parse', '--verify', ref + '^{commit}']).strip()

    def git(self, *a, cwd=None):
        return sh(['git', '-C', str(cwd or self.dir), *a])


# --- builds ---------------------------------------------------------------------------------------------------------------------------------------------------
class Build:
    def __init__(self, repo, sha, patch=None, label=''):
        self.repo, self.sha, self.patch, self.label = repo, sha, patch, label
        self.key = sha[:12] + ('-' + hashlib.sha256(patch.encode()).hexdigest()[:8] if patch else '')
        self.dir = W / 'builds' / self.key
        self.tree = self.dir / 'src'
        self.root = self.dir / 'root'
        self.info = {}

    def ensure(self):
        with Lock('lock-builds'):
            marker = self.dir / 'done.json'
            if marker.exists() and json.loads(marker.read_text()).get('recipe') == RECIPE:
                self.info = json.loads(marker.read_text())
                self.touch()
                return self
            if self.dir.exists():
                self.repo.git('worktree', 'remove', '--force', str(self.tree))
            shutil.rmtree(self.dir, ignore_errors=True)
            self.dir.mkdir(parents=True)
            log(f'build {self.key} ({self.label})')
            self.repo.git('worktree', 'prune')
            self.repo.git('worktree', 'add', '-q', '--detach', str(self.tree), self.sha)
            if self.patch:
                (self.dir / 'cand.patch').write_text(self.patch)
                sh(['git', 'apply', '--whitespace=nowarn', str(self.dir / 'cand.patch')], cwd=self.tree)
            env = {**os.environ, 'CCACHE_BASEDIR': str(self.tree)}
            t0 = time.time()
            sh(MAKE + ['-j4'], cwd=self.tree, env=env)
            sh(MAKE + [f'DESTDIR={self.root}', 'install'], cwd=self.tree, env=env)
            self.info = dict(objects=self.hash_objects(), text=self.text_size(), build_s=round(time.time() - t0, 1), recipe=RECIPE)
            marker.write_text(json.dumps(self.info))
            self.evict_old()
        return self

    def touch(self):
        os.utime(self.dir)

    def text_size(self):
        out = sh(['size', str(self.tree / 'pg_sift.so')]).splitlines()[1].split()
        return int(out[0])

    def hash_objects(self):
        objs = sorted(str(p.relative_to(self.tree)) for p in self.tree.glob('src/**/*.o'))
        tmp = self.dir / 'strip'
        tmp.mkdir()

        def one(item):
            i, o = item
            dst = tmp / f'{i}.o'
            sh(['objcopy', '--strip-debug', str(self.tree / o), str(dst)])
            h = hashlib.sha256(dst.read_bytes()).hexdigest()[:16]
            dst.unlink()
            return o, h
        with cf.ThreadPoolExecutor(4) as ex:
            res = dict(ex.map(one, enumerate(objs)))
        shutil.rmtree(tmp)
        return res

    def evict_old(self, keep=4):
        dirs = sorted((d for d in (W / 'builds').iterdir() if (d / 'done.json').exists()), key=lambda d: d.stat().st_mtime, reverse=True)
        for d in dirs[keep:]:
            self.repo.git('worktree', 'remove', '--force', str(d / 'src'))
            shutil.rmtree(d)


# --- clusters -------------------------------------------------------------------------------------------------------------------------------------------------
class Cluster:
    def __init__(self, name, data, sock, port, root):
        self.name, self.data, self.sock, self.port, self.root = name, Path(data), Path(sock), port, Path(root)
        self.sharedir = str(self.root) + sh(['pg_config', '--sharedir']).strip()
        self.libdir = str(self.root) + sh(['pg_config', '--pkglibdir']).strip()

    def env(self):
        return {**os.environ, 'PGHOST': str(self.sock), 'PGPORT': str(self.port), 'PGUSER': 'postgres', 'PGDATABASE': DB, 'LC_ALL': 'C', 'PGOPTIONS': ''}

    def start(self):
        self.sock.mkdir(parents=True, exist_ok=True)
        opts = (f"-c port={self.port} -c listen_addresses='' -c unix_socket_directories={self.sock} -c extension_control_path='{self.sharedir}:$system' "
                f"-c dynamic_library_path='{self.libdir}:$libdir'")
        sh(['pg_ctl', '-D', str(self.data), '-l', str(self.data) + '.log', '-w', '-o', opts, 'start'], env=self.env())
        self.pm = int((self.data / 'postmaster.pid').read_text().split()[0])

    def stop(self):
        if (self.data / 'postmaster.pid').exists():
            sh(['pg_ctl', '-D', str(self.data), '-m', 'fast', '-w', 'stop'], env=self.env(), check=False)

    def psql(self, sql=None, file=None, args=(), db=DB):
        cmd = ['psql', '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1', '-d', db, *args]
        cmd += ['-f', str(file)] if file else ['-c', sql]
        return sh(cmd, env=self.env())


def make_template(base, rows, tpl, lab_tree):
    """initdb + lab + fixture with the BASE library; one data directory every side copies."""
    shutil.rmtree(tpl, ignore_errors=True)
    log(f'template {tpl.name}: loading the corpus lab and the gate fixture ({rows} rows)')
    sh(['initdb', '--no-sync', '-D', str(tpl), '-A', 'trust', '-U', 'postgres', '-E', 'UTF8', '--no-locale', '--no-instructions', '--wal-segsize=16'])
    with open(tpl / 'postgresql.conf', 'a') as f:
        f.write(CONF)
    c = Cluster('template', tpl, W / 'tsock', 57800, base.root)
    c.start()
    try:
        c.psql('CREATE DATABASE ' + DB, db='postgres')
        c.psql('CREATE EXTENSION pg_sift')
        lab = lab_tree / 'test/corpus/lab.sql'
        c.psql(file=lab, args=('-v', f'n_events={rows}', '-o', '/dev/null'))
        c.psql(file=HERE / 'fixture-gate.sql', args=('-v', f'rows={rows}', '-o', '/dev/null'))
        c.psql('CHECKPOINT')
    finally:
        c.stop()
    for f in ('postmaster.opts', 'postmaster.pid'):
        (tpl / f).unlink(missing_ok=True)
    (tpl / 'gate.complete').write_text('ok\n')


# --- registry ---------------------------------------------------------------------------------------------------------------------------------------------------
def load_registry(tree):
    def mod(name, path):
        spec = importlib.util.spec_from_file_location(name, path)
        m = importlib.util.module_from_spec(spec)
        sys.modules[name] = m
        spec.loader.exec_module(m)
        return m
    sys.path.insert(0, str(tree / 'bench/perf'))
    corpus = mod('corpus_run', tree / 'test/corpus/run.py')
    catalog = mod('perf_catalog', tree / 'bench/perf/catalog.py')
    sys.path.pop(0)
    cc = {c['slug']: dict(c, sql=corpus.statement(c)) for c in corpus.parse(str(tree / 'test/corpus/cases.sql'))}
    cat = {c.name: c for c in catalog.CASES}
    spec = importlib.util.spec_from_file_location('gate_cases', HERE / 'cases.py')
    gc = importlib.util.module_from_spec(spec)
    sys.modules['gate_cases'] = gc
    spec.loader.exec_module(gc)
    return gc.build(cc, cat)


def area_rows():
    rows = []
    for line in (HERE / 'area-map.tsv').read_text().splitlines():
        if line and not line.startswith('#'):
            rx, groups = line.split('\t')
            rows.append((re.compile(rx), [] if groups == '-' else groups.split(',')))
    return rows


def select_groups(paths):
    rows, groups, unmapped = area_rows(), set(), []
    for p in paths:
        for rx, gs in rows:
            if rx.search(p):
                groups.update(gs)
                break
        else:
            groups.add('smoke')
            unmapped.append(p)
    return groups, unmapped


# --- one measured run ---------------------------------------------------------------------------------------------------------------------------------------
S_SQL = ("select '@@%s|' || coalesce((select sum(hits) from pg_stat_get_backend_io(pg_backend_pid()) where object = 'relation'), 0) || '|' || "
         "coalesce((select sum(reads) from pg_stat_get_backend_io(pg_backend_pid()) where object = 'relation'), 0) || '|' || "
         "coalesce((select sum(writes) from pg_stat_get_backend_io(pg_backend_pid()) where object = 'relation'), 0) || '|' || "
         "coalesce((select sum(extends) from pg_stat_get_backend_io(pg_backend_pid()) where object = 'relation'), 0) || '|' || pg_current_wal_insert_lsn();")


def semi(s):
    s = s.rstrip()
    return s if s.endswith(';') else s + ';'


def script(case, ctl, warm=True):
    L = ['\\set ON_ERROR_STOP on', BASE_SET, case.settings, '\\o /dev/null']
    L += [semi(p) for p in case.pre]
    if case.kind == 'warm' and warm:
        L.append(semi(case.sql))
    if case.kind == 'evict':
        L.append('select count(*) from pg_buffercache_evict_all();')
    L += ['\\o', 'select pg_stat_force_next_flush();', S_SQL % 'S0', '\\echo @@R', f'\\! {ctl} enable', '\\timing on', semi(case.sql), '\\timing off', f'\\! {ctl} disable',
          '\\echo @@E', 'select pg_stat_force_next_flush();', S_SQL % 'S1', '\\o /dev/null']
    L += [semi(p) for p in case.post]
    return '\n'.join(L) + '\n'


def lsn(s):
    hi, lo = s.split('/')
    return (int(hi, 16) << 32) + int(lo, 16)


def wait_ready(perf, ctl, ack):
    """perf has attached to the postmaster (a backend forked before that would not be counted) once it acknowledges a ping."""
    fd_ack = os.open(ack, os.O_RDONLY | os.O_NONBLOCK)
    try:
        deadline = time.time() + 15
        while True:
            try:
                fd_ctl = os.open(ctl, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError:
                if perf.poll() is not None or time.time() > deadline:
                    raise SystemExit('gate: perf stat did not start: ' + (perf.stderr.read() if perf.poll() is not None else 'timeout'))
                time.sleep(0.01)
        os.write(fd_ctl, b'ping\n')
        if not select.select([fd_ack], [], [], 15)[0]:
            raise SystemExit('gate: perf stat did not acknowledge the control ping')
        os.read(fd_ack, 64)
        os.close(fd_ctl)
    finally:
        os.close(fd_ack)


def run_once(cl, case, rundir, tag):
    ctl, ack, out = rundir / 'ctl', rundir / 'ack', rundir / 'perf.out'
    for f in (ctl, ack, out):
        f.unlink(missing_ok=True)
    os.mkfifo(ctl)
    os.mkfifo(ack)
    spath = rundir / 'run.sql'
    spath.write_text(script(case, HERE / 'ctl.sh'))
    env = {**cl.env(), 'GATE_CTL': str(ctl), 'GATE_ACK': str(ack)}
    perf = subprocess.Popen(['perf', 'stat', '-p', str(cl.pm), '-e', 'instructions:u,cycles:u', '-x,', '-o', str(out), '--control', f'fifo:{ctl},{ack}', '--delay=-1'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(perf, ctl, ack)
        p = subprocess.run(['psql', '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1', '-d', DB, '-f', str(spath)], env=env, capture_output=True, text=True, timeout=600)
    finally:
        perf.send_signal(signal.SIGINT)
        try:
            perf.wait(timeout=20)
        except subprocess.TimeoutExpired:
            perf.kill()
            perf.wait()
    r = dict(tag=tag, ok=p.returncode == 0)
    if p.returncode:
        r['error'] = (p.stderr.strip().splitlines() or ['psql failed'])[-1][:200]
        return r
    text = p.stdout
    (rundir / 'last.out').write_text(text + '\n--- stderr\n' + p.stderr)
    s0 = re.search(r'@@S0\|(\d+)\|(\d+)\|(\d+)\|(\d+)\|(\S+)', text)
    s1 = re.search(r'@@S1\|(\d+)\|(\d+)\|(\d+)\|(\d+)\|(\S+)', text)
    body = text.split('@@R\n', 1)[1].split('@@E', 1)[0] if '@@R' in text else ''
    ms = re.search(r'^Time: ([0-9.]+) ms', body, re.M)
    rows = sorted(l for l in body.splitlines() if not l.startswith('Time: '))
    vals = {}
    for line in out.read_text().splitlines():
        f = line.split(',')
        if len(f) > 2 and f[2].startswith(('instructions', 'cycles')):
            vals[f[2].split(':')[0]] = int(f[0]) if f[0].isdigit() else None
    if not (s0 and s1 and ms) or vals.get('instructions') is None:
        r.update(ok=False, error=f'no counters (s0={bool(s0)} s1={bool(s1)} time={bool(ms)} perf={vals}) ' + perf.stderr.read().strip()[-150:])
        return r
    d = [int(b) - int(a) for a, b in zip(s0.groups()[:4], s1.groups()[:4])]
    r.update(instr=vals['instructions'], cycles=vals['cycles'], ms=float(ms.group(1)), hits=d[0], reads=d[1], writes=d[2], extends=d[3],
             wal=lsn(s1.group(5)) - lsn(s0.group(5)), rows=hashlib.md5('\n'.join(rows).encode()).hexdigest()[:12], nrows=len(rows))
    return r


def plan_of(cl, case, rundir):
    if not case.plan and not case.expect:
        return None
    out = cl.psql(BASE_SET + case.settings + ' explain (costs off) ' + case.sql.rstrip(';') + ';')
    if case.expect and not re.search(case.expect, out):
        return 'MISSING:' + case.expect
    return hashlib.md5(out.encode()).hexdigest()[:12]


# --- statistics and verdicts ---------------------------------------------------------------------------------------------------------------------------------
def spread(xs):
    return (max(xs) - min(xs)) / min(xs) * 100 if xs and min(xs) else 0.0


def summarize(runs):
    ok = [r for r in runs if r['ok']]
    if not ok:
        return dict(error=runs[-1].get('error', 'failed') if runs else 'no runs', n=0)
    m = {k: min(r[k] for r in ok) for k in ('instr', 'cycles', 'ms')}
    for k in ('hits', 'reads', 'writes', 'extends', 'wal'):
        m[k] = min(r[k] for r in ok)
        m[k + '_spread'] = max(r[k] for r in ok) - m[k]
    m.update(instr_spread=spread([r['instr'] for r in ok]), cycles_spread=spread([r['cycles'] for r in ok]), n=len(ok),
             rows=sorted({r['rows'] for r in ok}), nrows=ok[0]['nrows'])
    return m


def judge(b, c, cfg, noise, par=False):
    """Verdict of one case from the summaries of the two sides: (status, [reasons], numbers)."""
    why = []
    if 'error' in b or 'error' in c:
        return 'ERROR' if 'error' in b and 'error' in c else 'FAIL', [f"base: {b.get('error', 'ok')}; cand: {c.get('error', 'ok')}"], {}
    di = (c['instr'] - b['instr']) / b['instr'] * 100
    dc = (c['cycles'] - b['cycles']) / b['cycles'] * 100
    abs_ok = c['instr'] - b['instr'] <= cfg.instr_abs
    thr_c = max(cfg.max_cycles_pct, 1.5 * noise.get('cycles', 0))
    if b['rows'] != c['rows'] or len(c['rows']) > 1 or len(b['rows']) > 1:
        why.append('rows differ')
    max_instr = max(cfg.max_instr_pct, cfg.par_instr_pct) if par else cfg.max_instr_pct
    if di > max_instr and not abs_ok:
        why.append(f'instructions {di:+.2f}% > {max_instr}%')
    warn = []
    if dc > thr_c and b['cycles'] >= cfg.min_cycles:
        if abs(di) <= cfg.layout_instr_pct and dc <= cfg.layout_cycles_pct:
            # the same instructions and blocks, a moderate cycles difference: the machine code moved (alignment, cache sets), not the work; recorded, not failed
            warn.append(f'cycles {dc:+.2f}% with equal instructions ({di:+.3f}%): code layout or noise')
        else:
            why.append(f'cycles {dc:+.2f}% > {thr_c:.2f}%')
    for k in ('hits', 'reads', 'wal'):
        tol = max(cfg.wal_tol, 2 * noise.get('wal', 0)) if k == 'wal' else cfg.blocks_tol
        if par:       # the leader's blocks and the WAL of the workers vary with the split of the work
            tol = max(tol, 16, b[k] * (cfg.par_wal_pct if k == 'wal' else cfg.par_blocks_pct) / 100)
        if abs(c[k] - b[k]) > tol:
            why.append(f'{k} {b[k]} -> {c[k]}')
    return ('FAIL' if why else 'WARN' if warn else 'PASS'), why + warn, dict(d_instr=di, d_cycles=dc, thr_cycles=thr_c)


# --- driver ---------------------------------------------------------------------------------------------------------------------------------------------------
def measure_cases(cases, sides, runs, rundir, retry_cycles=True, cfg=None, noise=None):
    """cases measured interleaved on `sides` = {name: Cluster}; returns {case: {side: summary, plan: {side: md5}}}."""
    res, names = {}, list(sides)
    for case in cases:
        t0 = time.time()
        raw = {n: [] for n in names}

        def rounds(k, first):
            for r in range(k):
                order = names if (r + first) % 2 == 0 else names[::-1]
                for n in order:
                    raw[n].append(run_once(sides[n], case, rundir, f'{case.id}/{n}/{r}'))
        for n in names:
            raw[n].append(run_once(sides[n], case, rundir, f'{case.id}/{n}/warmup'))
        raw = {n: [] for n in names}
        rounds(runs, 0)
        summ = {n: summarize(raw[n]) for n in names}
        if retry_cycles and cfg and len(names) == 2 and all('error' not in summ[n] for n in names):
            for attempt in range(2):
                st, why, _ = judge(summ[names[0]], summ[names[1]], cfg, case_noise(noise, case.id), case.par)
                if st != 'FAIL' or not all(w.startswith('cycles') for w in why):
                    break
                rounds(runs, attempt + 1)
                summ = {n: summarize(raw[n]) for n in names}
        plans = {n: plan_of(sides[n], case, rundir) for n in names}
        res[case.id] = dict(sides=summ, plan=plans, secs=round(time.time() - t0, 1), runs={n: [{k: v for k, v in r.items() if k != 'tag'} for r in raw[n]] for n in names})
        log(f'  {case.id:34s} {time.time() - t0:5.1f}s ' + ' '.join(f"{n}:{summ[n].get('instr', 0) / 1e6:.2f}Minstr" for n in names))
    return res


def fmt_m(x):
    return f'{x / 1e6:,.2f}M'


def report(verdicts, res, args, meta):
    lines, fails = [], 0
    hdr = f"{'case':34s} {'instr base -> cand':>26s} {'d%':>7s} {'cycles d%':>9s} {'noise%':>6s} {'ms b/c':>12s} {'hit+read b -> c':>22s} {'wal b -> c':>16s} verdict"
    lines.append(hdr)
    for cid, (st, why, num) in verdicts.items():
        r = res[cid]
        b, c = r['sides'].get('base', {}), r['sides'].get('cand', {})
        if 'error' in b or 'error' in c:
            lines.append(f'{cid:34s} ERROR {b.get("error", "")} | {c.get("error", "")}')
            fails += st == 'FAIL'
            continue
        lines.append(f"{cid:34s} {fmt_m(b['instr']):>12s} -> {fmt_m(c['instr']):>10s} {num['d_instr']:+7.3f} {num['d_cycles']:+9.2f} {max(b['cycles_spread'], c['cycles_spread']):6.2f} "
                     f"{b['ms']:6.1f}/{c['ms']:<6.1f} {b['hits']:>9d}+{b['reads']:<5d}-> {c['hits']:>9d}+{c['reads']:<4d} {b['wal']:>7d}->{c['wal']:<7d} {st}" + (' ' + '; '.join(why) if why else ''))
        fails += st == 'FAIL'
    return lines, fails


def evidence_line(verdicts, res, meta, status):
    ok = [cid for cid in verdicts if 'd_instr' in verdicts[cid][2]]
    if not ok:
        return f"perf-gate {meta['base']} -> {meta['cand']}: {status}"
    bi = sum(res[c]['sides']['base']['instr'] for c in ok)
    ci = sum(res[c]['sides']['cand']['instr'] for c in ok)
    bc = sum(res[c]['sides']['base']['cycles'] for c in ok)
    cc = sum(res[c]['sides']['cand']['cycles'] for c in ok)
    blocks = lambda s: sum(res[c]['sides'][s]['hits'] + res[c]['sides'][s]['reads'] for c in ok)
    wal = lambda s: sum(res[c]['sides'][s]['wal'] for c in ok)
    worst = max(ok, key=lambda c: verdicts[c][2]['d_instr'])
    noise = max(max(res[c]['sides']['base']['cycles_spread'], res[c]['sides']['cand']['cycles_spread']) for c in ok)
    size = meta['cand_text'] - meta['base_text']
    return (f"perf-gate {meta['base']} -> {meta['cand']} ({len(ok)} cases, groups {','.join(sorted(meta['groups']))}): "
            f"instructions:u {fmt_m(bi)} -> {fmt_m(ci)} ({(ci - bi) / bi * 100:+.3f}%, worst {worst} {verdicts[worst][2]['d_instr']:+.3f}%), "
            f"cycles:u {fmt_m(bc)} -> {fmt_m(cc)} ({(cc - bc) / bc * 100:+.2f}%, noise {noise:.1f}%), blocks {blocks('base')} -> {blocks('cand')}, WAL {wal('base')} -> {wal('cand')} B, "
            f"text {size:+d} B, rows identical: {status}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=('run', 'baseline', 'select', 'list'), nargs='?', default='run')
    ap.add_argument('base', nargs='?')
    ap.add_argument('cand', nargs='?')
    ap.add_argument('--src', default=DEFAULT_SRC, help='repository holding the commits (default: the repository of this script, or GATE_SRC)')
    ap.add_argument('--worktree', help='candidate = HEAD of this checkout plus its uncommitted changes')
    ap.add_argument('--patch', help='candidate = BASE plus this patch')
    ap.add_argument('--groups', help='comma list of groups instead of the selection by changed files')
    ap.add_argument('--cases', help='comma list of case ids (overrides the groups)')
    ap.add_argument('--runs', type=int, default=9, help='measured runs per side per case after one discarded run (default 9)')
    ap.add_argument('--rows', type=int, default=100000, help='corpus events and fixture documents (default 100000)')
    ap.add_argument('--max-instr-pct', type=float, default=0.5)
    ap.add_argument('--instr-abs', type=int, default=20000, help='ignore an instruction increase below this many (default 20000)')
    ap.add_argument('--max-cycles-pct', type=float, default=2.0)
    ap.add_argument('--min-cycles', type=int, default=5_000_000, help='cycles are judged only above this many in the base (shorter statements: instructions decide)')
    ap.add_argument('--layout-instr-pct', type=float, default=0.1, help='a cycles excess with instructions within this percent is a WARN (code layout), not a FAIL (default 0.1)')
    ap.add_argument('--layout-cycles-pct', type=float, default=15.0, help='...unless the cycles excess is above this (default 15)')
    ap.add_argument('--par-instr-pct', type=float, default=1.0, help='instruction limit of the parallel (workers) cases (default 1.0)')
    ap.add_argument('--par-blocks-pct', type=float, default=10.0, help='the leader counts only its own share of the blocks (default 10)')
    ap.add_argument('--par-wal-pct', type=float, default=3.0)
    ap.add_argument('--blocks-tol', type=int, default=0)
    ap.add_argument('--wal-tol', type=int, default=0)
    ap.add_argument('--max-size-pct', type=float, default=0.1)
    ap.add_argument('--allow-plan-change', action='store_true')
    ap.add_argument('--force-workload', action='store_true', help='run the cases even when the stripped objects are identical')
    ap.add_argument('--json', help='write the full result here')
    ap.add_argument('--keep', action='store_true', help='keep the run directory')
    a = ap.parse_args()

    require_tmpfs(W)
    for k in ('PGHOST', 'PGPORT', 'PGDATABASE', 'PGSERVICE'):
        os.environ.pop(k, None)
    os.environ['PGHOST'], os.environ['PGPORT'] = '/nonexistent', '1'   # fails closed: a psql without a cluster of ours never reaches the system server
    repo = Repo(a.src)
    if a.cmd == 'list':
        reg = load_registry(repo_tree(repo))
        for c in reg:
            print(f"{c.id:36s} {c.kind:6s} {c.src:8s} {','.join(c.groups)}")
        return 0
    base_sha = repo.rev(a.base or 'HEAD~1')
    patch = None
    if a.worktree:
        wt = Path(a.worktree).resolve()
        cand_sha = sh(['git', '-C', str(wt), 'rev-parse', 'HEAD']).strip()
        patch = sh(['git', '-C', str(wt), 'diff', '--binary', 'HEAD']) or None
        if sh(['git', '-C', str(wt), 'ls-files', '-o', '--exclude-standard']).strip():
            log('warning: untracked files in the worktree are not part of the candidate (git add -N them)')
    elif a.patch:
        cand_sha, patch = base_sha, Path(a.patch).read_text()
    else:
        cand_sha = repo.rev(a.cand or 'HEAD')
    if a.cmd == 'baseline':
        cand_sha, patch = base_sha, None

    base = Build(repo, base_sha, label='base')
    cand = Build(repo, cand_sha, patch, label='candidate') if (cand_sha != base_sha or patch) else base
    if a.cmd == 'select':
        paths = changed_paths(repo, base_sha, cand_sha, patch)
        groups, un = select_groups(paths)
        print('changed:', len(paths), 'files; groups:', ','.join(sorted(groups)) or '-', '; unmapped:', un[:5])
        return 0

    (W / 'builds').mkdir(parents=True, exist_ok=True)
    (W / 'data').mkdir(exist_ok=True)
    base.ensure()
    if cand is not base:
        cand.ensure()
    diff = sorted(o for o in set(base.info['objects']) | set(cand.info['objects']) if base.info['objects'].get(o) != cand.info['objects'].get(o))
    meta = dict(base=base.key, cand=cand.key, base_text=base.info['text'], cand_text=cand.info['text'], differing_objects=diff)
    sql_changed = [p for p in changed_paths(repo, base_sha, cand_sha, patch) if p.startswith('sql/pg_sift--') or p == 'pg_sift.control']
    if a.cmd == 'run' and not diff and not sql_changed and not a.force_workload:
        line = f"perf-gate {base.key} -> {cand.key}: PASS-IDENTICAL (all {len(base.info['objects'])} stripped object files byte-identical, text {cand.info['text'] - base.info['text']:+d} B)"
        print(line)
        out = dict(status='PASS-IDENTICAL', meta=meta, evidence=line)
        if a.json:
            Path(a.json).write_text(json.dumps(out, indent=1))
        return 0

    paths = changed_paths(repo, base_sha, cand_sha, patch) + [o[:-2] + '.c' for o in diff]
    reg_tree = base.tree if (base.tree / 'test/corpus/run.py').exists() and (base.tree / 'bench/perf/catalog.py').exists() else repo_tree(repo)
    if reg_tree != base.tree:
        log('the base predates test/corpus: registry and lab come from', reg_tree)
    registry = load_registry(reg_tree)
    byid = {c.id: c for c in registry}
    if a.cases:
        groups, selected = {'cases'}, [byid[x] for x in a.cases.split(',')]
    elif a.cmd == 'baseline':
        groups, selected = {'all'}, registry
    else:
        groups, un = (set(a.groups.split(',')), []) if a.groups else select_groups(paths)
        if un:
            log('unmapped files fall back to smoke:', ' '.join(un[:6]))
        selected = [c for c in registry if set(c.groups) & groups]
    meta['groups'] = sorted(groups)
    log(f'{len(selected)} cases, groups {",".join(sorted(groups)) or "-"}; differing objects: {len(diff)}')
    if not selected:
        print(f'perf-gate {base.key} -> {cand.key}: PASS (no build input changed: {len(diff)} objects differ, no group selected)')
        return 0

    # the template depends on what the base library writes (its stripped objects), the extension scripts and the fixture, not on the commit id
    fp = hashlib.sha256(json.dumps(sorted(base.info['objects'].items())).encode())
    for f in sorted(base.tree.glob('sql/pg_sift--*.sql')) + [base.tree / 'pg_sift.control', HERE / 'fixture-gate.sql', base.tree / 'test/corpus/lab.sql']:
        fp.update(f.read_bytes())
    tpl = W / 'data' / f'tpl-{fp.hexdigest()[:12]}-r{a.rows}'
    with Lock('lock-template'):
        if not (tpl / 'gate.complete').exists():
            make_template(base, a.rows, tpl, reg_tree)
        os.utime(tpl)
        olds = sorted((p for p in (W / 'data').iterdir() if p.name.startswith('tpl-') and p.is_dir()), key=lambda p: p.stat().st_mtime, reverse=True)
        for old in olds[3:]:
            shutil.rmtree(old)
            Path(str(old) + '.log').unlink(missing_ok=True)
    rundir = W / 'run' / f'{os.getpid()}'
    shutil.rmtree(rundir, ignore_errors=True)
    rundir.mkdir(parents=True)
    sides = {'base': Cluster('base', rundir / 'data-base', rundir / 'sock-b', 57810, base.root), 'cand': Cluster('cand', rundir / 'data-cand', rundir / 'sock-c', 57811, cand.root)}
    try:
        for s in sides.values():
            shutil.copytree(tpl, s.data, symlinks=True)
            s.start()
        t0 = time.time()
        noise = load_noise(base.key)
        cfg = a
        res = measure_cases(selected, sides, a.runs, rundir, True, cfg, noise)
        verdicts = {}
        for c in selected:
            r = res[c.id]
            st, why, num = judge(r['sides']['base'], r['sides']['cand'], cfg, case_noise(noise, c.id), c.par)
            missing = [p for p in r['plan'].values() if p and p.startswith('MISSING:')]
            if missing:
                st, why = 'FAIL', why + [f'the plan lacks {missing[0][8:]}: the case would measure another plan']
            elif r['plan']['base'] != r['plan']['cand'] and not a.allow_plan_change:
                st, why = 'FAIL', why + ['plan differs']
            verdicts[c.id] = (st, why, num)
        lines, fails = report(verdicts, res, a, meta)
        size_pct = (cand.info['text'] - base.info['text']) / base.info['text'] * 100
        removes = net_removed(repo, base_sha, cand, patch)
        size_msg = f"binary text {base.info['text']} -> {cand.info['text']} B ({size_pct:+.3f}%)"
        if size_pct > a.max_size_pct and not removes:
            fails += 1
            size_msg += f' > {a.max_size_pct}% and the commit adds more lines than it removes'
        fails += sum(1 for v in verdicts.values() if v[0] == 'ERROR')
        warns = [cid for cid, v in verdicts.items() if v[0] == 'WARN']
        status = 'FAIL' if fails else 'PASS' + (f' ({len(warns)} WARN: ' + ', '.join(f"{c} {verdicts[c][2]['d_cycles']:+.1f}% cycles, instructions equal" for c in warns) + ')' if warns else '')
        print('\n'.join(lines))
        print(size_msg)
        ev = evidence_line(verdicts, res, meta, status)
        print(ev)
        out = dict(status=status, meta=meta, evidence=ev, verdicts={k: dict(status=v[0], why=v[1], **v[2]) for k, v in verdicts.items()}, res=res, size=size_msg,
                   host=host_info(), seconds=round(time.time() - t0, 1), rows=a.rows, runs=a.runs)
        if a.cmd == 'baseline':
            save_baseline(base.key, out, partial=bool(a.cases))
        if a.json:
            Path(a.json).write_text(json.dumps(out, indent=1))
        return 1 if fails else 0
    finally:
        for s in sides.values():
            s.stop()
        if not a.keep:
            shutil.rmtree(rundir, ignore_errors=True)


def repo_tree(repo):
    tip = repo.rev(DEFAULT_REF)
    b = Build(repo, tip)
    b.ensure()
    return b.tree


def changed_paths(repo, base_sha, cand_sha, patch):
    if patch:
        return [l.split(' b/', 1)[1] for l in patch.splitlines() if l.startswith('diff --git ')]
    return [p for p in repo.git('diff', '--name-only', '--no-renames', base_sha, cand_sha).splitlines() if p]


def net_removed(repo, base_sha, cand, patch):
    if patch:
        add = sum(1 for l in patch.splitlines() if l.startswith('+') and not l.startswith('+++'))
        rem = sum(1 for l in patch.splitlines() if l.startswith('-') and not l.startswith('---'))
        return rem > add
    num = repo.git('diff', '--numstat', '--no-renames', base_sha, cand.sha, '--', 'src').split()
    adds, rems = sum(int(x) for x in num[0::3] if x.isdigit()), sum(int(x) for x in num[1::3] if x.isdigit())
    return rems > adds


def host_info():
    cpu = next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name')), '?')
    psi = {k: open(f'/proc/pressure/{k}').readline().split()[1] for k in ('cpu', 'io')}
    return dict(cpu=cpu, nproc=os.cpu_count(), load=os.getloadavg(), psi=psi, date=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))


def case_noise(noise, cid):
    return noise.get(cid) or noise.get('_machine') or {'cycles': 8.0}


def load_noise(key):
    p = STATE / 'baselines' / 'noise.json'
    raw = json.loads(p.read_text()) if p.exists() else {}
    return {k: v if isinstance(v, dict) else {'cycles': v} for k, v in raw.items()}


def save_baseline(key, out, partial=False):
    d = STATE / 'baselines'
    d.mkdir(parents=True, exist_ok=True)
    noise = {}
    for cid, v in out['res'].items():
        b, c = v['sides']['base'], v['sides']['cand']
        if 'error' not in b and 'error' not in c:
            noise[cid] = dict(cycles=round(abs(c['cycles'] - b['cycles']) / b['cycles'] * 100, 3), wal=abs(c['wal'] - b['wal']),
                              instr_pct=round(abs(c['instr'] - b['instr']) / b['instr'] * 100, 4))
    old = json.loads((d / 'noise.json').read_text()) if (d / 'noise.json').exists() else {}
    big = sorted(v['cycles'] for cid, v in noise.items() if out['res'][cid]['sides']['base']['cycles'] >= 5_000_000)
    if partial:
        noise = {**old, **noise}      # a few cases re-measured: the other floors stay
    else:
        noise['_machine'] = dict(cycles=big[int(0.9 * len(big))] if big else 8.0, wal=0, instr_pct=0)
    (d / 'noise.json').write_text(json.dumps(noise, indent=1, sort_keys=True))
    if partial:
        return
    slim = dict(base=out['meta']['base'], host=out['host'], rows=out['rows'], runs=out['runs'], noise_pct=noise, text=out['meta']['base_text'],
                cases={cid: v['sides']['base'] for cid, v in out['res'].items()}, plan={cid: v['plan']['base'] for cid, v in out['res'].items()})
    (d / f"{out['meta']['base']}.json").write_text(json.dumps(slim, indent=1, sort_keys=True))
    with open(d / f"{out['meta']['base']}.tsv", 'w') as f:
        f.write('case\tinstr\tcycles\tms\thits\treads\twrites\textends\twal\tinstr_spread_pct\tcycles_spread_pct\tcycles_noise_pct\trows\n')
        for cid, s in slim['cases'].items():
            if 'error' not in s:
                f.write('\t'.join(map(str, [cid, s['instr'], s['cycles'], s['ms'], s['hits'], s['reads'], s['writes'], s['extends'], s['wal'], round(s['instr_spread'], 4),
                                            round(s['cycles_spread'], 3), noise.get(cid, {}).get('cycles', ''), s['rows'][0]])) + '\n')


if __name__ == '__main__':
    sys.exit(main())
