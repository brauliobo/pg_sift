#!/usr/bin/env python3
"""Exact-revision PostgreSQL 18 paired elapsed-time performance gate."""
import argparse
import ctypes
import contextlib
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'bench/perf'))
sys.path.insert(0, str(ROOT / 'bench/steps'))
import cell
from catalog import CASES, select

INITIAL_PAIRS = 6
READ_CONFIRMATION_PAIRS = 16
MUTATION_CONFIRMATION_PAIRS = 6
# An inconclusive confirmation (neither inside the margin nor clearly outside it) is a noise-limited case: one more fresh block of the
# widest supported size decides it when the budget still covers it and the cases after it. Measured: a mutation case with its blocks
# runs 25-50 s on the hosted runner, a settled 6-pair INSERT block has a pair log-ratio SD of 0.09 (95% half-width 0.09 ~ the margin).
EXTENSION_PAIRS = 16
CASE_RESERVE_SECONDS = 30
TARGET = 0.150
MATERIAL = 1.10
T95 = {6:  2.570581835636305,
       16: 2.131449545559323}
DEADLINE = math.inf
PROCESS_ORIGIN = time.monotonic()
BATCH_WALL_LIMIT = 1.0
INSERT_PILOT = 2048
INSERT_GROWTH_CAP = 2048
INSERT_HISTORY_CAP = 5
CPU_PILOT_ROWS = 32
CPU_PILOT_ITERATIONS = 16
CPU_ITERATION_CAP = 1024
SETTINGS = '''SET statement_timeout='30s'; SET max_parallel_workers_per_gather=0;
SET max_parallel_maintenance_workers=0; SET jit=off; SET sift.async_maintenance=off;
SET sift.table_write_buffer_limit=2048; SET cpu_tuple_cost=0.5; SET enable_seqscan=off; SET sift.enable_agg=on; SET sift.agg_preagg=on'''


class Inconclusive(Exception):
    pass


class BudgetExpired(Inconclusive):
    pass


def interval(pairs):
    if len(pairs) not in T95:
        raise ValueError('a block requires exactly 6 or 16 independent pairs')
    if any(not math.isfinite(x) or x <= 0 for p in pairs for x in p):
        raise ValueError('invalid elapsed sample')
    logs = [math.log(b / a) for a, b in pairs]
    mu = statistics.mean(logs)
    half = T95[len(pairs)] * statistics.stdev(logs) / math.sqrt(len(logs))
    return dict(n=len(logs), degrees_freedom=len(logs)-1, t_critical=T95[len(logs)], ratio=math.exp(mu), lower=math.exp(mu-half),
                upper=math.exp(mu+half), log_sd=statistics.stdev(logs),
                baseline_median=statistics.median(a for a, _ in pairs),
                candidate_median=statistics.median(b for _, b in pairs),
                baseline_range=[min(a for a, _ in pairs), max(a for a, _ in pairs)],
                candidate_range=[min(b for _, b in pairs), max(b for _, b in pairs)])


def verdict(block, confirmation=False):
    if block['upper'] <= MATERIAL:
        return 'pass'
    if confirmation and block['lower'] > MATERIAL:
        return 'regression'
    return 'inconclusive' if confirmation else 'suspect'


def extension_fits(confirmation, deadline, reserve):
    cost = EXTENSION_PAIRS * confirmation['runtime_seconds'] / confirmation['n']
    return dict(fits=deadline - time.monotonic() > cost + reserve, predicted_seconds=cost, reserve_seconds=reserve)


def require_equal(left, right, label):
    if left != right:
        raise ValueError(f'{label}: correctness differs: {left!r} != {right!r}')


class SQL(cell.Connection):
    """Reuse cell's libpq ABI/connection; copy all rows only for untimed checks."""
    def rows(self, sql):
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('global runtime budget exhausted before SQL validation')
        lib = self.lib
        result = lib.PQexec(self.handle, sql.encode())
        if not result:
            raise RuntimeError('libpq returned no result')
        try:
            if lib.PQresultStatus(result) not in (cell.COMMAND_OK, cell.TUPLES_OK):
                raise RuntimeError(lib.PQresultErrorMessage(result).decode().strip())
            if time.monotonic()>=DEADLINE:
                raise BudgetExpired('global runtime budget exhausted after SQL validation')
            return [[None if lib.PQgetisnull(result, r, c) else lib.PQgetvalue(result, r, c).decode()
                     for c in range(lib.PQnfields(result))] for r in range(lib.PQntuples(result))]
        finally:
            lib.PQclear(result)

    def exec(self, sql):
        # Execute separately: PQexec only returns the last result of a multi-command string.
        # Fixture scripts use psql ON_ERROR_STOP; runtime SQL is deliberately one statement.
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('global runtime budget exhausted before SQL operation')
        result = self.run(sql.encode())
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('global runtime budget exhausted after SQL operation')
        return result

    def script(self, text):
        for sql in text.split(';'):
            if sql.strip():
                self.exec(sql)


def command(argv, *, cwd=None, env=None, log=None, bounded=True):
    timeout = DEADLINE-time.monotonic() if bounded else 60
    if timeout <= 0:
        raise BudgetExpired('global runtime budget exhausted')
    proc = subprocess.Popen([str(a) for a in argv], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)
    try:
        stdout, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid,signal.SIGTERM)
        try:
            stdout, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid,signal.SIGKILL)
            stdout, _ = proc.communicate()
        if log:
            with Path(log).open('a') as f:
                f.write(stdout)
        raise BudgetExpired('global runtime budget exhausted during '+str(argv[0]))
    result = subprocess.CompletedProcess(argv,proc.returncode,stdout)
    if log:
        with Path(log).open('a') as f:
            f.write('$ ' + ' '.join(map(str, argv)) + '\n' + result.stdout)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} exited {result.returncode}: {result.stdout[-3000:]}')
    return result.stdout.strip()


def exact_sha(ref):
    if not re.fullmatch('[0-9a-f]{40}', ref):
        raise ValueError('refs must be exact full 40-character commit SHAs')
    got = command(['git', 'rev-parse', ref+'^{commit}'], cwd=ROOT)
    require_equal(ref, got, 'commit identity')
    return got


# Socket endpoints only: data/build/TMPDIR remain on the caller's test backing.
SOCKET_PARENT = Path('/tmp')


def validate_socket_path(directory, port):
    if sys.platform.startswith('linux'):
        limit=107  # sockaddr_un.sun_path[108], including the terminating NUL.
    elif sys.platform.startswith(('darwin','freebsd','openbsd','netbsd','dragonfly')):
        limit=103  # These platforms use sun_path[104].
    else:
        raise ValueError('unsupported platform for UNIX socket pathname contract: '+sys.platform)
    if type(port) is not int or not 1<=port<=65535:
        raise ValueError('invalid PostgreSQL socket port')
    path=Path(directory)/f'.s.PGSQL.{port}'
    length=len(os.fsencode(path))
    if not path.is_absolute() or length>limit:
        raise ValueError(f'UNIX socket path is {length} bytes; {sys.platform} maximum {limit} pathname bytes plus NUL: {path}; use a shorter socket parent')
    return dict(path=str(path),path_bytes=length,pathname_limit_bytes=limit,port=port,
                platform=sys.platform,encoding=sys.getfilesystemencoding(),
                parent_policy='private0700 socket-only directory under /tmp; independent of scratch-root and TMPDIR')


def allocate_socket_directory(port):
    directory=Path(tempfile.mkdtemp(prefix='pgs-',dir=SOCKET_PARENT))
    try:
        directory.chmod(0o700)
        evidence=validate_socket_path(directory,port)
    except Exception:
        shutil.rmtree(directory)  # No cluster can exist before this constructor returns.
        raise
    return directory,evidence


class Revision:
    def __init__(self, name, sha, tree, scratch, output, pg_config, port):
        self.name, self.sha = name, sha
        self.base = scratch / name
        self.base.mkdir()
        self.tree = self.base / 'source'
        self.data = self.base / 'cluster'
        self.stage = self.base / 'stage'
        self.port = port
        self.conn = None
        self.started = False
        self.pg_config = pg_config
        self.log = output / f'{name}-setup.log'
        source = Path(tree).resolve() if tree else ROOT
        if tree:
            require_equal(command(['git','rev-parse','HEAD'],cwd=source), sha, name+' tree HEAD')
            if command(['git','status','--porcelain','--untracked-files=no'],cwd=source):
                raise ValueError(f'{name} tree has tracked modifications')
        # git archive ignores untracked build products and avoids inherited object files.
        archive = self.base / 'source.tar'
        command(['git','archive','--format=tar','-o',archive,sha],cwd=source,log=self.log)
        self.tree.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(self.tree, filter='data')
        archive.unlink()
        self.socket,self.socket_path = allocate_socket_directory(port)
        self.socket_owned = True
        self.server_log_evidence = None

    def setup(self, rows=8192):
        self.timings = {}
        phase = time.monotonic()
        env = os.environ.copy()
        for key in ('MAKEFLAGS','MFLAGS','CFLAGS','CPPFLAGS','LDFLAGS','PGOPTIONS','SIFT_INITDB_TEMPLATE',
                    'SANITIZE','SIFT_HARDEN','SIFT_LTO','VULKAN','SIFT_TESTING','LD_PRELOAD'):
            env.pop(key, None)
        env['PATH'] = str(Path(self.pg_config).parent)+os.pathsep+env['PATH']
        flags = ['USE_PGXS=1','SIFT_TESTING=','SIFT_LTO=','SANITIZE=','VULKAN=',f'PG_CONFIG={self.pg_config}']
        command(['nice','-n','19','make','-j2',*flags],cwd=self.tree,env=env,log=self.log)
        command(['nice','-n','19','make','-j2',*flags,f'DESTDIR={self.stage}','install'],cwd=self.tree,env=env,log=self.log)
        self.timings['build_install'] = time.monotonic()-phase
        phase = time.monotonic()
        bindir = Path(command([self.pg_config,'--bindir']))
        sharedir = self.stage / command([self.pg_config,'--sharedir']).lstrip('/')
        pkglib = self.stage / command([self.pg_config,'--pkglibdir']).lstrip('/')
        self.bindir = bindir
        command([bindir/'initdb','--no-sync','--wal-segsize=1','-D',self.data,'-A','trust','-U','postgres','--no-locale','-E','UTF8'],env=env,log=self.log)
        # Independent logical load; no physical index is copied between revisions.
        config = f"""
port={self.port}
listen_addresses=''
unix_socket_directories='{self.socket}'
extension_control_path='{sharedir}:$system'
dynamic_library_path='{pkglib}:$libdir'
shared_buffers='64MB'
max_wal_size='48MB'
min_wal_size='32MB'
fsync=off
full_page_writes=off
autovacuum=off
max_parallel_workers_per_gather=0
max_parallel_maintenance_workers=0
jit=off
"""
        with (self.data/'postgresql.conf').open('a',encoding=sys.getfilesystemencoding(),errors='surrogateescape') as f:
            f.write(config)
        self.env = env
        self.timings['cluster_init_start'] = time.monotonic()-phase
        self.start(fixture_rows=rows)
        return config

    def start(self, *, fixture_rows=None):
        # The restart path launches the existing data directory without loading fixtures.
        phase=time.monotonic()
        if not self.socket_owned:
            # Exclusive recreation after the cost probe; never adopt an existing path.
            self.socket.mkdir(mode=0o700)
            self.socket_owned = True
            self.socket.chmod(0o700)
        self.started = True
        command(['nice','-n','19',self.bindir/'pg_ctl','-D',self.data,'-l',self.base/'server.log','-w','start'],env=self.env,log=self.log)
        if fixture_rows is not None:
            self.timings['cluster_init_start']+=time.monotonic()-phase
            phase=time.monotonic()
            command([self.bindir/'psql','-X','-v','ON_ERROR_STOP=1','-h',self.socket,'-p',self.port,'-U','postgres','-d','postgres',
                     '-v',f'rows={fixture_rows}','-f',ROOT/'bench/perf/fixture.sql'],env=self.env,log=self.log)
            self.timings['fixture_load']=time.monotonic()-phase
        lib = cell.libpq()
        lib.PQgetisnull.restype = ctypes.c_int
        lib.PQgetisnull.argtypes = [ctypes.c_void_p,ctypes.c_int,ctypes.c_int]
        self.conn = SQL(lib, f'host={self.socket} port={self.port} user=postgres dbname=postgres application_name=perf-gate')
        self.conn.exec("LOAD 'pg_sift'")
        self.conn.script(SETTINGS)
        require_equal(self.conn.rows('SHOW block_size'),[['8192']],'PostgreSQL page size')
        self.versions = self.conn.rows("SELECT version(),extversion FROM pg_extension WHERE extname='pg_sift'")

    def stop(self, output):
        try:
            if self.conn:
                self.conn.close()
                self.conn = None
            pid=self.data/'postmaster.pid'
            self.started = pid.exists()
            if self.started:
                command([self.bindir/'pg_ctl','-D',self.data,'-m','fast','-w','stop'],log=self.log,bounded=False)
                if pid.exists():
                    raise RuntimeError('postmaster.pid remains after stop; preserving cluster and socket directory')
                self.started = False
        finally:
            primary=sys.exc_info()[1]
            source,destination=self.base/'server.log',output/f'{self.name}-server.log'
            self.server_log_evidence=dict(source=str(source),artifact=str(destination),state='absent')
            try:
                # Failed startup can produce a log without ever leaving a PID.
                try:
                    source.stat()
                except FileNotFoundError:
                    pass
                else:
                    shutil.copyfile(source,destination)
                    self.server_log_evidence['state']='copied'
            except Exception as exc:
                self.server_log_evidence.update(state='copy_failed',error=f'{type(exc).__name__}: {exc}')
                if primary is None:
                    raise
                primary.add_note('server log preservation failed: '+str(exc))
            finally:
                if self.socket_owned and not self.started and not (self.data/'postmaster.pid').exists():
                    shutil.rmtree(self.socket)
                    self.socket_owned = False



def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def finalize_fixtures(revisions, deadline, sink):
    """Drain setup checkpoints on both clusters before any fixture/read probes."""
    start = time.monotonic()
    sink.update(started_at=utc_now(),revisions={})
    try:
        for rev in revisions:
            if time.monotonic()>=deadline:
                raise BudgetExpired('global runtime budget exhausted before fixture checkpoint')
            phase = time.monotonic()
            barrier = dict(started_at=utc_now(),complete=False)
            sink['revisions'][rev.name] = barrier
            try:
                rev.conn.exec('CHECKPOINT')
                if time.monotonic()>=deadline:
                    raise BudgetExpired('global runtime budget exhausted after fixture checkpoint')
                barrier['complete'] = True
            finally:
                barrier.update(finished_at=utc_now(),runtime_seconds=time.monotonic()-phase)
    finally:
        sink.update(finished_at=utc_now(),runtime_seconds=time.monotonic()-start)


def budget_context(total, workflow_start, reserve, *, now=None):
    if not math.isfinite(total) or total<=0 or not math.isfinite(reserve) or reserve<0:
        raise ValueError('budget must be positive and reserve nonnegative, both finite')
    if workflow_start is not None and (not math.isfinite(workflow_start) or workflow_start<=0):
        raise ValueError('workflow start must be a positive finite Unix timestamp')
    now = time.time() if now is None else now
    if workflow_start is not None and workflow_start>now:
        raise ValueError('workflow start is in the future')
    prior = now-workflow_start if workflow_start is not None else 0
    return dict(total_budget_seconds=total,workflow_start_seconds=workflow_start,
                prior_elapsed_seconds=prior,cleanup_report_reserve_seconds=reserve,
                measurement_budget_seconds=max(0,total-prior-reserve))


FORKS = ('main','fsm','vm','init')
MUTABLE_RELATIONS = {'mutable':'mutable_table_storage','mutable_pk':'mutable_btree_index'}


def relation_forks(conn):
    rows = conn.rows("SELECT c.relname," + ','.join(f"pg_relation_size(c.oid,'{fork}')" for fork in FORKS) +
                     " FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relkind IN ('r','i') ORDER BY 1")
    return {row[0]:dict(zip(FORKS,map(int,row[1:]))) for row in rows}


def fork_totals(forks):
    if any(value<0 for relation in forks.values() for value in relation.values()):
        raise ValueError('negative relation fork size')
    return {name:sum(values.values()) for name,values in forks.items()}


def sizes(conn):
    return list(fork_totals(relation_forks(conn)).items())


SIZE_RELATIONS = {'docs_bm25':'bm25_index','docs_col':'column_index','store':'table_storage',
                  'store_pk':'btree_index','docs':'heap','seed':'fixture_heap'}
SIZE_PAGE_TOLERANCE = 2


def size_verdict(baseline, candidate):
    if not all(isinstance(x,int) and x>=0 for x in (baseline,candidate)):
        raise ValueError('invalid size measurement')
    threshold = max(math.ceil(baseline*MATERIAL),baseline+SIZE_PAGE_TOLERANCE*8192)
    return dict(baseline_bytes=baseline,candidate_bytes=candidate,
                baseline_pages=math.ceil(baseline/8192),candidate_pages=math.ceil(candidate/8192),
                ratio=candidate/baseline if baseline else None,threshold_bytes=threshold,
                absolute_tolerance_pages=SIZE_PAGE_TOLERANCE,relative_threshold=MATERIAL,
                status='regression' if candidate>threshold else 'pass')


def size_snapshot(revisions,state):
    forks = [relation_forks(r.conn) for r in revisions]
    return dict(compare_sizes([fork_totals(f) for f in forks],state,SIZE_RELATIONS),forks=forks)


def compare_sizes(snapshots,state,relations):
    comparisons = []
    for name,role in relations.items():
        if any(name not in snap for snap in snapshots):
            raise ValueError('missing size relation '+name)
        comparisons.append(dict(relation=name,role=role,state=state,
                                gated=role in ('bm25_index','column_index','table_storage','mutable_table_storage','mutable_btree_index'),
                                **size_verdict(int(snapshots[0][name]),int(snapshots[1][name]))))
    for side,snapshot in enumerate(snapshots):
        if any(int(v)<0 for v in snapshot.values()):
            raise ValueError('negative relation size')
    return dict(state=state,relations=comparisons,
                footprint_bytes=[sum(int(v) for v in snap.values()) for snap in snapshots],
                status='regression' if any(c['gated'] and c['status']=='regression' for c in comparisons) else 'pass')


def churn_sizes(revisions):
    digests = []
    for rev in revisions:
        conn = rev.conn
        conn.exec("UPDATE docs SET body=body || ' churn' WHERE id%11=0")
        for table in ('docs','store'):
            conn.script(f"UPDATE {table} SET amount=amount+1 WHERE id%19=0; DELETE FROM {table} WHERE id%29=0")
            conn.exec(f'VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) {table}')
            conn.exec('SELECT pg_current_xact_id()')
            conn.exec(f'VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) {table}')
        digests.append([conn.rows(f"SELECT count(*),md5(string_agg(row(t.*)::text,'|' ORDER BY id)) FROM {table} t") for table in ('docs','store')])
    require_equal(digests[0],digests[1],'settled churn logical fixture')
    result = size_snapshot(revisions,'churn_vacuum')
    result['logical_digests'] = digests
    return result


def reset(conn, case):
    if case.reset:
        conn.script(case.reset)


def configure(conn, case):
    conn.script('SET sift.agg_preagg=on; SET sift.enable_agg=on')
    if case.settings:
        conn.script(case.settings)


def prepare(conn, case):
    configure(conn,case)
    reset(conn, case)


def table_geometry(conn, relation):
    fields = conn.rows(f"SELECT relation_blocks,free_blocks,reusable_blocks FROM sift.table_space('{relation}')")[0]
    segments,units = conn.rows(f"SELECT count(*),coalesce(sum(units),0) FROM sift.table_segments('{relation}') WHERE kind='columns'")[0]
    return dict(relation_blocks=int(fields[0]),free_blocks=int(fields[1]),
                reusable_blocks=int(fields[2]),column_segments=int(segments),units=int(units))


def plan_nodes(plan):
    if not plan:
        return []
    root = plan[0]['Plan'] if isinstance(plan,list) else plan
    return [root]+[node for child in root.get('Plans',[]) for node in plan_nodes(child)]


def topology(plan):
    return [{k:n[k] for k in ('Node Type','Custom Plan Provider','Index Name','Index','Relation Name','Parent Relationship') if k in n}
            for n in plan_nodes(plan)]


def require_plan(plan,case):
    nodes = plan_nodes(plan)
    if case.plan=='SiftAgg':
        valid = any(n.get('Node Type')=='Custom Scan' and n.get('Custom Plan Provider')=='SiftAgg' for n in nodes)
    else:
        valid = any((n.get('Node Type') in ('Index Scan','Index Only Scan','Bitmap Index Scan') and n.get('Index Name')==case.plan) or (case.plan=='docs_bm25' and n.get('Node Type')=='Custom Scan' and n.get('Custom Plan Provider') in ('SiftScan','SiftCount') and n.get('Index')==case.plan) for n in nodes)
    if not valid:
        raise ValueError(f'{case.name}: required structured plan {case.plan} absent: {topology(plan)}')


def validate_case(revisions, case):
    if case.lifecycle=='insert_trajectory':
        raise ValueError('INSERT validation requires frozen full-trajectory calibration')
    checks, plans, footprints = [], [], []
    for rev in revisions:
        conn = rev.conn
        prepare(conn, case)
        if case.lifecycle=='reset_each_operation':
            before_forks = relation_forks(conn)
            before = list(fork_totals(before_forks).items())
            before_geometry = table_geometry(conn,'mutable')
            if before_geometry['units']<3:
                raise ValueError('mutable fixture lacks actual table unit boundary')
            if case.sql.startswith('VACUUM'):
                conn.exec(case.sql)
                plan = None
                # Advance the xid horizon and check free-space reuse after maintenance.
                conn.exec('SELECT pg_current_xact_id()')
                conn.exec(case.sql)
                geometry = table_geometry(conn,'mutable')
                if geometry['reusable_blocks'] <= before_geometry['reusable_blocks'] and geometry['relation_blocks'] >= before_geometry['relation_blocks']:
                    raise ValueError('VACUUM neither reclaimed nor made space reusable')
            else:
                plan = json.loads(conn.rows('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING OFF) '+case.sql)[0][0])
            got = conn.rows(case.check)
            # Mutable expected state is a plain heap SQL query, independent of table AM.
            oracle = {'storage_update': 'SELECT count(*),sum(id),sum(amount)+count(amount) FILTER (WHERE id BETWEEN 100 AND 200) FROM seed',
                      'storage_vacuum_space': 'SELECT count(*),sum(id),sum(amount) FROM seed WHERE id<=2048'}[case.name]
            require_equal(got, conn.rows(oracle), case.name+' heap oracle')
            after_forks = relation_forks(conn)
            footprints.append(dict(before=before,after=list(fork_totals(after_forks).items()),
                                   before_geometry=before_geometry,after_geometry=table_geometry(conn,'mutable'),
                                   before_forks=before_forks,after_forks=after_forks))
            plans.append(plan)  # VACUUM has no EXPLAIN; validate actual space instead.
        else:
            plan = json.loads(conn.rows('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING OFF) '+case.sql)[0][0])
            require_plan(plan,case)
            plans.append(plan)
            got = conn.rows(case.sql)
            if case.oracle:
                conn.exec('SET sift.enable_agg=off')
                require_equal(got, conn.rows(case.oracle), case.name+' heap oracle')
                conn.exec('SET sift.enable_agg=on')
            if not got:
                raise ValueError(case.name+': unexpectedly empty result')
        checks.append(got)
    require_equal(checks[0], checks[1], case.name+' baseline/candidate')
    plan_changed = topology(plans[0])!=topology(plans[1])
    lifecycle_sizes = None
    if case.name=='storage_vacuum_space':
        lifecycle_sizes = compare_sizes([dict(p['after']) for p in footprints],
                                        'mutable_vacuum',MUTABLE_RELATIONS)
        lifecycle_sizes['forks'] = [p['after_forks'] for p in footprints]
    return dict(results=checks,plans=plans,plan_changed=plan_changed,
                topology=[topology(p) for p in plans],sizes=footprints,size_checks=lifecycle_sizes)


def elapsed(conn, case, operations, deadline, delay=0, *, statements=None, raw=None):
    if case.lifecycle=='insert_trajectory' and (statements is None or len(statements)!=operations):
        raise ValueError('INSERT requires precomputed point statements')
    total = 0.0
    for ordinal in range(operations):
        if time.monotonic() >= deadline:
            raise BudgetExpired('measurement budget exhausted')
        if case.lifecycle=='reset_each_operation':
            reset(conn, case)
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after reset')
        start = time.perf_counter()
        conn.exec(statements[ordinal] if statements is not None else case.sql)
        value = time.perf_counter()-start
        if not math.isfinite(value) or value <= 0:
            raise ValueError('invalid monotonic SQL elapsed time')
        total += value
        if raw is not None:
            raw.append(value)
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after SQL')
    if delay:
        # Explicit acceptance mode: one SQL-visible delay per measured batch.
        start = time.perf_counter()
        conn.exec(f'SELECT pg_sleep({delay:.9f})')
        total += time.perf_counter()-start
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after SQL control')
    return total


def calibrate(revisions, case, deadline):
    sql_costs, wall_costs = [], []
    for rev in revisions:
        prepare(rev.conn,case)
        elapsed(rev.conn,case,1,deadline)
        start = time.monotonic()
        sql_costs.append(elapsed(rev.conn,case,3,deadline)/3)
        wall_costs.append((time.monotonic()-start)/3)
    desired = max(1,math.ceil(TARGET/min(sql_costs)))
    cap = max(1,math.floor(BATCH_WALL_LIMIT/max(wall_costs)))
    operations = min(desired,cap)
    return dict(operations=operations,desired_operations=desired,
                sql_seconds_per_operation=sql_costs,wall_seconds_per_operation=wall_costs,
                target_limited_by_reset=operations<desired)


SEED_IDENTITY = "SELECT count(*),max(id),md5(string_agg(row(seed.*)::text,'|' ORDER BY id)) FROM seed"


def insert_identity(revisions):
    seeds = [r.conn.rows(SEED_IDENTITY) for r in revisions]
    require_equal(seeds[0],seeds[1],'INSERT seed identity')
    count,maximum,digest = seeds[0][0]
    count,maximum = int(count),int(maximum)
    if count<=0 or not -(2**31)<=maximum<=2**31-1-max(INSERT_PILOT,INSERT_GROWTH_CAP):
        raise ValueError('INSERT seed/full growth exceeds integer domain')
    return dict(seed_count=count,seed_max_id=maximum,first_id=maximum+1,seed_digest=digest)


def insert_footprint(conn):
    geometry = table_geometry(conn,'mutable')
    if geometry['units']<3 or geometry['column_segments']<1 or geometry['relation_blocks']<256:
        raise ValueError('mutable fixture lacks actual table unit/segment/page boundary')
    forks = relation_forks(conn)
    return dict(geometry=geometry,forks={name:forks[name] for name in MUTABLE_RELATIONS},
                totals={name:fork_totals(forks)[name] for name in MUTABLE_RELATIONS})


def cleanup_insert_sql(conn, statements, sink, primary):
    """Restore temporary INSERT SQL state without demoting a known primary error."""
    if primary is not None:
        sink['primary_error'] = dict(type=type(primary).__name__,error=str(primary))
    for statement in statements:
        try:
            conn.exec(statement)
        except Exception as cleanup:
            detail = dict(statement=statement,type=type(cleanup).__name__,error=str(cleanup))
            sink.setdefault('cleanup_errors',[]).append(detail)
            if primary is None or (isinstance(primary,Inconclusive) and not isinstance(cleanup,Inconclusive)):
                raise
            primary.add_note(f"INSERT SQL cleanup failed ({statement}): {type(cleanup).__name__}: {cleanup}")
            return False
    return True


def check_insert(conn, identity, operations, sink):
    require_equal(conn.rows(SEED_IDENTITY),[[str(identity['seed_count']),str(identity['seed_max_id']),identity['seed_digest']]],
                  'INSERT seed unchanged')
    case = next(c for c in CASES if c.lifecycle=='insert_trajectory')
    expected = case.expected_rows(identity['seed_max_id'],operations)
    # Both full row multisets, including NULLs and pad, come from explicit columns.
    query = f"""WITH expected AS ({expected}), actual AS (SELECT id,g,amount,body,pad FROM mutable),
    delta AS ((SELECT * FROM expected EXCEPT ALL SELECT * FROM actual)
              UNION ALL (SELECT * FROM actual EXCEPT ALL SELECT * FROM expected))
    SELECT (SELECT count(*) FROM expected),
           (SELECT md5(string_agg(row(e.*)::text,'|' ORDER BY id,g,amount,body,pad)) FROM expected e),
           (SELECT count(*) FROM actual),
           (SELECT md5(string_agg(row(a.*)::text,'|' ORDER BY id,g,amount,body,pad)) FROM actual a),
           (SELECT count(*) FROM delta)"""
    conn.exec('SET sift.enable_agg=off')
    try:
        count,digest,actual_count,actual_digest,difference = conn.rows(query)[0]
        sink.update(expected_count=int(count),expected_digest=digest,actual_count=int(actual_count),
                    actual_digest=actual_digest,multiset_difference=int(difference))
        require_equal(int(count),identity['seed_count']+operations,'INSERT expected count')
        require_equal((actual_count,actual_digest,difference),(count,digest,'0'),'INSERT full multiset')
        return sink
    finally:
        cleanup_insert_sql(conn,['SET sift.enable_agg=on'],sink,sys.exc_info()[1])


def insert_control_count(conn):
    return int(conn.rows("SELECT current_setting('perf_gate.delayed_rows')")[0][0])


def install_insert_control(conn, seed_max, iterations, *, active=True, sink=None):
    # Infrastructure DDL only is transactional. Every workload point remains autocommit.
    try:
        conn.exec('BEGIN')
        conn.exec("""CREATE FUNCTION perf_gate_cpu_work(row_id integer, iterations integer) RETURNS text
        LANGUAGE plpgsql VOLATILE AS $$
        DECLARE state text := row_id::text; i integer;
        BEGIN
          FOR i IN 1..iterations LOOP state := md5(state || ':' || row_id::text); END LOOP;
          RETURN state;
        END $$""")
        conn.exec(f"""CREATE FUNCTION perf_gate_insert_delay() RETURNS trigger LANGUAGE plpgsql AS $$
        BEGIN
          IF NEW.id > {seed_max} THEN
            PERFORM set_config('perf_gate.delayed_rows',(current_setting('perf_gate.delayed_rows')::bigint+1)::text,false);
            PERFORM set_config('perf_gate.cpu_checksum',perf_gate_cpu_work(NEW.id,{iterations}),false);
          END IF;
          RETURN NEW;
        END $$""")
        conn.exec('CREATE TRIGGER perf_gate_insert_delay BEFORE INSERT ON mutable FOR EACH ROW EXECUTE FUNCTION perf_gate_insert_delay()')
        conn.exec('ALTER TABLE mutable DISABLE TRIGGER perf_gate_insert_delay')
        conn.exec("SET perf_gate.delayed_rows='0'")
        if sink is not None: sink['commit_attempted']=True
        conn.exec('COMMIT')
        if sink is not None: sink['infrastructure_committed']=True
    except Exception:
        cleanup_insert_sql(conn,['ROLLBACK'],sink if sink is not None else {},sys.exc_info()[1])
        raise
    if active:
        conn.exec('ALTER TABLE mutable ENABLE TRIGGER perf_gate_insert_delay')
        if sink is not None: sink['active']=True


def remove_insert_control(conn, sink, primary):
    return cleanup_insert_sql(conn,['DROP TRIGGER perf_gate_insert_delay ON mutable',
                                    'DROP FUNCTION perf_gate_insert_delay()',
                                    'DROP FUNCTION perf_gate_cpu_work(integer,integer)'],sink,primary)


def require_control_absent(conn):
    require_equal(conn.rows("SELECT count(*) FROM pg_proc WHERE proname IN ('perf_gate_cpu_work','perf_gate_insert_delay')"),
                  [['0']],'control functions absent')
    require_equal(conn.rows("SELECT count(*) FROM pg_trigger WHERE tgname='perf_gate_insert_delay'"),[['0']],'control trigger absent')


def calibrate_cpu_work(conn, sink):
    """One fixed zero/16-iteration warm+measurement, no INSERT or verdict tuning."""
    sink.update(rows=CPU_PILOT_ROWS,pilot_iterations=CPU_PILOT_ITERATIONS,iteration_cap=CPU_ITERATION_CAP,
                blocks=[],complete=False,scope='client elapsed helper calls; no mutable writes; trigger/counter/seed overhead excluded')
    for iterations in (0,CPU_PILOT_ITERATIONS):
        block = dict(iterations=iterations,warm_seconds=[],operation_seconds=[],checksums=[])
        sink['blocks'].append(block)
        for key in ('warm_seconds','operation_seconds'):
            for ordinal in range(CPU_PILOT_ROWS):
                before=time.perf_counter()
                checksum=conn.rows(f'SELECT perf_gate_cpu_work({-ordinal-1},{iterations})')[0][0]
                block[key].append(time.perf_counter()-before)
                require_equal(checksum,cpu_checksum(-ordinal-1,iterations),'CPU calibration checksum')
                if key=='operation_seconds': block['checksums'].append(checksum)
    zero,loaded=(sum(b['operation_seconds'])/CPU_PILOT_ROWS for b in sink['blocks'])
    slope=(loaded-zero)/CPU_PILOT_ITERATIONS
    if not math.isfinite(slope) or slope<=0:
        raise Inconclusive('fixed CPU calibration has no positive finite incremental work evidence')
    sink.update(seconds_per_iteration=slope,complete=True)


def cpu_checksum(row_id, iterations):
    state=str(row_id)
    for _ in range(iterations): state=hashlib.md5((state+':'+str(row_id)).encode()).hexdigest()
    return state


def freeze_cpu_work(calibration, cpu):
    if not cpu.get('complete') or not finite_number(cpu.get('seconds_per_iteration')) or cpu['seconds_per_iteration']<=0:
        raise Inconclusive('missing completed same-invocation CPU calibration')
    # A large prospective functional signal: added helper CPU equal to slower clean per-row SQL.
    desired=math.ceil(max(calibration['sql_seconds_per_operation'])/cpu['seconds_per_iteration'])
    iterations=min(desired,CPU_ITERATION_CAP)
    return dict(iterations=iterations,desired_iterations=desired,limited_by_cap=iterations<desired,
                seconds_per_iteration=cpu['seconds_per_iteration'],predicted_added_seconds_per_row=iterations*cpu['seconds_per_iteration'],
                target_added_seconds_per_row=max(calibration['sql_seconds_per_operation']),
                policy='one fixed zero/16 slope; ceil(slower clean client SQL per row / slope), cap1024; never verdict tuned',
                provenance=cpu)


INSERT_PHASES = ('warm_reset','warm','final_reset','preprobes','checkpoint','stream','postcheck')
POST_CONTROL_PHASES = ('fault_removal','final_report','cluster_teardown')
CONTROL_COST_PHASES = ('fault_setup',)+POST_CONTROL_PHASES


@contextlib.contextmanager
def insert_phase(side, name, deadline):
    start = time.monotonic()
    phase = dict(started_at=utc_now(),start_offset_seconds=start-PROCESS_ORIGIN,complete=False)
    side['phases'][name] = phase
    try:
        if start>=deadline:
            raise BudgetExpired('global budget exhausted before INSERT '+name)
        yield
        if time.monotonic()>=deadline:
            raise BudgetExpired('global budget exhausted after INSERT '+name)
        phase['complete'] = True
    except Exception as exc:
        phase['error'] = dict(type=type(exc).__name__,error=str(exc))
        raise
    finally:
        end = time.monotonic()
        phase.update(finished_at=utc_now(),end_offset_seconds=end-PROCESS_ORIGIN,seconds=end-start)


def insert_pair(revisions, case, identity, operations, deadline, sink, *, order=(0,1), role='verdict', control=False):
    """One finite paired restoration -> barrier -> streams -> deferred checks owner."""
    origin = time.monotonic()
    sides = [dict(phases={},operation_seconds=[],warm_operation_seconds=[]),
             dict(phases={},operation_seconds=[],warm_operation_seconds=[])]
    sink.update(operations=operations,order=list(order),role=role,clean=not control,
                workload_version=case.workload_version,sides=sides,complete=False,started_at=utc_now(),
                before_observation='pre_finalization',after_observation='pair_end',
                start_offset_seconds=origin-PROCESS_ORIGIN,phase='warm')
    try:
        statements = case.statements(identity['seed_max_id'],operations)
        for index in order:
            conn,side = revisions[index].conn,sides[index]
            if control and index==1:
                side['control_counts'] = []
            with insert_phase(side,'warm_reset',deadline):
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
                prepare(conn,case)
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
            with insert_phase(side,'warm',deadline):
                side['warm_sql_seconds'] = elapsed(conn,case,operations,deadline,statements=statements,raw=side['warm_operation_seconds'])
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
        sink['phase'] = 'final_restore_and_preprobes'
        for index in order:
            conn,side = revisions[index].conn,sides[index]
            with insert_phase(side,'final_reset',deadline):
                reset(conn,case)
            with insert_phase(side,'preprobes',deadline):
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
                side['before'] = insert_footprint(conn)
        sink['phase'] = 'checkpoint'
        for index in order:
            with insert_phase(sides[index],'checkpoint',deadline):
                revisions[index].conn.exec('CHECKPOINT')
        sink.update(paired_ready_at=utc_now(),paired_ready_offset_seconds=time.monotonic()-PROCESS_ORIGIN)
        sink['phase'] = 'streams'
        for index in order:
            side = sides[index]
            with insert_phase(side,'stream',deadline):
                # No SQL other than the two point streams until BOTH streams return.
                start = time.monotonic()
                side['sql_seconds'] = elapsed(revisions[index].conn,case,operations,deadline,
                                              statements=statements,raw=side['operation_seconds'])
                side['loop_wall_seconds'] = time.monotonic()-start
        sink['phase'] = 'deferred_checks'
        for index in order:
            conn,side = revisions[index].conn,sides[index]
            with insert_phase(side,'postcheck',deadline):
                if control and index==1:
                    counts = side['control_counts']
                    counts.append(insert_control_count(conn))
                    first = counts[0]
                    require_equal(counts,[first,first,first+operations,first+operations,first+2*operations],
                                  'real per-new-row CPU counts; seed reset must not perform work')
                    checksum=conn.rows("SELECT current_setting('perf_gate.cpu_checksum')")[0][0]
                    side['control_checksum']=checksum
                    require_equal(checksum,cpu_checksum(identity['seed_max_id']+operations,control['iterations']),'last new-row CPU checksum')
                side['oracle'] = {}
                check_insert(conn,identity,operations,side['oracle'])
                side['after'] = insert_footprint(conn)
        for side in sides:
            phases = side['phases']
            stream_start = phases['stream']['start_offset_seconds']
            side.update(warm_age_seconds=stream_start-phases['warm']['end_offset_seconds'],
                        final_reset_age_seconds=stream_start-phases['final_reset']['end_offset_seconds'],
                        checkpoint_age_seconds=stream_start-phases['checkpoint']['end_offset_seconds'],
                        paired_ready_age_seconds=stream_start-sink['paired_ready_offset_seconds'],
                        deferred_check_age_seconds=phases['postcheck']['start_offset_seconds']-phases['stream']['end_offset_seconds'])
        sink.update(samples=[side['sql_seconds'] for side in sides],complete=True,phase='complete')
        return sink['samples']
    finally:
        phase_sum = sum(p['seconds'] for side in sides for p in side['phases'].values())
        finished_at = utc_now()
        end = time.monotonic()
        wall = end-origin
        sink.update(finished_at=finished_at,end_offset_seconds=end-PROCESS_ORIGIN,
                    wall_seconds=wall,phase_seconds=phase_sum,residual_seconds=wall-phase_sum)


def insert_observation(revisions, case, identity, operations, histories, deadline, sink, *, order=(0,1), role='verdict', control=False):
    """K dependent restored histories form ONE pair; publish only after every check."""
    if not 1<=histories<=INSERT_HISTORY_CAP:
        raise ValueError('invalid frozen INSERT history count')
    start=time.monotonic()
    schedule=[tuple(order if j%2==0 else reversed(order)) for j in range(histories)]
    sink.update(operations=operations,history_count=histories,internal_orders=schedule,histories=[],role=role,
                clean=not control,workload_version=case.workload_version,complete=False)
    try:
        totals=[0.,0.]
        for order in schedule:
            history={}
            sink['histories'].append(history)
            values=insert_pair(revisions,case,identity,operations,deadline,history,order=order,role=role,control=control)
            for side in (0,1): totals[side]+=values[side]
        sink.update(samples=totals,complete=True)
        return totals
    finally:
        sink['history_seconds']=sum(h['wall_seconds'] for h in sink['histories'] if 'wall_seconds' in h)
        sink['wall_seconds']=time.monotonic()-start
        sink['residual_seconds']=sink['wall_seconds']-sink['history_seconds']


def calibrate_insert(revisions, case, identity, deadline, sink):
    sink.update(pilot_operations=INSERT_PILOT,growth_cap_operations=INSERT_GROWTH_CAP,
                target_seconds=TARGET,predicted_wall_limit_seconds=BATCH_WALL_LIMIT,pilot_pair={},
                wall_cap_scope='FINAL reset + measured loop only; warm/probes/both checkpoints/waits/checks excluded')
    insert_pair(revisions,case,identity,INSERT_PILOT,deadline,sink['pilot_pair'],role='pilot')
    sides = sink['pilot_pair']['sides']
    costs = [side['sql_seconds']/INSERT_PILOT for side in sides]
    walls = [side['loop_wall_seconds']/INSERT_PILOT for side in sides]
    resets = [side['phases']['final_reset']['seconds'] for side in sides]
    if any(not math.isfinite(v) or v<=0 for v in costs+walls) or any(not math.isfinite(r) or r<0 for r in resets):
        raise ValueError('invalid INSERT pilot elapsed')
    desired = math.ceil(TARGET/min(costs))
    wall_cap = math.floor(min((BATCH_WALL_LIMIT-r)/w for r,w in zip(resets,walls)))
    sink.update(desired_operations=desired,wall_cap_operations=wall_cap,
                sql_seconds_per_operation=costs,loop_wall_seconds_per_operation=walls)
    if wall_cap<1:
        raise Inconclusive('no admissible INSERT trajectory within predicted reset+loop wall cap')
    operations = min(desired,wall_cap,INSERT_GROWTH_CAP)
    sink.update(operations=operations,limited_by_wall=operations<desired and wall_cap==operations,
                limited_by_growth=operations<desired and INSERT_GROWTH_CAP==operations,
                predicted_sql_seconds=[operations*c for c in costs],
                predicted_reset_loop_seconds=[r+operations*w for r,w in zip(resets,walls)])
    desired_histories=math.ceil(TARGET/min(sink['predicted_sql_seconds']))
    histories=min(desired_histories,INSERT_HISTORY_CAP)
    sink.update(histories=histories,desired_histories=desired_histories,history_cap=INSERT_HISTORY_CAP,
                internal_order_policy='alternate histories; complement entire schedule on BA observations',
                initial_and_confirmation_orders=[[[int((j+i)%2),int(1-(j+i)%2)] for j in range(histories)] for i in range(6)],
                validation_orders=[[j%2,1-j%2] for j in range(histories)],
                predicted_observation_sql_seconds=[histories*v for v in sink['predicted_sql_seconds']],
                predicted_observation_wall_seconds=histories*sink['pilot_pair']['wall_seconds'],
                predicted_max_clean_remaining_wall_seconds=13*histories*sink['pilot_pair']['wall_seconds'],
                clean_remaining_model_scope='13 composites of pilot history wall; N may differ; estimate not bound; pilot already spent',
                target_attained_prediction=histories>=desired_histories,
                limitations='client/encoding/deadline/libpq/autocommit per-row overhead retained; composition does not remove side bias or correlated noise')
    return sink


def validate_insert(revisions, case, identity, operations, deadline, sink, *, control=False, histories=1):
    sink.update(pair={},size_checks=None,history_size_checks=[],complete=False)
    try:
        insert_observation(revisions,case,identity,operations,histories,deadline,sink['pair'],role='validation',control=control)
    finally:
        # Size decisions belong to completed histories, independently of observation timing.
        for history in sink['pair'].get('histories',[]):
            if not history.get('complete'):
                continue
            sides=history['sides']
            sink['history_size_checks'].append(dict(compare_sizes([side['after']['totals'] for side in sides],
                                                    'mutable_insert_trajectory',MUTABLE_RELATIONS),
                                                   forks=[side['after']['forks'] for side in sides]))
        checks=sink['history_size_checks']
        if checks:
            complete=sink['pair'].get('complete',False) and len(checks)==histories
            # First history represents the state; ANY completed history regression is decisive.
            sink['size_checks']=dict(checks[0],representative_history=0,history_checks=checks,
                                    complete=complete,completed_histories=len(checks),expected_histories=histories,
                                    status='regression' if any(s['status']=='regression' for s in checks) else
                                           'pass' if complete else 'inconclusive')
    sink['complete']=True
    return sink


def finite_number(value):
    return type(value) in (int,float) and math.isfinite(value)


def clean_full_n_pair_cost(pair, operations, role):
    """Only complete, finite full-N validation/verdict evidence can fund controls."""
    if (pair.get('workload_version')!=next(c.workload_version for c in CASES if c.lifecycle=='insert_trajectory')
            or not pair.get('complete') or not pair.get('clean') or pair.get('operations')!=operations
            or pair.get('role')!=role or len(pair.get('sides',[]))!=2):
        raise Inconclusive('missing completed clean full-N '+role+' pair cost evidence')
    for side in pair['sides']:
        oracle=side.get('oracle',{})
        if (oracle.get('multiset_difference')!=0 or oracle.get('actual_count')!=oracle.get('expected_count')
                or not isinstance(oracle.get('expected_count'),int) or oracle['expected_count']<=0
                or not oracle.get('expected_digest') or oracle.get('actual_digest')!=oracle['expected_digest']
                or not all(key in side.get(state,{}) for state in ('before','after') for key in ('geometry','forks','totals'))):
            raise Inconclusive('missing completed full-N oracle/geometry/raw-fork evidence')
        if any(len(side.get(key,[]))!=operations or any(not finite_number(v) or v<=0 for v in side[key])
               for key in ('operation_seconds','warm_operation_seconds')):
            raise Inconclusive('missing complete full-N raw operation evidence')
        for metric,raw in (('sql_seconds','operation_seconds'),('warm_sql_seconds','warm_operation_seconds')):
            if not finite_number(side.get(metric)) or side[metric]<=0 or not math.isclose(side[metric],sum(side[raw]),rel_tol=1e-12):
                raise Inconclusive('full-N SQL sum differs from complete raw point evidence')
        if set(side.get('phases',{}))!=set(INSERT_PHASES):
            raise Inconclusive('missing full-N phase cost evidence')
        for phase in side['phases'].values():
            if not phase.get('complete') or any(not finite_number(phase.get(key,math.nan)) for key in
                                               ('seconds','start_offset_seconds','end_offset_seconds')) or phase['seconds']<0 or not math.isclose(phase['end_offset_seconds']-phase['start_offset_seconds'],phase['seconds'],abs_tol=1e-9):
                raise Inconclusive('incomplete/nonfinite full-N phase cost evidence')
    wall,phases,residual = (pair.get(key,math.nan) for key in ('wall_seconds','phase_seconds','residual_seconds'))
    actual = sum(p['seconds'] for side in pair['sides'] for p in side['phases'].values())
    if (not all(finite_number(v) for v in (wall,phases,residual)) or wall<=0 or residual<0
            or not math.isclose(actual,phases,abs_tol=1e-9) or not math.isclose(wall,phases+residual,abs_tol=1e-9)):
        raise Inconclusive('invalid complete full-N wall/phase/residual evidence')
    return wall


def clean_observation_cost(observation, operations, histories, role):
    if (not observation.get('complete') or not observation.get('clean') or observation.get('operations')!=operations
            or observation.get('history_count')!=histories or observation.get('role')!=role
            or observation.get('workload_version')!=next(c.workload_version for c in CASES if c.lifecycle=='insert_trajectory')
            or len(observation.get('histories',[]))!=histories):
        raise Inconclusive('missing complete frozen N/K observation cost evidence')
    history_seconds=sum(clean_full_n_pair_cost(h,operations,role) for h in observation['histories'])
    wall=observation.get('wall_seconds',math.nan)
    residual=observation.get('residual_seconds',math.nan)
    if not finite_number(wall) or not finite_number(residual) or residual<0 or not math.isclose(wall,history_seconds+residual,abs_tol=1e-9):
        raise Inconclusive('invalid composite wall/history/residual evidence')
    return wall


@contextlib.contextmanager
def run_phase(report, name):
    """Invocation-owned measured phases; nested spans explain, never double-charge."""
    entries=report.setdefault('phase_ledger',[])
    parent=next((i for i in range(len(entries)-1,-1,-1) if 'end_offset_seconds' not in entries[i]),None)
    start=time.monotonic()
    entry=dict(name=name,parent=parent,start_offset_seconds=start-PROCESS_ORIGIN,started_at=utc_now(),complete=False)
    entries.append(entry)
    try:
        yield entry
        entry['complete']='error' not in entry
    except Exception as exc:
        entry['error']=f'{type(exc).__name__}: {exc}'
        raise
    finally:
        end=time.monotonic()
        entry.update(end_offset_seconds=end-PROCESS_ORIGIN,seconds=end-start,finished_at=utc_now())


def reconcile_ledger(report, start, end):
    entries=report.get('phase_ledger',[])
    exclusive=sum(e['seconds'] for e in entries if e['parent'] is None)
    total=end-start
    residual=total-exclusive
    if not finite_number(residual) or residual<0:
        raise ValueError('phase ledger overlaps or exceeds driver wall')
    return dict(driver_wall_seconds=total,exclusive_phase_seconds=exclusive,residual_seconds=residual,
                partition='only parent=null inclusive spans + residual; child phases/pair histories are explanatory')


def measured_estimate(entry, report, scope):
    if not entry.get('complete') or not finite_number(entry.get('seconds',math.nan)) or entry['seconds']<=0:
        raise Inconclusive('missing completed finite cost producer: '+scope)
    return dict(complete=True,seconds=entry['seconds'],kind='same_invocation_measured_estimate',
                provenance=dict(phase=entry['name'],baseline=report['baseline_ref'],candidate=report['candidate_ref'],
                                tier=report['config']['tier'],settings=SETTINGS,scope=scope,
                                artifact_bytes=entry.get('artifact_bytes')))


def restart_identity(rev):
    settings=sorted(re.findall(r'SET ([a-z_.]+)=',SETTINGS))
    return dict(seed=rev.conn.rows(SEED_IDENTITY),versions=rev.versions,
                system=rev.conn.rows('SELECT system_identifier FROM pg_control_system()'),
                settings=rev.conn.rows("SELECT name,setting FROM pg_settings WHERE name IN ("+
                                      ','.join("'"+name+"'" for name in settings)+") ORDER BY name"))


def completed_control_schedule(report):
    required={c.name for c in CASES if c.lifecycle!='insert_trajectory'}
    catalog=report.get('acceptance_catalog_validation',{})
    read_controls={c['name']:c for c in report['cases'] if c['name'] in ('control_unchanged','control_sql_slowdown')}
    phases=[dict(entry) for entry in report.get('phase_ledger',[]) if entry['name'] in
            ('churn_normal_sizes','bm25_growth_logical_forks')]
    states={s['state'] for s in normal_size_checks(report)}
    catalog_complete=required<=catalog.keys() and all(catalog[name].get('complete') for name in required)
    reads_complete=set(read_controls)=={'control_unchanged','control_sql_slowdown'} and all(c.get('complete') for c in read_controls.values())
    size_complete='size_control' in report and report['size_control']['state']=='control_build_segments_64'
    coverage=dict(catalog_complete=catalog_complete,read_controls_complete=reads_complete,size_control_complete=size_complete,
                  normal_size_states=sorted(states),phases=phases,sources=[report['baseline_ref'],report['candidate_ref'],report['config']['tier']],
                  scope='completed actuals already spent; never future demand')
    coverage['complete']=valid_completed_control_schedule(coverage,tuple(coverage['sources']))
    return coverage


def valid_completed_control_schedule(coverage, sources):
    phases=coverage.get('phases',[])
    return (coverage.get('sources')==list(sources) and coverage.get('catalog_complete') and coverage.get('read_controls_complete')
            and coverage.get('size_control_complete') and {'settled_build','churn_vacuum','mutable_vacuum'}<=set(coverage.get('normal_size_states',[]))
            and {p['name'] for p in phases}=={'churn_normal_sizes','bm25_growth_logical_forks'}
            and len(phases)==2 and all(p.get('complete') and finite_number(p.get('seconds',math.nan)) and p['seconds']>=0 for p in phases))


def prepare_control_costs(revisions, report, output):
    """One inactive infrastructure cycle and one existing-cluster stop/restart."""
    report['completed_control_work']=completed_control_schedule(report)
    if not report['completed_control_work']['complete']:
        raise Inconclusive('read/catalog/normal-size/BM25 prerequisites incomplete before infrastructure probe')
    identity=insert_identity(revisions)
    report['control_setup_intent']=dict(phase='inactive_infrastructure_probe',complete=False,admitted=False,
        active_fault=False,mutable_writes_permitted=False,seed_max_id=identity['seed_max_id'],
        schedule='all reads/catalog -> normal churn/forks -> BM25 growth proof -> inactive costs/restart -> clean composite INSERT -> admitted CPU control')
    write_report(output,report)  # closed intent before first inactive DDL
    conn=revisions[1].conn
    require_control_absent(conn)
    before=conn.rows(SEED_IDENTITY)
    with run_phase(report,'inactive_fault_setup') as install:
        install_insert_control(conn,identity['seed_max_id'],CPU_PILOT_ITERATIONS,active=False,sink=report['control_setup_intent'])
    primary=None
    try:
        require_equal(conn.rows("SELECT tgenabled FROM pg_trigger WHERE tgname='perf_gate_insert_delay'"),[['D']],'probe trigger disabled')
        report['cpu_calibration']={}
        with run_phase(report,'cpu_helper_calibration'):
            calibrate_cpu_work(conn,report['cpu_calibration'])
        require_equal(insert_control_count(conn),0,'inactive probe has no faulted rows')
        require_equal(before,conn.rows(SEED_IDENTITY),'inactive probe seed identity')
    except Exception as exc:
        primary=exc
        raise
    finally:
        with run_phase(report,'inactive_fault_removal') as removal:
            removed=remove_insert_control(conn,report['control_setup_intent'],primary)
            if not removed: removal['error']='inactive SQL cleanup incomplete; see control_setup_intent.cleanup_errors'
    if not removed: raise Inconclusive('inactive infrastructure removal incomplete')
    require_control_absent(conn)
    identities=[restart_identity(r) for r in revisions]
    with run_phase(report,'cluster_stop_probe') as stop:
        for rev in reversed(revisions): rev.stop(output)
    with run_phase(report,'cluster_restart'):
        for rev in revisions: rev.start()
        for rev,expected in zip(revisions,identities):
            require_equal(restart_identity(rev),expected,'same cluster/binary/settings after restart')
            require_control_absent(rev.conn)
    report['post_control_costs']=dict(
        fault_setup=measured_estimate(install,report,'inactive install/disable/init; future enable and varying DDL wall unknown'),
        fault_removal=measured_estimate(removal,report,'same DROP trigger/functions; future duration varies'),
        cluster_teardown=measured_estimate(stop,report,'same two clusters/log copies; final dirty state and rmtree differ'))
    report['control_setup_intent']['complete']=True
    write_report(output,report)
    refresh_report_cost(report)


def refresh_report_cost(report):
    closed=[e for e in report['phase_ledger'] if e['name']=='report_write' and e.get('complete')]
    if not closed: raise Inconclusive('no closed report writer cost evidence')
    report['post_control_costs']['final_report']=measured_estimate(max(closed,key=lambda e:e['seconds']),report,
        'largest closed real report at current volume; final growth, terminal sidecar and upload unknown')


def insert_control_budget(unchanged, deadline, cost_evidence, *, cpu_work, sources, completed_work, reserve_seconds=0):
    operations,histories=(unchanged['calibration'][key] for key in ('operations','histories'))
    population=[]
    try:
        if not unchanged.get('complete'):
            raise Inconclusive('clean INSERT case incomplete before control')
        for name in ('initial','confirmation'):
            if name=='confirmation' and name not in unchanged: continue
            block=unchanged.get(name,{})
            if block.get('n')!=6 or sum(bool(pair.get('complete')) for pair in block.get('pairs',[]))!=6:
                raise Inconclusive('clean INSERT '+name+' block lacks six completed observations')
        population.append(dict(source='validation',wall_seconds=clean_observation_cost(
            unchanged.get('validation',{}).get('pair',{}),operations,histories,'validation')))
        for block_name in ('initial','confirmation'):
            for index,pair in enumerate(unchanged.get(block_name,{}).get('pairs',[])):
                if pair.get('complete'):
                    population.append(dict(source=f'{block_name}.pairs[{index}]',wall_seconds=clean_observation_cost(pair,operations,histories,'verdict')))
    except Inconclusive as exc:
        clean_error=str(exc)
    else:
        clean_error=None
    missing=[]
    estimates={}
    for name in CONTROL_COST_PHASES:
        entry=cost_evidence.get(name)
        if (not entry or not entry.get('complete') or not entry.get('provenance')
                or not finite_number(entry.get('seconds',math.nan)) or entry['seconds']<=0
                or not isinstance(entry['provenance'],dict)
                or tuple(entry['provenance'].get(k) for k in ('baseline','candidate','tier'))!=sources
                or entry['provenance'].get('settings')!=SETTINGS):
            missing.append(name)
        else:
            estimates[name]=entry
    if not valid_completed_control_schedule(completed_work,sources): missing.append('completed_read_catalog_churn_BM25_work')
    visits=INITIAL_PAIRS+MUTATION_CONFIRMATION_PAIRS+1
    rows=2*operations*histories*visits
    maximum=max(p['wall_seconds'] for p in population) if population and clean_error is None else None
    cpu=cpu_work['predicted_added_seconds_per_row']
    if not finite_number(cpu) or cpu<=0: raise Inconclusive('invalid frozen CPU work cost evidence')
    added=rows*cpu
    sampling=visits*maximum+added+estimates['fault_setup']['seconds'] if maximum is not None and not missing else None
    tail=sum(estimates[k]['seconds'] for k in POST_CONTROL_PHASES) if not missing else None
    return dict(controlled_pairs=visits,histories=histories,expected_checkpoint_calls=2*histories*visits,
                candidate_delayed_operations=rows,candidate_cpu_operations=rows,cpu_work=cpu_work,
                completed_work=completed_work,sources=list(sources),
                clean_full_N_population=population,clean_evidence_error=clean_error,max_clean_full_N_pair_wall=maximum,
                predicted_clean_pair_seconds=visits*maximum if maximum is not None else None,
                predicted_added_cpu_seconds=added,post_control_cost_evidence=estimates,
                missing_post_control_costs=missing,post_control_seconds=tail,
                predicted_seconds=sampling,predicted_total_remaining_seconds=sampling+tail if sampling is not None else None,
                evidence_complete=sampling is not None,remaining_seconds=deadline-time.monotonic(),reserve_seconds=reserve_seconds,
                cost_scope='13 * completed clean N/K observation max; pilot excluded; all history phases/residual once; completed tail already spent',
                limitations=['same-run measured estimates, not bounds','trigger seed predicate/counter/enable overhead not isolated',
                             'CPU helper slope includes client calls; per-row trigger cost may differ',
                             'future dirty-stop/report-growth/rmtree/upload variability covered by allowance, not measured zero'])


def block(revisions, case, operations, deadline, control=False, sink=None, *, pair_count=INITIAL_PAIRS, identity=None, histories=1):
    if pair_count not in T95:
        raise ValueError('unsupported independent pair count')
    start = time.monotonic()
    pairs = []
    saved = sink if sink is not None else {}
    saved.update(samples=pairs,expected_pairs=pair_count,started_at=utc_now())
    if case.lifecycle=='insert_trajectory':
        saved['pairs'] = []
    try:
        for round_no in range(pair_count):
            pair = [None,None]
            pairs.append(pair)
            order = [0,1] if round_no%2 == 0 else [1,0]
            if case.lifecycle=='insert_trajectory':
                evidence = {}
                saved['pairs'].append(evidence)
                values = insert_observation(revisions,case,identity,operations,histories,deadline,evidence,order=order,control=control)
                pair[:] = values  # valid only after BOTH deferred checks
            else:
                for side in order:
                    conn = revisions[side].conn
                    prepare(conn,case)
                    elapsed(conn,case,1,deadline)  # independent warmed batch, excluded
                    pair[side] = elapsed(conn,case,operations,deadline,0.075 if control and side else 0)
        saved.update(interval(pairs))
    finally:
        saved.update(finished_at=utc_now(),runtime_seconds=time.monotonic()-start)
    return saved


def measure(revisions, case, deadline, control=False, sink=None, *, frozen=None, report=None, output=None, force_confirmation=False, reserve=0):
    if report is None:
        report={}
    with run_phase(report,case.name+('_controlled' if control else '_clean')):
        return _measure(revisions,case,deadline,control,sink,frozen=frozen,report=report,output=output,force_confirmation=force_confirmation,reserve=reserve)


def _measure(revisions, case, deadline, control=False, sink=None, *, frozen=None, report=None, output=None, force_confirmation=False, reserve=0):
    start = time.monotonic()
    result = dict(name=case.name,group=case.group,smoke=case.smoke,status='inconclusive',
                  timing_status='inconclusive',complete=False,phase='validation',started_at=utc_now(),
                  lifecycle=case.lifecycle,workload_version=case.workload_version,phase_runtime_seconds={})
    if sink is not None:
        sink.append(result)
    inserted = case.lifecycle=='insert_trajectory'
    try:
        if inserted:
            result['phase'] = 'identity'
            identity = insert_identity(revisions)
            result['identity'] = identity
            result['phase'] = 'calibration'
            calibration_start = time.monotonic()
            result['calibration'] = {}
            if frozen is None:
                calibrate_insert(revisions,case,identity,deadline,result['calibration'])
            else:
                require_equal(identity,frozen['identity'],'frozen INSERT identity')
                result['calibration'] = frozen['calibration']
                result['calibration_reused_from'] = frozen['name']
            result['phase_runtime_seconds']['calibration'] = time.monotonic()-calibration_start
            calibration = result['calibration']
            if control:
                result['operations'] = calibration['operations']
                result['phase'] = 'control_preflight'
                cpu_work=freeze_cpu_work(calibration,report['cpu_calibration'])
                result['control_budget'] = insert_control_budget(frozen,deadline,report.get('post_control_costs',{}),
                    cpu_work=cpu_work,sources=(report['baseline_ref'],report['candidate_ref'],report['config']['tier']),
                    completed_work=report.get('completed_control_work',{}),
                    reserve_seconds=report.get('budget',{}).get('cleanup_report_reserve_seconds',0))
                report['mutation_control_preflight'] = dict(
                    recorded_at=utc_now(),phase='before_fault',fault_installed=False,
                    workload_version=case.workload_version,identity=identity,
                    operations=calibration['operations'],histories=calibration['histories'],calibration=calibration,
                    control_budget=result['control_budget'],
                    clean_validation=frozen['validation'],clean_blocks={name:frozen[name] for name in ('initial','confirmation') if name in frozen})
                # Existing artifacts must be closed on disk before the first fault DDL.
                write_report(output,report)
                fresh_remaining = deadline-time.monotonic()
                result['control_admission'] = dict(remaining_after_persistence_seconds=fresh_remaining,
                    workflow_remaining_after_persistence_seconds=fresh_remaining+result['control_budget']['reserve_seconds'])
                if not result['control_budget']['evidence_complete']:
                    raise Inconclusive('control not admitted: required clean/post-control cost evidence is incomplete')
                if (result['control_budget']['predicted_seconds']>fresh_remaining
                        or result['control_budget']['predicted_total_remaining_seconds']>fresh_remaining+result['control_budget']['reserve_seconds']):
                    raise BudgetExpired('insufficient fresh remaining budget after control preflight persistence')
                result['control'] = dict(kind='per_new_row_cpu_trigger_v1',work=cpu_work,
                                         seed_max_id=identity['seed_max_id'],counter='perf_gate.delayed_rows')
                result['phase'] = 'control_setup'
                # Track ownership before fallible DDL; failed transactional setup rolls back.
                with run_phase(report,'active_fault_setup'):
                    install_insert_control(revisions[1].conn,identity['seed_max_id'],cpu_work['iterations'],sink=result['control'])
            histories=calibration['histories']
            if report is not None and report.get('config',{}).get('insert_feasibility'):
                report['coverage'].update(histories=histories,expected_history_pairs=1+13*histories,expected_restoration_checkpoints=2*(1+13*histories),
                                          expected_resets=4*(1+13*histories))
            validation_start = time.monotonic()
            result['phase'] = 'validation'
            result['validation'] = {}
            validate_insert(revisions,case,identity,calibration['operations'],deadline,result['validation'],control=cpu_work if control else False,histories=histories)
            result['phase_runtime_seconds']['validation'] = time.monotonic()-validation_start
        else:
            result['validation'] = validate_case(revisions,case)
            result['phase_runtime_seconds']['validation'] = time.monotonic()-start
            result['phase'] = 'calibration'
            calibration_start = time.monotonic()
            calibration = calibrate(revisions,case,deadline)
            result['phase_runtime_seconds']['calibration'] = time.monotonic()-calibration_start
            result['calibration'] = calibration
        operations = calibration['operations']
        result['operations'] = operations
        if force_confirmation:
            write_report(output,report)
        result['phase'] = 'initial'
        result['initial'] = {}
        options = {'identity':identity,'histories':histories} if inserted else {}
        block_control=cpu_work if control and inserted else control
        result['initial'].update(block(revisions,case,operations,deadline,block_control,sink=result['initial'],**options))
        result['status'] = verdict(result['initial'])
        if force_confirmation:
            write_report(output,report)
        if result['status'] == 'suspect' or force_confirmation or (control and inserted):
            result['status'] = 'inconclusive'
            result['phase'] = 'confirmation'
            result['confirmation'] = {}
            pair_count = READ_CONFIRMATION_PAIRS if case.lifecycle=='read' else MUTATION_CONFIRMATION_PAIRS
            result['confirmation'].update(block(revisions,case,operations,deadline,block_control,
                                                sink=result['confirmation'],pair_count=pair_count,**options))
            result['status'] = verdict(result['confirmation'],True)
            if force_confirmation:
                write_report(output,report)
            if result['status'] == 'inconclusive' and not (control or force_confirmation):
                result['extension_budget'] = extension_fits(result['confirmation'],deadline,reserve)
                if result['extension_budget']['fits']:
                    result['phase'] = 'extension'
                    result['extension'] = {}
                    result['extension'].update(block(revisions,case,operations,deadline,False,sink=result['extension'],
                                                     pair_count=EXTENSION_PAIRS,**options))
                    result['status'] = verdict(result['extension'],True)
        result['timing_status'] = result['status']
        if inserted:
            result['achieved_windows']={name:dict(sql_seconds=[result[name][key] for key in ('baseline_median','candidate_median')],
                target_seconds=TARGET,target_fractions=[result[name][key]/TARGET for key in ('baseline_median','candidate_median')],
                target_attained=min(result[name][key] for key in ('baseline_median','candidate_median'))>=TARGET)
                for name in ('initial','confirmation','extension') if name in result and 'n' in result[name]}
        if result['validation']['size_checks'] is not None:
            result['space_status'] = result['validation']['size_checks']['status']
            if result['space_status']=='regression':
                result['status'] = 'regression'
        result['complete'],result['phase'] = True,'complete'
        return result
    finally:
        try:
            if result.get('control',{}).get('infrastructure_committed'):
                primary=sys.exc_info()[1]
                with run_phase(report,'active_fault_removal') as removal:
                    result['control']['removed'] = remove_insert_control(revisions[1].conn,result,primary)
                    if not result['control']['removed']: removal['error']='active SQL cleanup incomplete; see case.cleanup_errors'
        finally:
            result['runtime_seconds'] = time.monotonic()-start


def normal_size_checks(report):
    return report['size_checks'] + [c['validation']['size_checks'] for c in report['cases']
                                   if c.get('validation',{}).get('size_checks')] + [
        v['size_checks'] for v in report.get('acceptance_catalog_validation',{}).values() if v.get('size_checks')]


def reduce_report(report):
    """Evidence survives interruption; coverage determines whether PASS is possible."""
    sizes = normal_size_checks(report)
    size_regression = any(s['status']=='regression' for s in sizes)
    if report['size_complete']:
        report['size_complete'] = bool(sizes) and set(report['required_size_states']) <= {s['state'] for s in sizes}
    report['size_status'] = ('regression' if size_regression else
                             'pass' if report['size_complete'] and sizes else 'inconclusive')
    if report['status']=='error':
        return
    if report['config']['verify_control']:
        report['status'] = ('pass' if report['complete'] and report['size_status']=='pass'
                             and report.get('acceptance_pass',False) else 'inconclusive')
        return
    statuses = {c['timing_status'] for c in report['cases']}
    report['status'] = ('regression' if size_regression or 'regression' in statuses else
                        'inconclusive' if not report['complete'] or statuses!={'pass'} or report['size_status']!='pass' else 'pass')


def reduce_insert_feasibility(report):
    """Scoped unchanged diagnostic; neither normal coverage nor acceptance is evaluated."""
    report.update(normal_gate_evaluated=False,acceptance_evaluated=False,
                  diagnostic_complete=report['complete'] and report['status']!='error',complete=False,size_complete=False,
                  size_status='not_evaluated')
    if report['status']=='error':
        report['diagnostic_status'],report['status'] = 'error','diagnostic_error'
        return
    case = report['cases'][0] if report['cases'] else {}
    sizes = case.get('validation',{}).get('size_checks')
    report['diagnostic_size_status'] = sizes['status'] if sizes else 'inconclusive'
    initial,confirmation = case.get('initial',{}),case.get('confirmation',{})
    costs = []
    try:
        operations = case.get('operations')
        clean_full_n_pair_cost(case.get('calibration',{}).get('pilot_pair',{}),INSERT_PILOT,'pilot')
        histories=case['calibration']['histories']
        costs.append(clean_observation_cost(case.get('validation',{}).get('pair',{}),operations,histories,'validation'))
        for block in (initial,confirmation):
            if len(block.get('pairs',[]))!=6:
                raise Inconclusive('diagnostic requires six complete pair evidence records in each block')
            costs.extend(clean_observation_cost(pair,operations,histories,'verdict') for pair in block['pairs'])
    except Inconclusive as exc:
        report['diagnostic_complete'],report['diagnostic_evidence_error'] = False,str(exc)
    else:
        report['clean_full_N_cost_evidence'] = dict(max_clean_full_N_pair_wall=max(costs),pair_walls=costs,
                                                  provenance='complete clean full-N validation and verdict pairs; pilot excluded')
    report['required_control_costs'] = {name:'NOT_MEASURED' for name in CONTROL_COST_PHASES}
    known_regression = (sizes and sizes['status']=='regression') or (
        'lower' in confirmation and confirmation['lower']>MATERIAL)
    both_pass = all(block.get('n')==6 and block['upper']<=MATERIAL for block in (initial,confirmation))
    status = ('regression' if known_regression else 'pass' if report['diagnostic_complete'] and both_pass
              and report['diagnostic_size_status']=='pass' else 'inconclusive')
    report['diagnostic_status'],report['status'] = status,'diagnostic_'+status
    report['achieved_windows'] = {name:dict(baseline_seconds=b['baseline_median'],candidate_seconds=b['candidate_median'],
                                          target_seconds=TARGET,target_fractions=[b['baseline_median']/TARGET,b['candidate_median']/TARGET],
                                          target_attained=min(b['baseline_median'],b['candidate_median'])>=TARGET)
                                  for name,b in (('initial',initial),('confirmation',confirmation)) if 'n' in b}


def write_report(output, report):
    with run_phase(report,'report_write') as phase:
        _write_report(output,report)
        phase['artifact_bytes']={name:(output/name).stat().st_size for name in ('results.json','summary.md')}


def _write_report(output, report):
    (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    lines = ['# INSERT feasibility diagnostic' if report['config'].get('insert_feasibility') else '# Focused performance gate', '', f"Status: **{report['status']}**. {report['date']}.",
             f"Normal size gate: **{report['size_status']}**.",
             f"Complete: **{report['complete']}**. Size coverage complete: **{report['size_complete']}**. Budget exhausted: **{report['budget_exhausted']}**.",
             f"Baseline: `{report['baseline_ref']}`. Candidate: `{report['candidate_ref']}`.",
             f"PostgreSQL: {report.get('postgresql','unknown')}. CPU: {report['cpu']}; CPUs: {report['cpus']}.",
             f"Runner: {report['runner']}. Load: {report['load']}.",
             'Metric: monotonic elapsed persistent SQL end-to-end, including planning and result receipt.',
             'Six initial alternating warmed pairs; one fresh confirmation: 16 read pairs or 6 mutation pairs; a confirmation still inconclusive takes one fresh 16-pair extension when the budget covers it and the cases after it. Ordinary cases confirm suspects; controlled INSERT and INSERT feasibility always confirm and never extend. Fixed common operations; 95% Student-t log-ratio CI; material ratio 1.10.',
             '']
    if report['config'].get('insert_feasibility'):
        lines.extend(['', 'Scope: INSERT PR geometry only. Normal gate and acceptance NOT evaluated.',
                      'Diagnostic coverage: `'+json.dumps(report.get('coverage',{}))+'`.',
                      'Achieved windows: `'+json.dumps(report.get('achieved_windows',{}))+'`.',
                      'A diagnostic pass does not establish adequate SQL window, power or gate readiness.'])
    lines.extend(['', '| Case | Phase | n | Ratio | 95% CI | Base median [min,max] s | Head median [min,max] s | Status |',
                  '|---|---|---:|---:|---|---|---|---|'])
    for case in report['cases']:
        for phase in ('initial','confirmation','extension'):
            if phase in case and 'n' in case[phase]:
                b = case[phase]
                lines.append(f"| {case['name']} | {phase} | {b['n']} | {b['ratio']:.3f} | [{b['lower']:.3f}, {b['upper']:.3f}] | {b['baseline_median']:.4f} {b['baseline_range']} | {b['candidate_median']:.4f} {b['candidate_range']} | {case['status']} |")
    lines.extend(['', '| Case / workload | Phase | Complete | Timing | Combined | Seed / first ID / N |',
                  '|---|---|---|---|---|---|'])
    for case in report['cases']:
        identity = case.get('identity',{})
        details = f"{identity.get('seed_count','-')} / {identity.get('first_id','-')} / {case.get('operations','-')}"
        lines.append(f"| {case['name']} / {case['workload_version']} | {case['phase']} | {case['complete']} | {case['timing_status']} | {case['status']} | {details} |")
    for case in report['cases']:
        if 'control_budget' in case:
            lines.extend(['', 'Mutation control budget: `'+json.dumps(case['control_budget'])+'`.', ''])
        if case.get('lifecycle')=='insert_trajectory':
            lines.extend(['', 'INSERT work: `'+json.dumps({k:case.get('calibration',{}).get(k) for k in
                         ('operations','histories','desired_histories','history_cap','predicted_observation_sql_seconds',
                          'target_attained_prediction','internal_order_policy','limitations')})+'`.',
                         'Actual achieved windows: `'+json.dumps(case.get('achieved_windows',{}))+'`. All histories contribute client SQL sums to one observation; n counts observations only. Per-operation Python/encoding/deadline/libpq/autocommit overhead retained, with all ordinal spikes. No bias-correction or sqrt(K) precision claim.'])
    lines.extend(['', 'Physical schedule: `'+report.get('physical_schedule_version','normal catalog order')+'`.',
                  'Measured phase ledger (nested phases are explanatory): `phase_ledger` in results.json; final closed-writer accounting: `terminal-accounting.json`.',
                  'Same-invocation future cost estimates: `'+json.dumps(report.get('post_control_costs',{}))+'`. Estimates are not bounds.',
                  'Cleanup errors: `'+json.dumps(report.get('cleanup_errors',[]))+'`.',
                  'UNIX socket paths: `'+json.dumps(report.get('unix_socket_paths',{}))+'`.',
                  'Server log preservation: `'+json.dumps(report.get('server_logs',{}))+'`.'])
    lines.extend(['', 'Required size states: `'+json.dumps(report.get('required_size_states',[]))+'`.'])
    lines.extend(['', '| Size state | Relation / role | Base bytes/pages | Head bytes/pages | Ratio | Limit bytes | Status |', '|---|---|---|---|---:|---:|---|'])
    for snapshot in normal_size_checks(report) + ([report['size_control']] if 'size_control' in report else []):
        for r in snapshot['relations']:
            lines.append(f"| {snapshot['state']} | {r['relation']} / {r['role']} | {r['baseline_bytes']} / {r['baseline_pages']} | {r['candidate_bytes']} / {r['candidate_pages']} | {r['ratio']} | {r['threshold_bytes']} | {r['status']} |")
    lines.extend(['', 'Phase runtime (seconds): `'+json.dumps(report['runtime_seconds'])+'`.',
                  'Configuration: `'+json.dumps(report['config'])+'`.',
                  'Budget envelope: `'+json.dumps(report.get('budget',{}))+'`. Cleanup/report reserve is an allowance, not a hard deadline.',
                  'Fixture checkpoint barrier: `'+json.dumps(report.get('fixture_checkpoint_barrier',{}))+'`.',
                  'This is a small warm serial workload on the recorded host; no hardware-counter or production throughput claim.'])
    if 'mutation_control_preflight' in report:
        preflight = report['mutation_control_preflight']
        lines.extend(['', 'Mutation control preflight (persisted before fault setup):',
                      f"N={preflight['operations']}; clean calibration, raw pilots and caps: `mutation_control_preflight.calibration` in results.json.",
                      'Budget: `'+json.dumps(preflight['control_budget'])+'`.'])
    if 'error' in report:
        lines.extend(['', 'Error: '+report['error']])
    (output/'summary.md').write_text('\n'.join(lines)+'\n')


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline-ref',required=True)
    p.add_argument('--candidate-ref',required=True)
    p.add_argument('--baseline-tree')
    p.add_argument('--candidate-tree')
    p.add_argument('--groups',default=None)
    p.add_argument('--insert-feasibility',action='store_true',help='equal-source INSERT PR-only diagnostic; always fixed6+6, no controls/normal gate')
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--tier',choices=['pr','nightly'],default='pr')
    p.add_argument('--verify-control',action='store_true',help='acceptance only: unchanged and SQL-visible slowdown; requires equal SHAs')
    p.add_argument('--scratch-root',type=Path,required=True,help='owned DB/build scratch filesystem (dedicated test partition on shared local host)')
    p.add_argument('--pg-config',default=shutil.which('pg_config'))
    p.add_argument('--port-base',type=int,default=56240)
    p.add_argument('--budget-seconds',type=float,default=300,help='global budget including builds/setup; exhaustion is inconclusive')
    p.add_argument('--workflow-start-seconds',type=float,help='workflow Unix start timestamp; prior elapsed time is deducted from the budget')
    p.add_argument('--cleanup-report-reserve-seconds',type=float,default=0,help='allowance deducted before builds/measurement; not a hard cleanup deadline')
    return p


def new_report(args):
    return dict(schema_version=4,complete=False,size_complete=False,budget_exhausted=False,date=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  baseline_ref=args.baseline_ref,candidate_ref=args.candidate_ref,status='inconclusive',cases=[],size_checks=[],size_status='inconclusive',
                  cpu=next((l.split(':',1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines() if l.startswith('model name')),platform.machine()),
                  cpus=os.cpu_count(),runner=os.environ.get('RUNNER_NAME',platform.node()),load=os.getloadavg(),
                  config=dict(tier=args.tier,groups=args.groups,insert_feasibility=args.insert_feasibility,initial_pairs=INITIAL_PAIRS,
                              read_confirmation_pairs=READ_CONFIRMATION_PAIRS,mutation_confirmation_pairs=MUTATION_CONFIRMATION_PAIRS,
                              supported_pairs=list(T95),t_critical_by_pairs=T95,target_seconds=TARGET,threshold=MATERIAL,
                              insert_pilot_operations=INSERT_PILOT,insert_growth_cap_operations=INSERT_GROWTH_CAP,
                              insert_history_cap=INSERT_HISTORY_CAP,cpu_pilot_rows=CPU_PILOT_ROWS,cpu_pilot_iterations=CPU_PILOT_ITERATIONS,cpu_iteration_cap=CPU_ITERATION_CAP,budget_seconds=args.budget_seconds,verify_control=args.verify_control,build_workers=2,
                              build_flags='USE_PGXS=1 SIFT_TESTING= SIFT_LTO= SANITIZE= VULKAN=',settings=SETTINGS),runtime_seconds={})


def verify_control(revisions,report,args,deadline):
        report['physical_schedule_version']='acceptance_reads_sizes_infrastructure_then_composite_insert_v4'
        report['acceptance_catalog_validation'] = {}
        for catalog_case in CASES:
            if catalog_case.lifecycle=='insert_trajectory':
                continue
            validation = dict(complete=False)
            report['acceptance_catalog_validation'][catalog_case.name] = validation
            with run_phase(report,'catalog_'+catalog_case.name):
                validation.update(validate_case(revisions,catalog_case))
            validation['complete'] = True
        case = next(c for c in CASES if c.name=='storage_point_lookup')
        unchanged = measure(revisions,case,deadline,sink=report['cases'],report=report,output=args.output)
        unchanged['name'] = 'control_unchanged'
        slowed = measure(revisions,case,deadline,True,sink=report['cases'],report=report,output=args.output)
        slowed['name'] = 'control_sql_slowdown'
        with run_phase(report,'churn_normal_sizes'):
            report['size_checks'].append(churn_sizes(revisions))
        with run_phase(report,'bm25_growth_logical_forks'):
            before = revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs")
            # Explicit acceptance-only SQL fault: force many small BM25 build segments.
            revisions[1].conn.script('ALTER INDEX docs_bm25 SET (build_segments=64); REINDEX INDEX docs_bm25')
            require_equal(before,revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs"),'size control logical rows')
            report['size_control'] = size_snapshot(revisions,'control_build_segments_64')
        prepare_control_costs(revisions,report,args.output)
        insert = next(c for c in CASES if c.lifecycle=='insert_trajectory')
        unchanged_insert = measure(revisions,insert,deadline,sink=report['cases'],report=report,output=args.output)
        unchanged_insert['name'] = 'control_insert_unchanged'
        report['acceptance_catalog_validation'][insert.name] = unchanged_insert['validation']
        write_report(args.output,report)
        refresh_report_cost(report)
        slowed_insert = measure(revisions,insert,deadline,True,sink=report['cases'],frozen=unchanged_insert,
                                report=report,output=args.output)
        slowed_insert['name'] = 'control_insert_per_row_slowdown'
        report['acceptance_pass'] = (unchanged['status']=='pass' and slowed['timing_status']=='regression'
                                     and unchanged_insert['status']=='pass' and slowed_insert['timing_status']=='regression'
                                     and slowed_insert['confirmation']['n']==MUTATION_CONFIRMATION_PAIRS
                                     and slowed_insert['confirmation']['lower']>MATERIAL
                                     and all(s['status']=='pass' for s in normal_size_checks(report))
                                     and report['size_control']['status']=='regression')


def stop_revisions(revisions,scratch,report,args,cleanup_phase):
    for rev in reversed(revisions):
        try:
            rev.stop(args.output)
        except Exception as exc:
            report.setdefault('cleanup_errors',[]).append(f'{type(exc).__name__}: {exc}')
            cleanup_phase['error']='; '.join(report['cleanup_errors'])
            if report['status']!='error':
                report['status'],report['error']='error',f'cleanup failed: {exc}'
        finally:
            report.setdefault('server_logs',{})[rev.name]=rev.server_log_evidence
    if scratch and not any(r.started for r in revisions) and not report.get('cleanup_errors'):
        try:
            shutil.rmtree(scratch)
        except Exception as exc:
            report.setdefault('cleanup_errors',[]).append(f'scratch removal: {exc}')
            cleanup_phase['error']='; '.join(report['cleanup_errors'])
            if report['status']!='error': report['status'],report['error']='error',f'scratch cleanup failed: {exc}'


def write_terminal(report,args,start,end):
    terminal=dict(status=report['status'],baseline_ref=args.baseline_ref,candidate_ref=args.candidate_ref,
                  accounting=reconcile_ledger(report,start,end),phase_ledger=report.get('phase_ledger',[]),
                  driver_seconds=end-start,workflow_seconds=report.get('budget',{}).get('prior_elapsed_seconds',0)+end-start,
                  measurement_deadline_exceeded=end>DEADLINE,
                  workflow_budget_exceeded=report.get('budget',{}).get('prior_elapsed_seconds',0)+end-start>args.budget_seconds,
                  boundary='through closed final results/summary writer; terminal sidecar write, device cleanup and artifact upload outside',
                  errors=report.get('terminal_errors',[]),primary_error=report.get('error'),cleanup_errors=report.get('cleanup_errors',[]))
    (args.output/'terminal-accounting.json').write_text(json.dumps(terminal,indent=2,allow_nan=False)+'\n')


def record_source_identity(args,report):
    report['postgresql'] = command([args.pg_config,'--version'])
    if not report['postgresql'].startswith('PostgreSQL 18.'):
        raise ValueError('performance gate requires PostgreSQL 18')
    report['compiler'] = command(['cc','--version']).splitlines()[0]
    report['pg_config'] = command([args.pg_config,'--configure'])
    files = ('bench/steps/perf_gate.py','bench/perf/catalog.py','bench/perf/fixture.sql')
    blobs = {}
    for name in files:
        blobs[name] = command(['git','rev-parse',args.candidate_ref+':'+name],cwd=ROOT)
        require_equal(command(['git','hash-object',name],cwd=ROOT),blobs[name],'committed candidate harness '+name)
    report['source_identity'] = dict(commit=args.candidate_ref,tree=command(['git','rev-parse',args.candidate_ref+'^{tree}'],cwd=ROOT),
                                     harness_blobs=blobs,harness_sha256={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in files},
                                     fixture_owner='candidate harness, loaded identically into both isolated exact-SHA builds')


def check_fixture(revisions,report):
    report['fixture_sha256'] = hashlib.sha256((ROOT/'bench/perf/fixture.sql').read_bytes()).hexdigest()
    digests = [r.conn.rows("SELECT count(*),md5(string_agg(row(seed.*)::text,'|' ORDER BY id)) FROM seed") for r in revisions]
    require_equal(digests[0],digests[1],'logical fixture')
    report['fixture_digest'] = digests[0]
    # The padded heap must really cross superblock boundaries.
    for rev in revisions:
        boundaries = int(rev.conn.rows("SELECT count(DISTINCT ((ctid::text::point)[0]::int/256)) FROM docs")[0][0])
        if boundaries < 3:
            raise ValueError('fixture fails multi-superblock boundary')
    report['table_geometry'] = {r.name:table_geometry(r.conn,'store') for r in revisions}
    for rev in revisions:
        geometry = report['table_geometry'][rev.name]
        if geometry['column_segments']<1 or geometry['units']<3 or geometry['relation_blocks']<256:
            raise ValueError('actual sift_table fixture fails segment/page boundary')


def main(argv=None):
    global DEADLINE
    args = parser().parse_args(argv)
    groups_supplied = args.groups is not None
    args.groups = args.groups or ''
    args.output = args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    report = new_report(args)
    revisions = []
    scratch = None
    start = time.monotonic()
    DEADLINE = start
    measurement_start = None
    try:
        report['budget'] = budget_context(args.budget_seconds,args.workflow_start_seconds,args.cleanup_report_reserve_seconds)
        DEADLINE = start+report['budget']['measurement_budget_seconds']
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('workflow budget exhausted before source/build setup')
        if args.insert_feasibility:
            if (groups_supplied or args.verify_control or args.tier!='pr' or args.baseline_tree or args.candidate_tree):
                raise ValueError('insert-feasibility requires PR tier without groups, controls or tree overrides')
            require_equal(args.baseline_ref,args.candidate_ref,'insert-feasibility requires equal exact revisions')
            report.update(mode='insert_feasibility',normal_gate_evaluated=False,acceptance_evaluated=False,
                          coverage_scope='storage_insert_pr_only',
                          coverage=dict(requested_cases=['storage_insert'],omitted_cases=[c.name for c in CASES if c.name!='storage_insert'],
                                        normal_size_states='not_evaluated',pilot_pairs=1,validation_pairs=1,
                                        initial_pairs=6,confirmation_pairs=6,statistical_observations=12,
                                        expected_history_pairs=None,expected_restoration_checkpoints=None,expected_resets=None))
        with run_phase(report,'source_build_fixture_preconditions'):
            exact_sha(args.baseline_ref)
            exact_sha(args.candidate_ref)
            if args.verify_control:
                require_equal(args.baseline_ref,args.candidate_ref,'control requires unchanged revisions')
            if not 56100 <= args.port_base <= 56298:
                raise ValueError('port-base must be in 56100..56298')
            selected = select([g for g in args.groups.split(',') if g],args.tier)
            report['required_size_states'] = ['settled_build','churn_vacuum'] + [
                state for lifecycle,state in (('insert_trajectory','mutable_insert_trajectory'),
                                              ('reset_each_operation','mutable_vacuum'))
                if (args.verify_control or any(c.lifecycle==lifecycle and (lifecycle=='insert_trajectory' or c.name=='storage_vacuum_space') for c in selected))]
            if args.insert_feasibility:
                report['required_size_states'] = ['mutable_insert_trajectory']
            record_source_identity(args,report)
            write_report(args.output,report)
            args.scratch_root.mkdir(parents=True,exist_ok=True)
            scratch = Path(tempfile.mkdtemp(prefix='perf-gate-',dir=args.scratch_root))
            for i, (name,sha,tree) in enumerate([('baseline',args.baseline_ref,args.baseline_tree),('candidate',args.candidate_ref,args.candidate_tree)]):
                phase = time.monotonic()
                rev = Revision(name,sha,tree,scratch,args.output,str(Path(args.pg_config).resolve()),args.port_base+i)
                revisions.append(rev)
                report.setdefault('unix_socket_paths',{})[name] = rev.socket_path
            write_report(args.output,report)
            for rev in revisions:
                phase = time.monotonic()
                name = rev.name
                report.setdefault('cluster_config',{})[name] = rev.setup(32768 if args.tier=='nightly' else 8192)
                report.setdefault('setup_phases',{})[name] = rev.timings
                report.setdefault('versions',{})[name] = rev.versions
                report['runtime_seconds'][name+'_build_cluster_fixture'] = time.monotonic()-phase
            report['fixture_checkpoint_barrier'] = {}
            finalize_fixtures(revisions,DEADLINE,report['fixture_checkpoint_barrier'])
            report['runtime_seconds']['fixture_checkpoint_barrier'] = report['fixture_checkpoint_barrier']['runtime_seconds']
            check_fixture(revisions,report)
            if not args.insert_feasibility:
                report['initial_sizes'] = {r.name:sizes(r.conn) for r in revisions}
                report['size_checks'].append(size_snapshot(revisions,'settled_build'))
        phase = time.monotonic()
        measurement_start = phase
        deadline = DEADLINE
        with run_phase(report,'measurement_validation'):
            if args.insert_feasibility:
                insert = next(c for c in CASES if c.lifecycle=='insert_trajectory')
                write_report(args.output,report)
                measure(revisions,insert,deadline,sink=report['cases'],force_confirmation=True,report=report,output=args.output)
            elif args.verify_control:
                verify_control(revisions,report,args,deadline)
            else:
                for index, case in enumerate(selected):
                    later = sum(c.lifecycle != 'read' for c in selected[index+1:])
                    measure(revisions,case,deadline,sink=report['cases'],report=report,output=args.output,reserve=CASE_RESERVE_SECONDS*later)
                    write_report(args.output,report)
                report['size_checks'].append(churn_sizes(revisions))
            if time.monotonic()>=deadline:
                raise BudgetExpired('global runtime budget exhausted after final size checks')
            report['complete'],report['size_complete'] = True,True
            report['runtime_seconds']['measurement_validation'] = time.monotonic()-phase
    except Inconclusive as exc:
        report['budget_exhausted'],report['error'] = isinstance(exc,BudgetExpired),str(exc)
    except Exception as exc:
        report['status'],report['error'] = 'error',f'{type(exc).__name__}: {exc}'
        if getattr(exc,'__notes__',None):
            report['error'] += '; '+'; '.join(exc.__notes__)
    finally:
        if 'fixture_checkpoint_barrier' in report:
            report['runtime_seconds']['fixture_checkpoint_barrier'] = report['fixture_checkpoint_barrier']['runtime_seconds']
        if measurement_start is not None:
            report['runtime_seconds']['measurement_validation'] = time.monotonic()-measurement_start
        phase = time.monotonic()
        with run_phase(report,'final_cluster_teardown') as cleanup_phase:
            stop_revisions(revisions,scratch,report,args,cleanup_phase)
        report['runtime_seconds']['cleanup'] = time.monotonic()-phase
        report['runtime_seconds']['total_before_final_reduce_write'] = time.monotonic()-start
        report['terminal_accounting_file']='terminal-accounting.json (includes final reducer/writer; excludes this sidecar write and outer upload)'
        try:
            with run_phase(report,'final_reduce'):
                if args.insert_feasibility:
                    reduce_insert_feasibility(report)
                else:
                    reduce_report(report)
            write_report(args.output,report)
        except Exception as exc:
            report.setdefault('terminal_errors',[]).append(f'{type(exc).__name__}: {exc}')
            if report['status'] not in ('error','diagnostic_error'):
                report['status'],report['error']='error',f'final report failed: {exc}'
        end=time.monotonic()
        prior=report.get('budget',{}).get('prior_elapsed_seconds',0)
        if prior+end-start>args.budget_seconds and report['status'] in ('pass','diagnostic_pass'):
            # A late final writer invalidates PASS. One status publication; no sampling retry.
            report.update(status='diagnostic_inconclusive' if args.insert_feasibility else 'inconclusive',
                          complete=False,budget_exhausted=True,error='whole workflow envelope exceeded during final artifacts')
            if args.insert_feasibility: report.update(diagnostic_complete=False,diagnostic_status='inconclusive')
            try:
                write_report(args.output,report)
            except Exception as exc:
                report['status']='error'
                report.setdefault('terminal_errors',[]).append(f'budget status publication failed: {exc}')
            end=time.monotonic()
        write_terminal(report,args,start,end)
    print(f"{report['status']}: {args.output/'summary.md'}")
    return {'pass':0,'regression':1,'inconclusive':2,'error':3,
            'diagnostic_pass':0,'diagnostic_regression':1,'diagnostic_inconclusive':2,'diagnostic_error':3}[report['status']]


if __name__=='__main__':
    sys.exit(main())
