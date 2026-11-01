# The records of a cases run (steps/records): each query case measured again on the arms the case can answer with, by the shared collector, for `bench cases --records FILE`.
#
# The arms of a case are forced on its own table: heap (a sequential scan), sift (the access method alone), a b-tree twin when the case has a `btree` variant (its schema in the
# search path), and every other variant as a sift arm with its name as the option. The plan the planner chooses with nothing forced is the default row. Load cases and the cases
# whose backend is fresh for every run (-- fresh: yes) have no records yet: their lanes are not a session the collector can keep.

import os
import re
import subprocess

import iolib

from . import runcfg
from .measure import expand
from .runcfg import REPO

import records.collect as collect  # noqa: E402
import records.schema as schema  # noqa: E402

ROWS = []
RUN = {}


def tip():
    return subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'


def table_state(case):
    """fresh or churned (an UPDATE or DELETE in the setup), vacuumed or not (a VACUUM after the last of them, and not `-- vacuum: no`)"""
    text = '\n'.join(case.sec.get(k, '') for k in ('setup', 'prepare'))
    dml = [m.end() for m in re.finditer(r'\b(UPDATE|DELETE)\b', text)]
    vac = [m.end() for m in re.finditer(r'\bVACUUM\b', text)]
    vacuumed = bool(vac) and case.opt.get('vacuum') != 'no' and (not dml or max(vac) > max(dml))
    return ('churned_' if dml else 'fresh_') + ('vacuumed' if vacuumed else 'unvacuumed')


def first_table(session, sql):
    """the first relation of a FROM clause of SQL that the session can resolve"""
    for name in re.findall(r'\b(?:FROM|JOIN)\s+([a-z_][a-z0-9_.]*)', sql, re.I):
        if collect.one(session, "SELECT coalesce(to_regclass('%s')::text, '')" % name)[0] != '':
            return name
    return None


def arms(case):
    out = [collect.Arm('heap'), collect.Arm('sift')]
    for name, sets in case.variants:
        sets = ['SET %s' % s for s in sets.split(';')]
        if name == 'btree':
            out.append(collect.Arm('btree', settings=sets + collect.FORCING['btree']))
        elif name != runcfg.FORCED[0]:
            out.append(collect.Arm('sift', option=name, settings=sets + collect.FORCING['sift']))
    return out


def emit(case, lanes, rows, runs):
    """append the records of CASE to ROWS: its sift lane's session measures the default plan and each arm"""
    session = lanes['sift']
    if not hasattr(session, 'run'):
        return
    sql = expand(case.statement('sift'), case)
    table = first_table(session, sql)
    if table is None:
        return
    if not RUN:
        pid = collect.backend_pid(session)
        RUN.update(tip=tip(), run=schema.run_id(tip()), cpus=collect.host_cpus(collect.postmaster_pid(pid)))
    # the cluster of the cases is a test cluster (scripts/pgtest.sh: no autovacuum, no automatic merge, so a state is the one the setup made): the records say so, and the report shows it
    off = 'disabled' if schema.maintenance_switches(collect.settings_of(session)) else 'default'
    env = collect.Env(RUN['tip'], 'bench cases', RUN['run'], RUN['cpus'], reps=max(3, runs), cool=iolib.cool, maintenance=off)
    subject = collect.Subject(case.dataset, rows, 'other', table_state(case), table, protocol='single')
    ROWS.extend(collect.measure_statement(session, case.id, sql, subject, arms(case), env))


def write(path):
    """the records file, then the checker's verdict on it (printed; the cases' own results are unaffected)"""
    from records import check
    schema.write(path, ROWS)
    problems = check.check(schema.read(path)) if ROWS else []
    for p in problems:
        print('RECORDS ' + p)
    print('records: %d rows written to %s, %d problems' % (len(ROWS), path, len(problems)))
    return not problems
