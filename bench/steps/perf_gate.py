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
TARGET = 0.150
MATERIAL = 1.10
T95 = {6:  2.570581835636305,
       16: 2.131449545559323}
DEADLINE = math.inf
PROCESS_ORIGIN = time.monotonic()
BATCH_WALL_LIMIT = 1.0
INSERT_PILOT = 2048
INSERT_GROWTH_CAP = 2048
INSERT_CONTROL_DELAY = .001
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


class Revision:
    def __init__(self, name, sha, tree, scratch, output, pg_config, port):
        self.name, self.sha = name, sha
        self.base = scratch / name
        self.base.mkdir()
        self.tree = self.base / 'source'
        self.data = self.base / 'cluster'
        self.stage = self.base / 'stage'
        self.socket = self.base / 'sock'
        self.socket.mkdir()
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
        with (self.data/'postgresql.conf').open('a') as f:
            f.write(config)
        self.started = True
        command(['nice','-n','19',bindir/'pg_ctl','-D',self.data,'-l',self.base/'server.log','-w','start'],env=env,log=self.log)
        self.timings['cluster_init_start'] = time.monotonic()-phase
        phase = time.monotonic()
        command([bindir/'psql','-X','-v','ON_ERROR_STOP=1','-h',self.socket,'-p',self.port,'-U','postgres','-d','postgres',
                 '-v',f'rows={rows}','-f',ROOT/'bench/perf/fixture.sql'],env=env,log=self.log)
        self.timings['fixture_load'] = time.monotonic()-phase
        lib = cell.libpq()
        lib.PQgetisnull.restype = ctypes.c_int
        lib.PQgetisnull.argtypes = [ctypes.c_void_p,ctypes.c_int,ctypes.c_int]
        self.conn = SQL(lib, f'host={self.socket} port={self.port} user=postgres dbname=postgres application_name=perf-gate')
        self.conn.script(SETTINGS)
        require_equal(self.conn.rows('SHOW block_size'),[['8192']],'PostgreSQL page size')
        self.versions = self.conn.rows("SELECT version(),extversion FROM pg_extension WHERE extname='pg_sift'")
        return config

    def stop(self, output):
        if self.conn:
            self.conn.close()
            self.conn = None
        if self.started and not (self.data/'postmaster.pid').exists():
            self.started = False
        if self.started:
            command([self.bindir/'pg_ctl','-D',self.data,'-m','fast','-w','stop'],log=self.log,bounded=False)
            self.started = False
            shutil.copyfile(self.base/'server.log',output/f'{self.name}-server.log')


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


def install_insert_control(conn, seed_max):
    # Acceptance only. Counter proves actual per-new-row work, including excluded warm work.
    conn.exec(f"""CREATE FUNCTION perf_gate_insert_delay() RETURNS trigger LANGUAGE plpgsql AS $$
    BEGIN
      IF NEW.id > {seed_max} THEN
        PERFORM set_config('perf_gate.delayed_rows',(current_setting('perf_gate.delayed_rows')::bigint+1)::text,false);
        PERFORM pg_sleep(0.001);
      END IF;
      RETURN NEW;
    END $$""")
    conn.exec('CREATE TRIGGER perf_gate_insert_delay BEFORE INSERT ON mutable FOR EACH ROW EXECUTE FUNCTION perf_gate_insert_delay()')
    conn.exec("SET perf_gate.delayed_rows='0'")


def remove_insert_control(conn, sink, primary):
    return cleanup_insert_sql(conn,['DROP TRIGGER perf_gate_insert_delay ON mutable',
                                    'DROP FUNCTION perf_gate_insert_delay()'],sink,primary)


INSERT_PHASES = ('warm_reset','warm','final_reset','preprobes','checkpoint','stream','postcheck')
POST_CONTROL_PHASES = ('fault_removal','churn_normal_sizes','bm25_alter_reindex',
                       'size_control_logical_and_forks','final_report','cluster_teardown')


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
    label_c = time.monotonic()
    sides = [dict(phases={},operation_seconds=[],warm_operation_seconds=[]),
             dict(phases={},operation_seconds=[],warm_operation_seconds=[])]
    sink.update(operations=operations,order=list(order),role=role,clean=not control,
                workload_version=case.workload_version,sides=sides,complete=False,started_at=utc_now(),
                before_observation='pre_finalization',after_observation='pair_end',
                start_offset_seconds=label_c-PROCESS_ORIGIN,phase='warm')
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
                                  'real per-new-row delay counts; seed reset must not delay')
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
    return sink


def validate_insert(revisions, case, identity, operations, deadline, sink, *, control=False):
    sink.update(pair={},size_checks=None,complete=False)
    insert_pair(revisions,case,identity,operations,deadline,sink['pair'],role='validation',control=control)
    sides = sink['pair']['sides']
    sink['size_checks'] = dict(compare_sizes([side['after']['totals'] for side in sides],
                                            'mutable_insert_trajectory',MUTABLE_RELATIONS),
                               forks=[side['after']['forks'] for side in sides])
    sink['complete'] = True
    return sink


def clean_full_n_pair_cost(pair, operations, role):
    """Only complete, finite full-N validation/verdict evidence can fund controls."""
    if (pair.get('workload_version')!=next(c.workload_version for c in CASES if c.lifecycle=='insert_trajectory')
            or not pair.get('complete') or not pair.get('clean') or pair.get('operations')!=operations
            or pair.get('role')!=role or len(pair.get('sides',[]))!=2):
        raise Inconclusive('missing completed clean full-N '+role+' pair cost evidence')
    for side in pair['sides']:
        if any(len(side.get(key,[]))!=operations or any(not math.isfinite(v) or v<=0 for v in side[key])
               for key in ('operation_seconds','warm_operation_seconds')):
            raise Inconclusive('missing complete full-N raw operation evidence')
        if set(side.get('phases',{}))!=set(INSERT_PHASES):
            raise Inconclusive('missing full-N phase cost evidence')
        for phase in side['phases'].values():
            if not phase.get('complete') or any(not math.isfinite(phase.get(key,math.nan)) for key in
                                               ('seconds','start_offset_seconds','end_offset_seconds')) or phase['seconds']<0 or not math.isclose(phase['end_offset_seconds']-phase['start_offset_seconds'],phase['seconds'],abs_tol=1e-9):
                raise Inconclusive('incomplete/nonfinite full-N phase cost evidence')
    wall,phases,residual = (pair.get(key,math.nan) for key in ('wall_seconds','phase_seconds','residual_seconds'))
    actual = sum(p['seconds'] for side in pair['sides'] for p in side['phases'].values())
    if (not all(math.isfinite(v) for v in (wall,phases,residual)) or wall<=0 or residual<0
            or not math.isclose(actual,phases,abs_tol=1e-9) or not math.isclose(wall,phases+residual,abs_tol=1e-9)):
        raise Inconclusive('invalid complete full-N wall/phase/residual evidence')
    return wall


def insert_control_budget(unchanged, deadline, post_control_costs):
    operations = unchanged['calibration']['operations']
    population = []
    try:
        population.append(dict(source='validation',wall_seconds=clean_full_n_pair_cost(
            unchanged.get('validation',{}).get('pair',{}),operations,'validation')))
        for block_name in ('initial','confirmation'):
            for index,pair in enumerate(unchanged.get(block_name,{}).get('pairs',[])):
                if pair.get('complete'):
                    population.append(dict(source=f'{block_name}.pairs[{index}]',wall_seconds=clean_full_n_pair_cost(pair,operations,'verdict')))
    except Inconclusive as exc:
        clean_error = str(exc)
    else:
        clean_error = None
    missing = []
    post = []
    for name in POST_CONTROL_PHASES:
        entry = post_control_costs.get(name)
        if (not entry or not entry.get('complete') or not entry.get('provenance')
                or not math.isfinite(entry.get('seconds',math.nan)) or entry['seconds']<=0):
            missing.append(name)
        else:
            post.append(dict(phase=name,**entry))
    visits = INITIAL_PAIRS+MUTATION_CONFIRMATION_PAIRS+1
    sleep = 2*operations*visits*INSERT_CONTROL_DELAY
    maximum = max(p['wall_seconds'] for p in population) if population and clean_error is None else None
    post_seconds = sum(p['seconds'] for p in post) if not missing else None
    demand = visits*maximum+sleep+post_seconds if maximum is not None and post_seconds is not None else None
    return dict(controlled_pairs=visits,expected_checkpoint_calls=2*visits,
                candidate_delayed_operations=2*operations*visits,configured_delay_seconds=INSERT_CONTROL_DELAY,
                clean_full_N_population=population,clean_evidence_error=clean_error,
                max_clean_full_N_pair_wall=maximum,predicted_clean_pair_seconds=visits*maximum if maximum is not None else None,
                minimum_added_sleep_seconds=sleep,required_post_control_costs=list(POST_CONTROL_PHASES),
                post_control_cost_evidence=post,missing_post_control_costs=missing,post_control_seconds=post_seconds,
                predicted_seconds=demand,evidence_complete=demand is not None,remaining_seconds=deadline-time.monotonic(),
                cost_scope='13 * observed full-pair maximum includes all phases/waits/residual once; pilot excluded; post-control phases disjoint',
                limitations='observed maximum is not a bound; extra trigger seed/counter/scheduler/DDL variability unmeasured; startup already spent')


def block(revisions, case, operations, deadline, control=False, sink=None, *, pair_count=INITIAL_PAIRS, identity=None):
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
                values = insert_pair(revisions,case,identity,operations,deadline,evidence,order=order,control=control)
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


def measure(revisions, case, deadline, control=False, sink=None, *, frozen=None, report=None, output=None, force_confirmation=False):
    start = time.monotonic()
    result = dict(name=case.name,group=case.group,smoke=case.smoke,status='inconclusive',
                  timing_status='inconclusive',complete=False,phase='validation',started_at=utc_now(),
                  lifecycle=case.lifecycle,workload_version=case.workload_version,phase_runtime_seconds={})
    if sink is not None:
        sink.append(result)
    inserted = case.lifecycle=='insert_trajectory'
    installed = False
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
                result['control_budget'] = insert_control_budget(frozen,deadline,report.get('post_control_costs',{}))
                report['mutation_control_preflight'] = dict(
                    recorded_at=utc_now(),phase='before_fault',fault_installed=False,
                    workload_version=case.workload_version,identity=identity,
                    operations=calibration['operations'],calibration=calibration,
                    control_budget=result['control_budget'],
                    clean_validation=frozen['validation'],clean_blocks={name:frozen[name] for name in ('initial','confirmation') if name in frozen})
                # Existing artifacts must be closed on disk before the first fault DDL.
                write_report(output,report)
                fresh_remaining = deadline-time.monotonic()
                result['control_admission'] = dict(remaining_after_persistence_seconds=fresh_remaining)
                if not result['control_budget']['evidence_complete']:
                    raise Inconclusive('control not admitted: required clean/post-control cost evidence is incomplete')
                if result['control_budget']['predicted_seconds']>fresh_remaining:
                    raise BudgetExpired('insufficient fresh remaining budget after control preflight persistence')
                result['control'] = dict(kind='per_new_row_trigger',delay_seconds=INSERT_CONTROL_DELAY,
                                         seed_max_id=identity['seed_max_id'],counter='perf_gate.delayed_rows')
                result['phase'] = 'control_setup'
                install_insert_control(revisions[1].conn,identity['seed_max_id'])
                installed = True
            validation_start = time.monotonic()
            result['phase'] = 'validation'
            result['validation'] = {}
            validate_insert(revisions,case,identity,calibration['operations'],deadline,result['validation'],control=control)
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
        options = {'identity':identity} if inserted else {}
        result['initial'].update(block(revisions,case,operations,deadline,control,sink=result['initial'],**options))
        result['status'] = verdict(result['initial'])
        if force_confirmation:
            write_report(output,report)
        if result['status'] == 'suspect' or force_confirmation or (control and inserted):
            result['status'] = 'inconclusive'
            result['phase'] = 'confirmation'
            result['confirmation'] = {}
            pair_count = READ_CONFIRMATION_PAIRS if case.lifecycle=='read' else MUTATION_CONFIRMATION_PAIRS
            result['confirmation'].update(block(revisions,case,operations,deadline,control,
                                                sink=result['confirmation'],pair_count=pair_count,**options))
            result['status'] = verdict(result['confirmation'],True)
            if force_confirmation:
                write_report(output,report)
        result['timing_status'] = result['status']
        if result['validation']['size_checks'] is not None:
            result['space_status'] = result['validation']['size_checks']['status']
            if result['space_status']=='regression':
                result['status'] = 'regression'
        result['complete'],result['phase'] = True,'complete'
        return result
    finally:
        try:
            if installed:
                result['control']['removed'] = remove_insert_control(revisions[1].conn,result,sys.exc_info()[1])
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
        costs.append(clean_full_n_pair_cost(case.get('validation',{}).get('pair',{}),operations,'validation'))
        for block in (initial,confirmation):
            if len(block.get('pairs',[]))!=6:
                raise Inconclusive('diagnostic requires six complete pair evidence records in each block')
            costs.extend(clean_full_n_pair_cost(pair,operations,'verdict') for pair in block['pairs'])
    except Inconclusive as exc:
        report['diagnostic_complete'],report['diagnostic_evidence_error'] = False,str(exc)
    else:
        report['clean_full_N_cost_evidence'] = dict(max_clean_full_N_pair_wall=max(costs),pair_walls=costs,
                                                  provenance='complete clean full-N validation and verdict pairs; pilot excluded')
    report['required_post_control_costs'] = {name:'NOT_MEASURED' for name in POST_CONTROL_PHASES}
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
    (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    lines = ['# INSERT feasibility diagnostic' if report['config'].get('insert_feasibility') else '# Focused performance gate', '', f"Status: **{report['status']}**. {report['date']}.",
             f"Normal size gate: **{report['size_status']}**.",
             f"Complete: **{report['complete']}**. Size coverage complete: **{report['size_complete']}**. Budget exhausted: **{report['budget_exhausted']}**.",
             f"Baseline: `{report['baseline_ref']}`. Candidate: `{report['candidate_ref']}`.",
             f"PostgreSQL: {report.get('postgresql','unknown')}. CPU: {report['cpu']}; CPUs: {report['cpus']}.",
             f"Runner: {report['runner']}. Load: {report['load']}.",
             'Metric: monotonic elapsed persistent SQL end-to-end, including planning and result receipt.',
             'Six initial alternating warmed pairs; one fresh confirmation: 16 read pairs or 6 mutation pairs. Ordinary cases confirm suspects; controlled INSERT and INSERT feasibility always confirm. Fixed common operations; 95% Student-t log-ratio CI; material ratio 1.10.',
             '', '| Case | Phase | n | Ratio | 95% CI | Base median [min,max] s | Head median [min,max] s | Status |',
             '|---|---|---:|---:|---|---|---|---|']
    if report['config'].get('insert_feasibility'):
        lines.extend(['', 'Scope: INSERT PR geometry only. Normal gate and acceptance NOT evaluated.',
                      'Diagnostic coverage: `'+json.dumps(report.get('coverage',{}))+'`.',
                      'Achieved windows: `'+json.dumps(report.get('achieved_windows',{}))+'`.',
                      'A diagnostic pass does not establish adequate SQL window, power or gate readiness.'])
    for case in report['cases']:
        for phase in ('initial','confirmation'):
            if phase in case and 'n' in case[phase]:
                b = case[phase]
                lines.append(f"| {case['name']} | {phase} | {b['n']} | {b['ratio']:.3f} | [{b['lower']:.3f}, {b['upper']:.3f}] | {b['baseline_median']:.4f} {b['baseline_range']} | {b['candidate_median']:.4f} {b['candidate_range']} | {case['status']} |")
    lines.extend(['', '| Case / workload | Phase | Complete | Timing | Combined | Seed / first ID / N |',
                  '|---|---|---|---|---|---|'])
    for case in report['cases']:
        identity = case.get('identity',{})
        details = f"{identity.get('seed_count','-')} / {identity.get('first_id','-')} / {case.get('operations','-')}"
        lines.append(f"| {case['name']} / {case['workload_version']} | {case['phase']} | {case['complete']} | {case['timing_status']} | {case['status']} | {details} |")
        if 'control_budget' in case:
            lines.extend(['', 'Mutation control budget: `'+json.dumps(case['control_budget'])+'`.', ''])
    lines.extend(['', 'Required size states: `'+json.dumps(report.get('required_size_states',[]))+'`.'])
    lines.extend(['', '| Size state | Relation / role | Base bytes/pages | Head bytes/pages | Ratio | Limit bytes | Status |', '|---|---|---|---|---:|---:|---|'])
    for snapshot in normal_size_checks(report) + ([report['size_control']] if 'size_control' in report else []):
        for r in snapshot['relations']:
            lines.append(f"| {snapshot['state']} | {r['relation']} / {r['role']} | {r['baseline_bytes']} / {r['baseline_pages']} | {r['candidate_bytes']} / {r['candidate_pages']} | {r['ratio']} | {r['threshold_bytes']} | {r['status']} |")
    lines.extend(['', 'Phase runtime (seconds): `'+json.dumps(report['runtime_seconds'])+'`.',
                  'Configuration: `'+json.dumps(report['config'])+'`.',
                  'Budget envelope: `'+json.dumps(report.get('budget',{}))+'`. Cleanup/report reserve is an allowance, not a hard deadline.',
                  'Fixture checkpoint barrier: `'+json.dumps(report.get('fixture_checkpoint_barrier',{}))+'`.',
                  'This is a small warm serial workload on the recorded host; no hardware-counter or release throughput claim.'])
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
    p.add_argument('--insert-feasibility',action='store_true',help='experimental equal-source INSERT PR-only diagnostic; always fixed6+6, no controls/normal gate')
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


def main(argv=None):
    global DEADLINE
    args = parser().parse_args(argv)
    groups_supplied = args.groups is not None
    args.groups = args.groups or ''
    args.output = args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    report = dict(schema_version=3,complete=False,size_complete=False,budget_exhausted=False,date=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  baseline_ref=args.baseline_ref,candidate_ref=args.candidate_ref,status='inconclusive',cases=[],size_checks=[],size_status='inconclusive',
                  cpu=next((l.split(':',1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines() if l.startswith('model name')),platform.machine()),
                  cpus=os.cpu_count(),runner=os.environ.get('RUNNER_NAME',platform.node()),load=os.getloadavg(),
                  config=dict(tier=args.tier,groups=args.groups,insert_feasibility=args.insert_feasibility,initial_pairs=INITIAL_PAIRS,
                              read_confirmation_pairs=READ_CONFIRMATION_PAIRS,mutation_confirmation_pairs=MUTATION_CONFIRMATION_PAIRS,
                              supported_pairs=list(T95),t_critical_by_pairs=T95,target_seconds=TARGET,threshold=MATERIAL,
                              insert_pilot_operations=INSERT_PILOT,insert_growth_cap_operations=INSERT_GROWTH_CAP,
                              insert_control_delay_seconds=INSERT_CONTROL_DELAY,budget_seconds=args.budget_seconds,verify_control=args.verify_control,build_workers=2,
                              build_flags='USE_PGXS=1 SIFT_TESTING= SIFT_LTO= SANITIZE= VULKAN=',settings=SETTINGS),runtime_seconds={})
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
                                        initial_pairs=6,confirmation_pairs=6,expected_insert_pairs=14,
                                        expected_restoration_checkpoints=28,expected_resets=56))
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
        report['postgresql'] = command([args.pg_config,'--version'])
        if not report['postgresql'].startswith('PostgreSQL 18.'):
            raise ValueError('performance gate requires PostgreSQL 18')
        report['compiler'] = command(['cc','--version']).splitlines()[0]
        report['pg_config'] = command([args.pg_config,'--configure'])
        if args.insert_feasibility:
            files = ('bench/steps/perf_gate.py','bench/perf/catalog.py','bench/perf/fixture.sql')
            blobs = {}
            for name in files:
                blobs[name] = command(['git','rev-parse',args.candidate_ref+':'+name],cwd=ROOT)
                require_equal(command(['git','hash-object',name],cwd=ROOT),blobs[name],'committed feasibility harness '+name)
            report['source_identity'] = dict(commit=args.candidate_ref,tree=command(['git','rev-parse',args.candidate_ref+'^{tree}'],cwd=ROOT),
                                             harness_blobs=blobs,harness_sha256={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in files},
                                             fixture_owner='candidate harness, loaded identically into both isolated exact-SHA builds')
            write_report(args.output,report)
        args.scratch_root.mkdir(parents=True,exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix='perf-gate-',dir=args.scratch_root))
        for i, (name,sha,tree) in enumerate([('baseline',args.baseline_ref,args.baseline_tree),('candidate',args.candidate_ref,args.candidate_tree)]):
            phase = time.monotonic()
            rev = Revision(name,sha,tree,scratch,args.output,str(Path(args.pg_config).resolve()),args.port_base+i)
            revisions.append(rev)
            report.setdefault('cluster_config',{})[name] = rev.setup(32768 if args.tier=='nightly' else 8192)
            report.setdefault('setup_phases',{})[name] = rev.timings
            report.setdefault('versions',{})[name] = rev.versions
            report['runtime_seconds'][name+'_build_cluster_fixture'] = time.monotonic()-phase
        report['fixture_checkpoint_barrier'] = {}
        finalize_fixtures(revisions,DEADLINE,report['fixture_checkpoint_barrier'])
        report['runtime_seconds']['fixture_checkpoint_barrier'] = report['fixture_checkpoint_barrier']['runtime_seconds']
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
        if not args.insert_feasibility:
            report['initial_sizes'] = {r.name:sizes(r.conn) for r in revisions}
            report['size_checks'].append(size_snapshot(revisions,'settled_build'))
        phase = time.monotonic()
        measurement_start = phase
        deadline = DEADLINE
        if args.insert_feasibility:
            insert = next(c for c in CASES if c.lifecycle=='insert_trajectory')
            write_report(args.output,report)
            measure(revisions,insert,deadline,sink=report['cases'],force_confirmation=True,report=report,output=args.output)
        elif args.verify_control:
            report['post_control_costs'] = {name:dict(state='NOT_MEASURED',complete=False,seconds=None,
                                                     provenance='required after control in this invocation; no measurement available before admission')
                                            for name in POST_CONTROL_PHASES}
            report['acceptance_catalog_validation'] = {}
            for catalog_case in CASES:
                if catalog_case.lifecycle=='insert_trajectory':
                    continue
                validation = dict(complete=False)
                report['acceptance_catalog_validation'][catalog_case.name] = validation
                validation.update(validate_case(revisions,catalog_case))
                validation['complete'] = True
            case = next(c for c in CASES if c.name=='storage_point_lookup')
            unchanged = measure(revisions,case,deadline,sink=report['cases'])
            unchanged['name'] = 'control_unchanged'
            slowed = measure(revisions,case,deadline,True,sink=report['cases'])
            slowed['name'] = 'control_sql_slowdown'
            insert = next(c for c in CASES if c.lifecycle=='insert_trajectory')
            unchanged_insert = measure(revisions,insert,deadline,sink=report['cases'])
            unchanged_insert['name'] = 'control_insert_unchanged'
            report['acceptance_catalog_validation'][insert.name] = unchanged_insert['validation']
            slowed_insert = measure(revisions,insert,deadline,True,sink=report['cases'],frozen=unchanged_insert,
                                    report=report,output=args.output)
            slowed_insert['name'] = 'control_insert_per_row_slowdown'
            report['size_checks'].append(churn_sizes(revisions))
            before = revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs")
            # Explicit acceptance-only SQL fault: force many small BM25 build segments.
            revisions[1].conn.script('ALTER INDEX docs_bm25 SET (build_segments=64); REINDEX INDEX docs_bm25')
            require_equal(before,revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs"),'size control logical rows')
            report['size_control'] = size_snapshot(revisions,'control_build_segments_64')
            report['acceptance_pass'] = (unchanged['status']=='pass' and slowed['timing_status']=='regression'
                                         and unchanged_insert['status']=='pass' and slowed_insert['timing_status']=='regression'
                                         and slowed_insert['confirmation']['n']==MUTATION_CONFIRMATION_PAIRS
                                         and slowed_insert['confirmation']['lower']>MATERIAL
                                         and all(s['status']=='pass' for s in normal_size_checks(report))
                                         and report['size_control']['status']=='regression')
        else:
            for case in selected:
                measure(revisions,case,deadline,sink=report['cases'])
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
        for rev in reversed(revisions):
            try:
                rev.stop(args.output)
            except Exception as exc:
                report['status'],report['error'] = 'error',f'cleanup failed: {exc}'
        if scratch and not any(r.started for r in revisions):
            shutil.rmtree(scratch)
        report['runtime_seconds']['cleanup'] = time.monotonic()-phase
        report['runtime_seconds']['total'] = time.monotonic()-start
        if args.insert_feasibility:
            reduce_insert_feasibility(report)
        else:
            reduce_report(report)
        write_report(args.output,report)
    print(f"{report['status']}: {args.output/'summary.md'}")
    return {'pass':0,'regression':1,'inconclusive':2,'error':3,
            'diagnostic_pass':0,'diagnostic_regression':1,'diagnostic_inconclusive':2,'diagnostic_error':3}[report['status']]


if __name__=='__main__':
    sys.exit(main())
