#!/usr/bin/env python3
"""Exact-revision PostgreSQL 18 paired elapsed-time performance gate."""
import argparse
import ctypes
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
BATCH_WALL_LIMIT = 1.0
SETTINGS = '''SET statement_timeout='30s'; SET max_parallel_workers_per_gather=0;
SET max_parallel_maintenance_workers=0; SET jit=off; SET sift.async_maintenance=off;
SET sift.table_write_buffer_limit=2048; SET cpu_tuple_cost=0.5; SET enable_seqscan=off; SET sift.enable_agg=on; SET sift.agg_preagg=on'''


class BudgetExpired(Exception):
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


def sizes(conn):
    return conn.rows("SELECT c.relname,pg_relation_size(c.oid,'main')+pg_relation_size(c.oid,'fsm')+pg_relation_size(c.oid,'vm')+pg_relation_size(c.oid,'init') FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relkind IN ('r','i') ORDER BY 1")


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
    return compare_sizes([dict(sizes(r.conn)) for r in revisions],state,SIZE_RELATIONS)


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


def prepare(conn, case):
    conn.script('SET sift.agg_preagg=on; SET sift.enable_agg=on')
    if case.settings:
        conn.script(case.settings)
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
    checks, plans, footprints = [], [], []
    for rev in revisions:
        conn = rev.conn
        prepare(conn, case)
        if case.reset:
            before = sizes(conn)
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
            oracle = {'storage_insert': 'SELECT count(*)+1,sum(id)+9001,sum(amount)+7 FROM seed',
                      'storage_update': 'SELECT count(*),sum(id),sum(amount)+count(amount) FILTER (WHERE id BETWEEN 100 AND 200) FROM seed',
                      'storage_vacuum_space': 'SELECT count(*),sum(id),sum(amount) FROM seed WHERE id<=2048'}[case.name]
            require_equal(got, conn.rows(oracle), case.name+' heap oracle')
            footprints.append(dict(before=before,after=sizes(conn),before_geometry=before_geometry,after_geometry=table_geometry(conn,'mutable')))
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
                                        'mutable_vacuum',{'mutable':'mutable_table_storage','mutable_pk':'mutable_btree_index'})
    return dict(results=checks,plans=plans,plan_changed=plan_changed,
                topology=[topology(p) for p in plans],sizes=footprints,size_checks=lifecycle_sizes)


def elapsed(conn, case, operations, deadline, delay=0):
    total = 0.0
    for _ in range(operations):
        if time.monotonic() >= deadline:
            raise BudgetExpired('measurement budget exhausted')
        reset(conn, case)
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after reset')
        start = time.perf_counter()
        conn.exec(case.sql)
        value = time.perf_counter()-start
        if not math.isfinite(value) or value <= 0:
            raise ValueError('invalid monotonic SQL elapsed time')
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after SQL')
        total += value
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


def block(revisions, case, operations, deadline, control=False, sink=None, *, pair_count=INITIAL_PAIRS):
    if pair_count not in T95:
        raise ValueError('unsupported independent pair count')
    start = time.monotonic()
    pairs = []
    saved = sink if sink is not None else {}
    saved.update(samples=pairs,expected_pairs=pair_count,started_at=utc_now())
    try:
        for round_no in range(pair_count):
            pair = [None,None]
            pairs.append(pair)
            order = [0,1] if round_no%2 == 0 else [1,0]
            for side in order:
                conn = revisions[side].conn
                prepare(conn,case)
                elapsed(conn,case,1,deadline)  # independent warmed batch, excluded
                pair[side] = elapsed(conn,case,operations,deadline,0.075 if control and side else 0)
        saved.update(interval(pairs))
    finally:
        saved.update(finished_at=utc_now(),runtime_seconds=time.monotonic()-start)
    return saved


def measure(revisions, case, deadline, control=False, sink=None):
    start = time.monotonic()
    result = dict(name=case.name,group=case.group,smoke=case.smoke,status='inconclusive',started_at=utc_now(),
                  validation=validate_case(revisions,case))
    result['phase_runtime_seconds'] = {'validation':time.monotonic()-start}
    if sink is not None:
        sink.append(result)
    calibration_start = time.monotonic()
    calibration = calibrate(revisions,case,deadline)
    result['phase_runtime_seconds']['calibration'] = time.monotonic()-calibration_start
    result['calibration'] = calibration
    operations = calibration['operations']
    result['operations'] = operations
    result['initial'] = {}
    result['initial'].update(block(revisions,case,operations,deadline,control,sink=result['initial']))
    result['status'] = verdict(result['initial'])
    if result['status'] == 'suspect':
        result['status'] = 'inconclusive'
        result['confirmation'] = {}
        pair_count = MUTATION_CONFIRMATION_PAIRS if case.reset else READ_CONFIRMATION_PAIRS
        result['confirmation'].update(block(revisions,case,operations,deadline,control,
                                            sink=result['confirmation'],pair_count=pair_count))
        result['status'] = verdict(result['confirmation'],True)
    if result['validation']['size_checks'] is not None:
        result['space_status'] = result['validation']['size_checks']['status']
        if result['space_status']=='regression':
            result['status'] = 'regression'
    result['runtime_seconds'] = time.monotonic()-start
    return result


def write_report(output, report):
    (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    lines = ['# Focused performance gate', '', f"Status: **{report['status']}**. {report['date']}.",
             f"Normal size gate: **{report['size_status']}**.",
             f"Baseline: `{report['baseline_ref']}`. Candidate: `{report['candidate_ref']}`.",
             f"PostgreSQL: {report.get('postgresql','unknown')}. CPU: {report['cpu']}; CPUs: {report['cpus']}.",
             f"Runner: {report['runner']}. Load: {report['load']}.",
             'Metric: monotonic elapsed persistent SQL end-to-end, including planning and result receipt.',
             'Six initial alternating warmed pairs; one fresh suspect-only confirmation: 16 read pairs or 6 mutation pairs. Fixed common operations; 95% Student-t log-ratio CI; material ratio 1.10.',
             '', '| Case | Phase | n | Ratio | 95% CI | Base median [min,max] s | Head median [min,max] s | Status |',
             '|---|---|---:|---:|---|---|---|---|']
    for case in report['cases']:
        for phase in ('initial','confirmation'):
            if phase in case and 'n' in case[phase]:
                b = case[phase]
                lines.append(f"| {case['name']} | {phase} | {b['n']} | {b['ratio']:.3f} | [{b['lower']:.3f}, {b['upper']:.3f}] | {b['baseline_median']:.4f} {b['baseline_range']} | {b['candidate_median']:.4f} {b['candidate_range']} | {case['status']} |")
    lines.extend(['', '| Size state | Relation / role | Base bytes/pages | Head bytes/pages | Ratio | Limit bytes | Status |', '|---|---|---|---|---:|---:|---|'])
    for snapshot in report['size_checks'] + [c['validation']['size_checks'] for c in report['cases'] if c.get('validation',{}).get('size_checks')] + ([report['size_control']] if 'size_control' in report else []):
        for r in snapshot['relations']:
            lines.append(f"| {snapshot['state']} | {r['relation']} / {r['role']} | {r['baseline_bytes']} / {r['baseline_pages']} | {r['candidate_bytes']} / {r['candidate_pages']} | {r['ratio']} | {r['threshold_bytes']} | {r['status']} |")
    lines.extend(['', 'Phase runtime (seconds): `'+json.dumps(report['runtime_seconds'])+'`.',
                  'Configuration: `'+json.dumps(report['config'])+'`.',
                  'Budget envelope: `'+json.dumps(report.get('budget',{}))+'`. Cleanup/report reserve is an allowance, not a hard deadline.',
                  'Fixture checkpoint barrier: `'+json.dumps(report.get('fixture_checkpoint_barrier',{}))+'`.',
                  'This is a small warm serial workload on the recorded host; no hardware-counter or release throughput claim.'])
    if 'error' in report:
        lines.extend(['', 'Error: '+report['error']])
    (output/'summary.md').write_text('\n'.join(lines)+'\n')


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline-ref',required=True)
    p.add_argument('--candidate-ref',required=True)
    p.add_argument('--baseline-tree')
    p.add_argument('--candidate-tree')
    p.add_argument('--groups',default='')
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
    args.output = args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    report = dict(schema_version=1,date=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  baseline_ref=args.baseline_ref,candidate_ref=args.candidate_ref,status='inconclusive',cases=[],size_checks=[],size_status='inconclusive',
                  cpu=next((l.split(':',1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines() if l.startswith('model name')),platform.machine()),
                  cpus=os.cpu_count(),runner=os.environ.get('RUNNER_NAME',platform.node()),load=os.getloadavg(),
                  config=dict(tier=args.tier,groups=args.groups,initial_pairs=INITIAL_PAIRS,
                              read_confirmation_pairs=READ_CONFIRMATION_PAIRS,mutation_confirmation_pairs=MUTATION_CONFIRMATION_PAIRS,
                              supported_pairs=list(T95),t_critical_by_pairs=T95,target_seconds=TARGET,threshold=MATERIAL,
                              budget_seconds=args.budget_seconds,verify_control=args.verify_control,build_workers=2,
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
        exact_sha(args.baseline_ref)
        exact_sha(args.candidate_ref)
        if args.verify_control:
            require_equal(args.baseline_ref,args.candidate_ref,'control requires unchanged revisions')
        if not 56100 <= args.port_base <= 56298:
            raise ValueError('port-base must be in 56100..56298')
        selected = select([g for g in args.groups.split(',') if g],args.tier)
        report['postgresql'] = command([args.pg_config,'--version'])
        if not report['postgresql'].startswith('PostgreSQL 18.'):
            raise ValueError('performance gate requires PostgreSQL 18')
        report['compiler'] = command(['cc','--version']).splitlines()[0]
        report['pg_config'] = command([args.pg_config,'--configure'])
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
        report['initial_sizes'] = {r.name:sizes(r.conn) for r in revisions}
        report['size_checks'].append(size_snapshot(revisions,'settled_build'))
        phase = time.monotonic()
        measurement_start = phase
        deadline = DEADLINE
        if args.verify_control:
            report['acceptance_catalog_validation'] = {c.name:validate_case(revisions,c) for c in CASES}
            case = next(c for c in CASES if c.name=='storage_point_lookup')
            unchanged = measure(revisions,case,deadline,sink=report['cases'])
            unchanged['name'] = 'control_unchanged'
            slowed = measure(revisions,case,deadline,True,sink=report['cases'])
            slowed['name'] = 'control_sql_slowdown'
            report['size_checks'].append(churn_sizes(revisions))
            before = revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs")
            # Explicit acceptance-only SQL fault: force many small BM25 build segments.
            revisions[1].conn.script('ALTER INDEX docs_bm25 SET (build_segments=64); REINDEX INDEX docs_bm25')
            require_equal(before,revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs"),'size control logical rows')
            report['size_control'] = size_snapshot(revisions,'control_build_segments_64')
            report['status'] = 'pass' if unchanged['status']=='pass' and slowed['status']=='regression' and all(s['status']=='pass' for s in report['size_checks']) and report['size_control']['status']=='regression' else 'inconclusive'
        else:
            for case in selected:
                measure(revisions,case,deadline,sink=report['cases'])
                write_report(args.output,report)
            report['size_checks'].append(churn_sizes(revisions))
            statuses = {c['status'] for c in report['cases']} | {s['status'] for s in report['size_checks']}
            report['status'] = 'regression' if 'regression' in statuses else 'inconclusive' if statuses != {'pass'} else 'pass'
        report['size_status'] = 'regression' if any(s['status']=='regression' for s in report['size_checks']) or any(c.get('space_status')=='regression' for c in report['cases']) else 'pass'
        if time.monotonic()>=deadline:
            report['size_status'] = 'inconclusive'
            raise BudgetExpired('global runtime budget exhausted after final size checks')
        report['runtime_seconds']['measurement_validation'] = time.monotonic()-phase
    except BudgetExpired as exc:
        report['status'],report['error'] = 'inconclusive',str(exc)
        if report['cases']:
            report['cases'][-1]['status'] = 'inconclusive'
    except Exception as exc:
        report['status'],report['error'] = 'error',f'{type(exc).__name__}: {exc}'
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
        write_report(args.output,report)
    print(f"{report['status']}: {args.output/'summary.md'}")
    return {'pass':0,'regression':1,'inconclusive':2,'error':3}[report['status']]


if __name__=='__main__':
    sys.exit(main())
