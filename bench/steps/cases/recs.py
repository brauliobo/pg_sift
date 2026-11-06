# The records of a cases run (steps/records): each query case measured again on the arms the case can answer with, by the shared collector, for `bench cases --records FILE`.
#
# The arms of a case are forced on its own table: heap (a sequential scan), sift (the access method alone), a b-tree twin when the case has a `btree` variant (its schema in the
# search path), every other variant as a sift arm with its name as the option, and, for a case with a native lane, the arm `native`: the normal plan (sift.enable_agg off), whatever
# structure it names, never the picked one. The plan the planner chooses with nothing forced is the default row. A case whose backend is
# fresh for every run (-- fresh: yes) runs each repetition in a new backend (collect's `fresh`). A load case has no plan to pick: its lanes (the sift table with its index, the heap
# with the b-tree or GIN twin, each variant) are forced arms, each repetition prepares the table again and the statements are timed and added. A case that gives no records is
# listed with its reason (SKIPPED) and fails the run.

import os
import re
import subprocess
import time

import iolib

from . import runcfg
from .measure import expand
from .runcfg import REPO
from .server import Fresh, server_session

import records.collect as collect  # noqa: E402
import records.schema as schema  # noqa: E402

ARM_BUDGET = None   # seconds one arm may take (--arm-budget): past it the arm is unavailable, with the reason
ROWS = []
RUN = {}
SKIPPED = []
UNAVAILABLE = []


def tip():
    return subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'


def table_state(case, sections=('setup', 'prepare')):
    """fresh or churned (an UPDATE or DELETE in the setup), vacuumed or not (a VACUUM after the last of them, and not `-- vacuum: no`)"""
    text = '\n'.join(case.sec.get(k, '') for k in sections)
    dml = [m.end() for m in re.finditer(r'\b(UPDATE|DELETE)\b', text)]
    vac = [m.end() for m in re.finditer(r'\bVACUUM\b', text)]
    vacuumed = bool(vac) and case.opt.get('vacuum') != 'no' and (not dml or max(vac) > max(dml))
    return ('churned_' if dml else 'fresh_') + ('vacuumed' if vacuumed else 'unvacuumed')


def first_table(session, sql):
    """the subject of SQL: the largest relation of its FROM and JOIN clauses that the session can resolve (the probe table of a lateral join is not what the arms compare)"""
    best, size = None, -1
    for name in dict.fromkeys(re.findall(r'\b(?:FROM|JOIN)\s+([a-z_][a-z0-9_.]*)', sql, re.I)):
        got = collect.one(session, "SELECT coalesce(pg_total_relation_size(to_regclass('%s')), -1)" % name)
        if got and int(got[0]) > size:
            best, size = name, int(got[0])
    return best if size >= 0 else None


def arms(case):
    out = [collect.Arm('heap'), collect.Arm('sift')]
    for name, sets in case.variants:
        sets = ['SET %s' % s for s in sets.split(';')]
        if name == 'btree':
            out.append(collect.Arm('btree', base=sets))
        elif name != runcfg.FORCED[0]:
            out.append(collect.Arm('sift', option='default_variant' if name == 'default' else name, base=sets, keep=name.startswith('warm')))   # `default` is the table with the default options beside the case's own index
    return out


def native_arm(case, sql):
    """the arm of the native lane, the normal plan (`sift.enable_agg = off`, and the column scan off when the case sets it), measured like every other arm: its own statement when the case has one
    (`--@ pg` beside `--@ sift`: a GIN match against the BM25 operator, a plain query against the rewritten one), else the statement of the sift lane. Its structure is whatever the plan names
    and it is never the picked arm (collect.NATIVE); None for a case that has no native lane"""
    if case.baselines[:1] != ['native']:
        return None
    native = expand(case.statement('native'), case)
    settings = ['SET sift.enable_agg = off'] + (['SET sift.enable_col_scan = off'] if 'sift.enable_col_scan' in case.opt.get('set', '') else [])
    return collect.Arm(collect.NATIVE, settings=settings, sql=None if native.strip() == sql.strip() else native)


def run_env(session, runs):
    if not RUN:
        pid = collect.backend_pid(session)
        RUN.update(tip=tip(), run=schema.run_id(tip()), cpus=collect.host_cpus(collect.postmaster_pid(pid)))
    # the cluster of the cases is a test cluster (scripts/pgtest.sh: no autovacuum, no automatic merge, so a state is the one the setup made): the records say so, and the report shows it
    off = 'disabled' if schema.maintenance_switches(collect.settings_of(session)) else 'default'
    return collect.Env(RUN['tip'], 'bench cases', RUN['run'], RUN['cpus'], reps=max(3, runs), cool=iolib.cool, maintenance=off, arm_budget=ARM_BUDGET)


def emit(case, lanes, rows, runs):
    """append the records of CASE to ROWS: its sift lane's session measures the default plan and each arm; a fresh lane measures each repetition in a new backend"""
    lane = lanes['sift']
    fresh = None
    if isinstance(lane, Fresh):
        session = lane.persistent()
        fresh = lambda arm: with_settings(lane.persistent(), arm)   # noqa: E731
    else:
        session = lane
    try:
        sql = expand(case.statement('sift'), case)
        table = first_table(session, sql)
        if table is None:
            SKIPPED.append((case.id, 'its statement reads no table of the schema, so there is no structure to compare'))
            return
        env = run_env(session, runs)
        subject = collect.Subject(case.dataset, rows, 'other', table_state(case), table, protocol='fresh backend' if fresh else 'single')
        missed = []
        forced = arms(case)
        extra = native_arm(case, sql)
        try:
            ROWS.extend(collect.measure_statement(session, case.id, sql, subject, forced + ([extra] if extra else []), env, fresh=fresh, unavailable=missed))
        except (collect.PerfUnavailable, collect.ArmTimeout) as e:
            SKIPPED.append((case.id, str(e)))
            return
        UNAVAILABLE.extend((case.id,) + m for m in missed)
    finally:
        if fresh:
            session.close()


def with_settings(session, arm):
    for st in arm.settings:
        session.run(st)
    return session


def split(text):
    return [x for x in re.split(r';[ \t]*\n', text) if x.strip()]


def native_structure(case):
    """the structure the native lane of a load builds: GIN for a text index, a b-tree for any other index, else the heap alone"""
    text = '\n'.join(case.sec.get(k, '') for k in ('prepare', 'load'))
    return 'gin' if re.search(r'USING gin\b', text, re.I) else 'btree' if re.search(r'\bCREATE\s+(UNIQUE\s+)?INDEX\b|PRIMARY KEY', text, re.I) else 'heap'


def quiet(session, limit=60):
    """wait until no other backend of the database is working (a maintenance worker of the table just measured): DROP SCHEMA of the next preparation takes the locks of the table in another order
    than a worker does and the two deadlock"""
    for _ in range(limit * 5):
        if collect.one(session, "SELECT count(*) FROM pg_stat_activity WHERE datname = current_database() AND pid <> pg_backend_pid() AND state = 'active'")[0] == '0':
            return
        time.sleep(0.2)


def emit_load(case, csv, rows, runs, build, workers):
    """append the records of a load CASE: a session keeps the schema of the case, each repetition of an arm builds it empty again (`build`), prepares the table (untimed) and times the
    statements of the load; a second session counts the parallel workers. The lanes are the sift table (or the sift index) and the heap with its twin, and each variant."""
    session, monitor = server_session(600), server_session(60)
    try:
        for st in ('SET search_path = c_%s, public' % case.id, 'SET max_parallel_maintenance_workers = %s' % workers):
            session.run(st)
        env = run_env(session, runs)

        def preparer(am):
            def prepare():
                quiet(session)
                build(case, rows)
                for stmt in split(case.sec['prepare'].replace('{am}', am).replace('{csv}', csv).replace('{rows}', str(rows))):
                    err = session.run(stmt)[2]
                    if err:
                        raise RuntimeError('%s: the preparation failed: %s' % (case.id, err))
                session.run('CHECKPOINT')
            return prepare

        def text(am):
            return split(case.sec.get('load_sift', case.sec['load']) if am == 'sift_table' else case.sec['load'])

        def fill(statements):
            return [x.replace('{csv}', csv).replace('{rows}', str(rows)) for x in statements]
        build_case = case.opt.get('kind') == 'build'   # a build is the sift index alone: no twin lane, and the leader, the workers and the segments are recorded
        declared = int(case.opt['segments']) if 'segments' in case.opt else None
        lanes = ([] if build_case else [(native_structure(case), 'default', 'heap', [])]) + [('sift', 'default', 'sift_table', [])] + [('sift', name, 'sift_table', ['SET %s' % x for x in sets.split(';')]) for name, sets in case.variants]
        keyed = re.search(r'PRIMARY KEY|UNIQUE', '\n'.join(case.sec.get(k, '') for k in ('prepare', 'load', 'load_sift')))   # a key or unique constraint is the table's definition in every lane: its index is listed as `constraint`
        arms = [collect.Arm(arm, option=option, settings=sets, sql=fill(text(am)), prepare=preparer(am), structures=','.join(sorted({'heap', arm} | ({'constraint'} if keyed else set()))), segments=declared if arm == 'sift' else None) for arm, option, am, sets in lanes]
        state = table_state(case, ('prepare', 'load'))
        subject = collect.Subject(case.dataset, rows, 'other', state, case.schema(), kind='build' if build_case else 'load', protocol='single')
        hash_text = '\n'.join(case.sec[k] for k in ('prepare', 'load', 'load_sift') if k in case.sec)
        try:
            ROWS.extend(collect.measure_statement(session, case.id, fill(text('heap')), subject, arms, env, monitor=monitor, temps=('warm',), hash_text=hash_text))
        except (collect.PerfUnavailable, collect.ArmTimeout) as e:
            SKIPPED.append((case.id, str(e)))
    finally:
        session.close()
        monitor.close()


def flush(path):
    """the records of the cases measured so far, written whole beside PATH and moved over it: a run that stops, or a case that never ends, keeps what the earlier cases gave"""
    schema.write(path + '.part', ROWS)
    os.replace(path + '.part', path)


def write(path):
    """the records file, then the checker's verdict on it (printed; the cases' own results are unaffected); a case that gave no records is a problem as well"""
    from records import check
    schema.write(path, ROWS)
    problems = check.check(schema.read(path)) if ROWS else []
    for p in problems:
        print('RECORDS ' + p)
    for case, why in SKIPPED:
        print('RECORDS NO RECORDS FOR CASE %s: %s' % (case, why))
    for case, arm, option, named in UNAVAILABLE:
        if named.startswith('timeout: '):
            print('RECORDS ARM TIMED OUT %s: %s %s is unavailable, %s; the default row and the other arms stand' % (case, arm, option, named[len('timeout: '):]))
        else:
            print('RECORDS ARM NOT FORCEABLE %s: %s %s is not a plan of its statement (the plans name %s); the default row and the other arms stand' % (case, arm, option, named))
    print('records: %d rows written to %s, %d problems, %d cases without records' % (len(ROWS), path, len(problems), len(SKIPPED)))
    return not problems and not SKIPPED
