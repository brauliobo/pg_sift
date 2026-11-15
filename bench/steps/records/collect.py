"""Measuring one statement on its arms and writing records (schema.py).

    rows = collect.measure_statement(session, 'top20', sql, subject, arms, env)        a list of records, ready for schema.write

SESSION is any object whose run(sql) returns (client ms, rows, error, ...) over one connection kept open (steps/duckdb.py Session, steps/cases/server.py server_session). The harness
describes what it measures (Subject: the dataset, its order and state), how each arm is forced (Arm) and the server it measures on (Env, from `server_env`); this module does the
rest the same way for everyone: the default plan first, then each arm with its settings, warm and cold, each repetition apart:

  structure   read from the EXPLAIN (ANALYZE) of the run: the index names of its scan nodes are matched with their access methods, a custom scan of sift is sift
  time        wall: the client's timing of the statement; plan_ms and exec_ms: EXPLAIN (ANALYZE, SUMMARY) of the same statement, run apart
  instr       perf stat on the backend and the postmaster (the workers of a plan are its children and are counted), scope `cluster`
  blocks      the difference of pg_statio_all_tables / pg_statio_all_indexes around the run, by access method; the server flushes its statistics first
  cold        the relations of the table evicted from shared buffers (iolib.cool); os_cache_dropped says whether the OS cache went too
  host        CPU and IO pressure at the start and end, the cpu quota of the server, its settings

Reads are repeated in place; a maintenance statement or a load (kind != read) changes the table, so the harness gives `prepare`, run before each repetition and not timed, and
`monitor`, a second session that counts the parallel workers while the statement runs.
"""
import contextlib
import hashlib
import json
import math
import os
import re
import signal
import subprocess
import sys
import threading
import time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import check  # noqa: E402
import schema  # noqa: E402

PERF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'perf.sh')
EXPLAIN = 'EXPLAIN (ANALYZE, BUFFERS, SUMMARY, COSTS, FORMAT JSON) '
FORCING = {
    'heap': ['SET enable_indexscan = off', 'SET enable_indexonlyscan = off', 'SET enable_bitmapscan = off', 'SET sift.enable_agg = off', 'SET sift.enable_col_scan = off'],
    'btree': ['SET enable_seqscan = off', 'SET sift.enable_agg = off', 'SET sift.enable_col_scan = off'],
    'gin': ['SET enable_seqscan = off', 'SET sift.enable_agg = off', 'SET sift.enable_col_scan = off'],
    'sift': ['SET enable_seqscan = off'],
}   # the settings that make the planner use a structure when the table holds more than one; a harness with a table for each structure gives its own (Arm.settings) and a search_path
TRIES = {'sift': [[], FORCING['sift'], FORCING['sift'] + ['SET enable_indexscan = off', 'SET enable_indexonlyscan = off', 'SET enable_bitmapscan = off']]}   # the sift scans are not
# one plan node (an index scan, a custom scan that counts as a sequential scan), so the first of these whose plan is sift's is the arm; none: the arm cannot be forced on this statement


class Subject:
    """the data of the measurement: dataset name, rows, keys, how the heap holds the key (arrival, sorted, hash, other), the variant (the table of a comparison that has several), the churn history, the table state (schema.STATES) and the protocol of a repetition"""

    def __init__(self, dataset, rows, heap_order, table_state, table, key=None, keys=None, protocol='single', structures=None, kind='read', default_option='default', variant='main', churn=None, settled=None, facts=None):
        self.default_option, self.variant = default_option, variant
        self.facts = facts   # churn_facts of the primary taken beside the table: a standby has no statistics of its own (pg_stat is not replicated), the arms start from the primary's
        self.churn, self.settled = churn, settled   # CHURN: what apply_churn returned; SETTLED: the harness waited for the maintenance to settle after the churn (True), or did not (False)
        self.dataset, self.rows, self.heap_order, self.table_state = dataset, rows, heap_order, table_state
        self.table, self.key, self.keys, self.protocol, self.structures, self.kind = table, key, keys, protocol, structures, kind


NATIVE = schema.NATIVE   # the arm of the lane that runs the normal plan (sift.enable_agg = off): its settings are the harness's, not a structure to force


class Arm:
    """a structure forced on the data: arm (heap, btree, gin, sift), option (default, key_order=ts, ...), the settings that force it (FORCING[arm]), an optional SQL of its own, an optional prepare of its own, the structures its table holds when they differ from the subject's (a load of a heap with a b-tree beside one of a sift_table)"""

    def __init__(self, arm, option='default', settings=None, sql=None, prepare=None, structures=None, base=None, keep=False, segments=None):
        self.arm, self.option, self.sql, self.prepare, self.structures = arm, option, sql, prepare, structures   # PREPARE: the arm's own, run before each repetition of a maintenance or a load (else the measure's)
        self.segments = segments   # the segment count the harness says the arm's sift index has; recorded beside the measured one, and check.py refuses a row where they differ
        self.keep = keep   # a fresh case's arm that keeps its backend: the statement as the second of a backend (protocol single)
        self.tries = [settings] if settings is not None else [list(base or []) + t for t in TRIES.get(arm) or [FORCING[arm]]]   # BASE: the settings of a variant, in front of each; ways to force the arm: the first whose plan is the structure of its name is used
        self.settings = self.tries[0]


class Env:
    """the build and the harness: the commit (12 hex digits), the harness name, the run id, the cpu quota, the repetitions and whether the server runs with the maintenance disabled"""

    def __init__(self, tip, harness, run, cpus, reps=3, cool=None, maintenance='default', arm_budget=None):
        self.arm_budget = arm_budget   # seconds one arm may take over all its repetitions: past it the arm is recorded as unavailable (ArmTimeout) and the run goes on
        self.maintenance = maintenance   # 'disabled' declares that the server runs with the maintenance off (a test cluster's configuration): check.py refuses the set otherwise
        self.tip, self.harness, self.run_id, self.cpus, self.reps, self.cool = tip, '%s %s' % (harness, schema.LIBRARY), run, cpus, reps, cool


# --- pure parts

def plan_nodes(plan):
    yield plan
    for p in plan.get('Plans', []):
        yield from plan_nodes(p)


def plan_structure(plan, index_am, table=None, index_table=None):
    """heap, btree, gin, sift or mixed for the plan of EXPLAIN (FORMAT JSON): INDEX_AM maps an index name to its access method (btree, gin, sift...). The scan nodes name the
    structure; a plan with no scan of a table (a Result) is mixed, which no arm is. With TABLE (the subject of the statement: `name` or `schema.name`, or the list of those of a table and its
    partitions, which is what a plan of a partitioned table names: `subject_tables`) only the scans of that table count, the heap of a probe table beside the index of the probed one is not a
    mix: INDEX_TABLE maps an index name to its table for the index scans that name no relation"""
    index_table = index_table or {}
    names = [table] if isinstance(table, str) else list(table or [])
    schemas = {}
    for name in names:
        schema_name, _, bare = name.rpartition('.')
        schemas.setdefault(bare, set()).add(schema_name)

    def scans(only):
        found = set()
        for n in plan_nodes(plan):
            t = n.get('Node Type', '')
            rel = n.get('Relation Name') or index_table.get(n.get('Index Name'))
            if only and rel and not (rel in schemas and (n.get('Schema') is None or n['Schema'] in schemas[rel] or '' in schemas[rel])):
                continue
            if t == 'Seq Scan' or t == 'Tid Scan':
                found.add('heap')
            elif t in ('Index Scan', 'Index Only Scan', 'Bitmap Index Scan'):
                am = index_am.get(n.get('Index Name'), '?')
                found.add('sift' if am.startswith('sift') else am if am in ('btree', 'gin') else 'other:' + am)
            elif t == 'Custom Scan' and 'sift' in (n.get('Custom Plan Provider', '') or '').lower():
                found.add('sift')
            elif t == 'Custom Scan':
                found.add('other:custom')
        return found
    found = scans(bool(names)) or scans(False)   # no scan of the table itself (a view): every scan of the plan
    return found.pop() if len(found) == 1 and not next(iter(found)).startswith('other:') else 'mixed'


def subject_tables(s, table):
    """the relations whose scans a plan of TABLE names, as `schema.name`: the partition tree of a partitioned table (itself, and its partitions at every level), else TABLE as given"""
    tree = [r[0] for r in all_rows(s, "SELECT n.nspname || '.' || c.relname FROM pg_partition_tree(to_regclass('%s')) t JOIN pg_class c ON c.oid = t.relid "
                                      "JOIN pg_namespace n ON n.oid = c.relnamespace" % table)]
    return tree or [table]


def catalog_of(s, table):
    """what `plan_structure` and `plan_facts` need to name the structure of a plan of TABLE: (index access methods, the relations of TABLE, the table of each index)"""
    return index_methods(s), subject_tables(s, table), index_tables(s)


def plan_facts(doc, index_am, table=None, index_table=None):
    """(plan_ms, exec_ms, cost, structure, workers_requested, workers_launched) of the JSON document of EXPLAIN (ANALYZE, SUMMARY)"""
    plan = doc['Plan']
    gathers = [n for n in plan_nodes(plan) if n.get('Node Type') in ('Gather', 'Gather Merge')]
    return (doc.get('Planning Time'), doc.get('Execution Time'), plan.get('Total Cost'), plan_structure(plan, index_am, table, index_table),
            sum(n.get('Workers Planned', 0) for n in gathers), sum(n.get('Workers Launched', 0) for n in gathers))


def plan_shape(plan):
    """the shape of a plan as text: the node type (with its join type, aggregate strategy and parallel mode), the relation, index or custom scan provider it reads, and its children in
    parentheses; no cost, row count or time, so two runs of one plan have one shape"""
    label = ('Parallel ' if plan.get('Parallel Aware') else '') + plan.get('Node Type', '?')
    for key in ('Join Type', 'Strategy', 'Partial Mode'):
        if plan.get(key) and plan[key] not in ('Plain', 'Simple'):
            label += '[%s]' % plan[key]
    what = plan.get('Index Name') or plan.get('Relation Name') or plan.get('Custom Plan Provider')
    label += ' %s' % what if what else ''
    kids = [plan_shape(p) for p in plan.get('Plans', [])]
    return label + ('(%s)' % ', '.join(kids) if kids else '')


def plan_hash(shape):
    """twelve hex digits of a plan shape"""
    return hashlib.sha1(shape.encode()).hexdigest()[:12]


def cache_dropped(how):
    """1 when `iolib.cool` emptied the OS cache too, 0 when only the shared buffers went (tmpfs, or the OS cache kept), None when nothing was emptied"""
    return 1 if how.startswith('evict+fadvise') else None if how.startswith('warm') else 0


def kind_of(amname):
    return 'sift' if amname.startswith('sift') else amname if amname in ('btree', 'gin') else 'other'


def block_deltas(before, after):
    """{'heap_hit': n, 'heap_read': n, 'btree_hit': ...} from two snapshots {(kind, 'hit' or 'read'): n}"""
    return {'%s_%s' % k: after.get(k, 0) - before.get(k, 0) for k in set(after) | set(before) if k[0] in ('heap', 'btree', 'gin', 'sift', 'other')}


def psi(text):
    """avg10 of the `some` line of /proc/pressure/*"""
    m = re.search(r'^some .*avg10=([0-9.]+)', text, re.M)
    return float(m.group(1)) if m else None


def cpu_quota(max_text, affinity):
    """cpus a process may use: the quota of cgroup v2 cpu.max ("200000 100000" is 2; "max 100000" none) or the cpus of its affinity, the smaller"""
    cpus = float(affinity)
    parts = (max_text or '').split()
    if len(parts) == 2 and parts[0] != 'max':
        cpus = min(cpus, int(parts[0]) / int(parts[1]))
    return cpus


def pg_bytes_mb(text):
    """megabytes of a setting such as 256MB, 128MB, 1GB, 4096kB"""
    m = re.fullmatch(r'(\d+(?:\.\d+)?)\s*(B|kB|MB|GB|TB)', text.strip())
    return float(m.group(1)) * {'B': 1 / 2 ** 20, 'kB': 1 / 1024, 'MB': 1, 'GB': 1024, 'TB': 2 ** 20}[m.group(2)]


# --- the server

def all_rows(s, sql):
    _, rows, err, *_ = s.run(sql)
    if err:
        raise RuntimeError('%s: %s' % (sql[:80], err))
    return [r.split('|') for r in rows]


def one(s, sql):
    rows = all_rows(s, sql)
    return rows[0] if rows else []


def backend_pid(s):
    return int(one(s, 'SELECT pg_backend_pid()')[0])


def postmaster_pid(pid):
    with open('/proc/%d/stat' % pid) as f:
        return int(f.read().rsplit(')', 1)[1].split()[1])


def read_text(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ''


def git_tip(override=None):
    """the commit of the build: `override` (--tip), or HEAD of the work tree (a tree that holds more than the commit says so in --note, which the records carry in `harness`)"""
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    return override or subprocess.run(['git', '-C', repo, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'


def host_cpus(pid):
    """the cpu quota of the server process PID: its cgroup's cpu.max against its affinity"""
    group = re.search(r'^0::(.*)$', read_text('/proc/%d/cgroup' % pid), re.M)
    quota = read_text('/sys/fs/cgroup%s/cpu.max' % (group.group(1) if group else ''))
    return cpu_quota(quota, len(os.sched_getaffinity(pid)))


def server_env(s):
    """the settings the figures depend on: full_page_writes, shared_buffers, work_mem, the version"""
    fpw, sb, wm, ver = one(s, "SELECT current_setting('full_page_writes'), current_setting('shared_buffers'), current_setting('work_mem'), current_setting('server_version')")
    return dict(fpw=1 if fpw == 'on' else 0, shared_buffers_mb=pg_bytes_mb(sb), work_mem_mb=pg_bytes_mb(wm), pg_version=ver.split()[0])


def settings_of(s):
    """{name: value} of every setting that is not the default: the cluster (files, command line) and the session as it is before an arm forces anything"""
    rows = s.run("SELECT name || '=' || current_setting(name) FROM pg_settings WHERE source NOT IN ('default', 'override') OR name = 'sift.eval_methods' ORDER BY name")[1]
    return dict(r.split('=', 1) for r in rows if '=' in r)


def session_base(s):
    """the statements that restore the settings the harness made in this session (RESET ALL between arms would drop them)"""
    return ["SELECT set_config('%s', '%s', false)" % (n.replace("'", "''"), v.replace("'", "''")) for n, v in (r.split('|', 1) for r in s.run("SELECT name || '|' || setting FROM pg_settings WHERE source = 'session'")[1] if '|' in r)]


def index_tables(s):
    """{index name: the name of its table} of the database"""
    return dict(r.split('|') for r in s.run("SELECT i.relname || '|' || t.relname FROM pg_index x JOIN pg_class i ON i.oid = x.indexrelid JOIN pg_class t ON t.oid = x.indrelid")[1] if r)


def index_methods(s):
    """{index name: access method} of the database"""
    return dict(r.split('|') for r in s.run("SELECT c.relname || '|' || am.amname FROM pg_class c JOIN pg_am am ON am.oid = c.relam WHERE c.relkind = 'i'")[1] if r)


def table_facts(s, table, key=None):
    """(allvisible_pct, heap_corr, structures) of a table, measured: pages all-visible (over the leaves of a partitioned table; none while a page count is unknown), the correlation of KEY with the heap,
    and the structures it holds (heap, plus the access methods of its indexes)"""
    counts = [[float(x) for x in one(s, "SELECT relpages, relallvisible, reltuples FROM pg_class WHERE oid = '%s'::regclass" % leaf)] for leaf in leaf_tables(s, table)]
    pages, vis = (sum(c[i] for c in counts) for i in (0, 1))
    known = bool(counts) and all(c[0] >= 0 and c[2] >= 0 for c in counts) and pages > 0   # a table no VACUUM or ANALYZE has counted has reltuples -1 (a partitioned one, relpages -1): unknown, not a share of 0
    corr = one(s, "SELECT correlation FROM pg_stats WHERE schemaname || '.' || tablename IN ('%s', 'public.%s') AND attname = '%s'" % (table, table, key))[0:1] if key else []
    ams = {kind_of(r) for r in s.run("SELECT am.amname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am am ON am.oid = c.relam WHERE i.indrelid = '%s'::regclass" % table)[1] if r}
    structures = ','.join(sorted({'heap'} | {k for k in ams if k != 'other'}))
    return (100.0 * vis / pages if known else None, float(corr[0]) if corr and corr[0] else None, structures)


def table_structures(s, table):
    """the structures TABLE holds as the records say them, measured: heap, the access method of each index that is not a constraint's, and `constraint` when an index backs a primary key,
    a unique or an exclusion constraint of the table (its definition: every write pays for it, and so it does in every arm of a comparison). None when TABLE names no relation"""
    if one(s, "SELECT to_regclass('%s') IS NOT NULL" % table)[0] != 't':
        return None
    rows = all_rows(s, "SELECT am.amname, EXISTS (SELECT 1 FROM pg_constraint k WHERE k.conindid = i.indexrelid AND k.conrelid = i.indrelid AND k.contype IN ('p', 'u', 'x')) "
                       "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid JOIN pg_am am ON am.oid = ic.relam WHERE i.indrelid = to_regclass('%s')" % table)
    return ','.join(sorted({'heap'} | {kind_of(am) for am, backs in rows if backs != 't'} - {'other'} | ({'constraint'} if any(backs == 't' for _, backs in rows) else set())))


def statio(s):
    """{(kind, 'hit' or 'read'): blocks} of the user relations: tables as heap, TOAST as other, indexes by access method; the statistics are flushed first"""
    s.run('SELECT pg_stat_force_next_flush()')
    user = "schemaname NOT IN ('pg_catalog', 'information_schema', 'pg_toast')"
    rows = s.run("SELECT 'heap', coalesce(sum(heap_blks_hit), 0), coalesce(sum(heap_blks_read), 0) FROM pg_statio_all_tables WHERE %s UNION ALL "
                 "SELECT 'other', coalesce(sum(toast_blks_hit), 0), coalesce(sum(toast_blks_read), 0) FROM pg_statio_all_tables WHERE %s UNION ALL "
                 "SELECT am.amname, sum(idx_blks_hit), sum(idx_blks_read) FROM pg_statio_all_indexes x JOIN pg_class c ON c.oid = x.indexrelid JOIN pg_am am ON am.oid = c.relam "
                 "WHERE %s GROUP BY am.amname" % (user, user, user.replace('schemaname', 'x.schemaname')))[1]
    out = {}
    for r in rows:
        if not r:
            continue
        kind, hit, read = r.split('|')
        k = kind_of(kind) if kind not in ('heap', 'other') else kind
        out[(k, 'hit')] = out.get((k, 'hit'), 0) + int(hit or 0)
        out[(k, 'read')] = out.get((k, 'read'), 0) + int(read or 0)
    return out


def apply_churn(s, table, column, upd=0.0, ins=0.0, dele=0.0, rounds=1, insert_sql=None):
    """churn TABLE for ROUNDS rounds, each: UPD percent of the rows rewritten (UPDATE of COLUMN to itself, a new tuple version), DELE percent deleted, INS percent inserted by INSERT_SQL ({pct}
    is the percent; a table with a key needs the statement that makes new keys). Returns the churn history for Subject(churn=...); the harness VACUUMs or waits as its lane says and declares it
    with Subject(settled=...). Every harness churns with this, so a history means the same thing in each"""
    if ins and not insert_sql:
        raise ValueError('inserts need the statement that makes the new rows (insert_sql with {pct})')
    for _ in range(rounds):
        for sql in ([ 'UPDATE %s SET %s = %s WHERE random() < %s' % (table, column, column, upd / 100.0)] if upd else []) + (['DELETE FROM %s WHERE random() < %s' % (table, dele / 100.0)] if dele else []) + \
                   ([insert_sql.format(pct=ins)] if ins else []):
            run_all(s, sql)
    return 'upd=%g;ins=%g;del=%g;rounds=%d' % (upd, ins, dele, rounds)


def leaf_tables(s, table):
    """[TABLE], or its leaf partitions when it is partitioned: a partitioned table has no pages, statistics, storage or access method of its own, its leaves hold the data (none for a
    partitioned table with no partition). A name that is no relation raises"""
    if one(s, "SELECT relkind FROM pg_class WHERE oid = '%s'::regclass" % table)[0] != 'p':
        return [table]
    return [r[0] for r in all_rows(s, "SELECT relid::regclass::text FROM pg_partition_tree('%s') WHERE isleaf" % table)]


def table_facts_of(s, table):
    """the statistics of the plain table TABLE, with the dead rows of its sift structure: {ins, upd, del, live, dead, vacuums, autovacuums, since, passed, dead_rows, rows}, or None when it
    has none of its own (a view). since is the seconds from its last vacuum, passed from the last pass of its sift index; dead_rows and rows are those of the sift structure, None without one"""
    row = one(s, "SELECT n_tup_ins, n_tup_upd, n_tup_del, n_live_tup, n_dead_tup, vacuum_count, autovacuum_count, "
                 "extract(epoch FROM now() - greatest(last_vacuum, last_autovacuum)) FROM pg_stat_all_tables WHERE relid = '%s'::regclass" % table)
    if not row:
        return None
    ints = [int(x) for x in row[:7]]
    out = dict(zip(('ins', 'upd', 'del', 'live', 'dead', 'vacuums', 'autovacuums'), ints), since=float(row[7]) if row[7] else None, passed=None, dead_rows=None, rows=None)
    try:
        if one(s, "SELECT am.amname FROM pg_class c JOIN pg_am am ON am.oid = c.relam WHERE c.oid = '%s'::regclass" % table)[0] == 'sift_table':
            out['dead_rows'], out['rows'] = (int(x) for x in one(s, "SELECT coalesce(sum(dead_rows), 0), coalesce(sum(rows), 0) FROM sift.table_segments('%s') WHERE kind = 'columns'" % table))
        index = one(s, "SELECT i.indexrelid::regclass FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am am ON am.oid = c.relam WHERE i.indrelid = '%s'::regclass AND am.amname = 'sift_col' LIMIT 1" % table)
        if index:
            out['dead_rows'], out['rows'] = (int(x) for x in one(s, "SELECT coalesce(sum(dead_rows), 0), coalesce(sum(rows), 0) FROM sift.col_segments('%s')" % index[0]))
            passed = one(s, "SELECT extract(epoch FROM now() - last_vacuum) FROM sift.col_maintenance_status('%s')" % index[0])
            out['passed'] = float(passed[0]) if passed and passed[0] else None
    except RuntimeError:
        pass   # the sift functions are not in this database: no tombstones to count
    return out


def churn_facts(s, table, declared=None):
    """the state of TABLE the arms start from, measured: {churn_history, vacuums, autovacuums, settled_s, dead_pct, tombstone_pct} from pg_stat_all_tables, the maintenance status of its sift
    index and the dead rows of that index's segments. The history is DECLARED (apply_churn) or, for a table churned by a script, the tuples its statistics counted.
    A partitioned table has no statistics, storage or access method of its own: its facts are those of its leaf partitions together. The tuples and dead tuples are summed; vacuums and
    autovacuums are the fewest of any leaf (every leaf had at least that many); settled_s is the time since the most recent pass of any leaf, none while a leaf has had no pass;
    tombstone_pct is over the leaves that have a sift structure. A view, or a partitioned table with no leaf, has none: {}"""
    s.run('SELECT pg_stat_clear_snapshot()')
    parts = [table_facts_of(s, leaf) for leaf in leaf_tables(s, table)]
    if not parts or None in parts:
        return {}
    total = lambda k: sum(p[k] for p in parts)  # noqa: E731
    since = [min((x for x in (p['since'], p['passed']) if x is not None), default=None) for p in parts]   # the last pass of each leaf
    dead_rows, indexed = (sum(p[k] or 0 for p in parts) for k in ('dead_rows', 'rows'))
    return dict(churn_history=declared or 'ins=%s;upd=%s;del=%s' % (total('ins'), total('upd'), total('del')), vacuums=min(p['vacuums'] for p in parts),
                autovacuums=min(p['autovacuums'] for p in parts), settled_s=None if None in since else min(since),
                dead_pct=100.0 * total('dead') / max(1, total('live') + total('dead')),
                tombstone_pct=100.0 * dead_rows / indexed if indexed else None)


def start_state(s, subject):
    """the churn facts the arms start from: those the harness took on the primary (Subject.facts, for a standby, which has no statistics of its own) or measured on S; a declared history wins"""
    if subject.facts:
        return dict(subject.facts, **({'churn_history': subject.churn} if subject.churn else {}))
    return churn_facts(s, subject.table, subject.churn)


def subject_oids(table):
    """SQL for the oids of the tables a measurement is of: TABLE (the leaf partitions of a partitioned one) or, when it names a schema, the tables of the schema"""
    return ("(SELECT relid AS oid FROM pg_partition_tree(to_regclass('%s')) WHERE isleaf UNION SELECT c.oid FROM pg_class c WHERE c.oid = to_regclass('%s') AND c.relkind <> 'p') "
            "UNION ALL SELECT c.oid FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = '%s' AND c.relkind IN ('r', 'p', 'm')" % (table, table, table))


def structure_bytes(s, table, arm):
    """(table_bytes, index_bytes) after a run: pg_table_size of TABLE (a load names the schema that holds its tables: their sum; a partitioned table has no storage, its leaf partitions
    are summed) and the pg_relation_size of the indexes of ARM's structure, None for the heap arm, for a mixed plan and when the table has no index of that structure"""
    tables = subject_oids(table)
    size = int(one(s, 'SELECT coalesce(sum(pg_table_size(oid)), 0) FROM (%s) t' % tables)[0])
    if arm in ('heap', 'mixed'):   # no index is the plan's: a mixed plan names none
        return size, None
    match = "am.amname LIKE 'sift%'" if arm == 'sift' else "am.amname = '%s'" % arm
    index = one(s, 'SELECT count(*), coalesce(sum(pg_relation_size(i.indexrelid)), 0) FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid JOIN pg_am am ON am.oid = ic.relam '
                   'WHERE i.indrelid IN (%s) AND %s' % (tables, match))
    return size, int(index[1]) if int(index[0]) else None   # no index of that structure (a sift_table keeps its structure in the table): unknown, not 0 bytes


def index_facts(s, table):
    """what the directory of the sift indexes of the tables of TABLE (as subject_oids) holds after a run, summed: {segments, immutable_segments, buffer_segments, index_options}. The segments
    are the rows of sift.col_segments (sift_col) and sift.segments (sift); a write buffer is a `buffer`, `sealed buffer`, `active` or `sealed` one. index_options are the reloptions of the
    indexes, sorted, those of different indexes told apart by ` | ` (`default` when none has any). Empty when there is no sift index: the heap, b-tree and GIN arms have no segments"""
    indexes = all_rows(s, "SELECT am.amname, i.indexrelid, coalesce((SELECT string_agg(o, ';' ORDER BY o) FROM unnest(ic.reloptions) o), '') FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
                          "JOIN pg_am am ON am.oid = ic.relam WHERE am.amname IN ('sift', 'sift_col') AND i.indrelid IN (SELECT oid FROM (%s) t) ORDER BY 2" % subject_oids(table))
    if not indexes:
        return {}
    counts = [0, 0, 0]
    for am, oid, _ in indexes:
        fn = 'sift.col_segments' if am == 'sift_col' else 'sift.segments'
        got = one(s, "SELECT count(*), count(*) FILTER (WHERE kind = 'immutable'), count(*) FILTER (WHERE kind IN ('buffer', 'sealed buffer', 'active', 'sealed')) FROM %s(%s::regclass)" % (fn, oid))
        counts = [a + int(b) for a, b in zip(counts, got)]
    options = sorted({o for _, _, o in indexes if o})
    return dict(zip(('segments', 'immutable_segments', 'buffer_segments'), counts), index_options=' | '.join(options) or 'default')


def wal_lsn(s):
    """the WAL position of the server: written (pg_current_wal_lsn) on a primary, replayed (pg_last_wal_replay_lsn) on a standby, which has no current one"""
    return int(one(s, "SELECT pg_wal_lsn_diff(CASE WHEN pg_is_in_recovery() THEN pg_last_wal_replay_lsn() ELSE pg_current_wal_lsn() END, '0/0')::bigint")[0])


def pressure():
    return psi(read_text('/proc/pressure/cpu')), psi(read_text('/proc/pressure/io'))


SHORT_MS = 5.0     # a statement faster than this is counted over a window of runs, not one run
WINDOW_MS = 5.0    # the window of a short one holds at least this much of the statement
MAX_REPEATS = 5000


def repeats_for(ms):
    """how many times a statement of MS (client time of one run) is run in the window its instructions are counted over: 1 for a long one, else enough for the window to hold WINDOW_MS of
    it and at least 50 runs (a count of one run of 20 microseconds is lost in what perf attaches and detaches), at most MAX_REPEATS"""
    if ms >= SHORT_MS:
        return 1
    return min(MAX_REPEATS, max(50, math.ceil(WINDOW_MS / max(ms, 0.001))))


def run_all(s, sql):
    """milliseconds of SQL on S: a list of statements one after the other with their times added; an error raises, a statement that failed is no measurement"""
    ms = 0.0
    for st in [sql] if isinstance(sql, str) else sql:
        took, _, err, *_ = s.run(st)
        if err:
            raise failed(st[:80], err)
        ms += took
    return ms


class ArmTimeout(RuntimeError):
    """a statement of an arm reached the statement timeout, or the arm used its time budget: the reason. The arm is recorded as unavailable and the other arms and the run go on"""


def failed(what, err):
    """the exception for the error ERR of a statement: the statement timeout belongs to the arm (ArmTimeout), any other error stops the run"""
    return (ArmTimeout if 'statement timeout' in err else RuntimeError)('%s: %s' % (what, err))


class PerfUnavailable(RuntimeError):
    """perf could not count the instructions of a statement: the reason of each attempt, and the `perf stat` processes that hold the counters"""


def stale_perf(min_age_s=600, proc='/proc'):
    """[(pid, age in seconds, uid)] of `perf stat -p` processes older than MIN_AGE_S: a count that never ended (a shell killed before its trap, a lane that stopped) keeps counting while the
    process it watches lives and holds the counters of the processor, so every later `perf stat` reports <not counted>"""
    out = []
    ticks = os.sysconf('SC_CLK_TCK')
    up = float(read_text(os.path.join(proc, 'uptime')).split()[0] or 0)
    for d in os.listdir(proc):
        if not d.isdigit():
            continue
        try:
            args = read_text(os.path.join(proc, d, 'cmdline')).split('\0')
            if os.path.basename(args[0]) != 'perf' or 'stat' not in args or '-p' not in args:
                continue
            age = up - int(read_text(os.path.join(proc, d, 'stat')).rsplit(')', 1)[1].split()[19]) / ticks
            uid = os.stat(os.path.join(proc, d)).st_uid
        except (OSError, ValueError, IndexError):
            continue
        if age > min_age_s:
            out.append((int(d), int(age), uid))
    return sorted(out)


def check_perf_free():
    held = stale_perf()
    if held:
        raise PerfUnavailable('perf stat processes older than 10 minutes hold the counters of the processor, so a count reports <not counted>: pids %s (age in seconds %s): end them with `kill -INT PID`' % (
            ', '.join(str(p) for p, _, _ in held), ', '.join(str(a) for _, a, _ in held)))


def wait_attached(p, limit=10.0):
    """until the shell of a count has started its perf (it has a child) and a moment longer for perf to open its counters"""
    end = time.time() + limit
    while time.time() < end and p.poll() is None:
        if read_text('/proc/%d/task/%d/children' % (p.pid, p.pid)).split():
            time.sleep(0.5)
            return True
        time.sleep(0.02)
    return False


def stop_perf(p, wait=30):
    """the output of the count PROCESS p: SIGINT to its group (perf prints its counts on it), SIGKILL to anything that is left, on every path: no perf outlives its count"""
    out = ''
    try:
        try:
            os.killpg(p.pid, signal.SIGINT)
        except ProcessLookupError:
            pass
        try:
            out, _ = p.communicate(timeout=wait)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            out, _ = p.communicate()
    finally:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return out


def count_instructions(s, sql, pid, post, tries=3, repeats=1, leader=False):
    """(millions of instructions, millions of cycles) of one run of SQL on the backend PID and the postmaster POST (the workers are its children, forked after perf attached), counted
    over REPEATS runs in the one backend between the two readings and divided (a short statement). LEADER counts the backend PID alone (a build's leader: the workers are the postmaster's
    children, so they are left out). The count is tried again up to TRIES times; PerfUnavailable says why each attempt failed (never a count of the backend alone unless asked for,
    never a record without instructions). Whatever happens, no perf of this count is left running"""
    why = []
    for attempt in range(tries):
        p = subprocess.Popen(['bash', PERF, str(pid) if leader else '%d,%d' % (pid, post)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            if not wait_attached(p):
                why.append('attempt %d: perf did not start' % (attempt + 1))
                continue
            for _ in range(repeats):
                run_all(s, sql)
        finally:
            out = stop_perf(p)
        f = out.split()
        if len(f) == 3 and f[0].isdigit() and f[1].isdigit():
            return int(f[1]) / 1e6 / repeats, int(f[0]) / 1e6 / repeats
        why.append('attempt %d: perf printed "%s" (NA is a counter that was not counted)' % (attempt + 1, out.strip()[:60]))
    held = stale_perf(60)
    raise PerfUnavailable('perf counters unavailable; %s%s' % ('; '.join(why), ('; perf stat processes alive for over a minute: pids %s' % ', '.join(str(p) for p, _, _ in held)) if held else ''))


class WorkerMonitor:
    """counts the parallel workers a statement starts: the most seen at once in pg_stat_activity of the monitor session, sampled while the statement runs"""

    def __init__(self, monitor):
        self.monitor, self.most, self.stop = monitor, 0, threading.Event()

    def __enter__(self):
        def sample():
            while not self.stop.wait(0.02):
                r = one(self.monitor, "SELECT count(*) FROM pg_stat_activity WHERE backend_type IN ('parallel worker', 'background worker') OR backend_type LIKE 'parallel%'")
                self.most = max(self.most, int(r[0]))
        self.thread = threading.Thread(target=sample, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *a):
        self.stop.set()
        self.thread.join()


# --- the measurement

def measure_statement(s, statement, sql, subject, arms, env, prepare=None, monitor=None, temps=('warm', 'cold'), hash_text=None, fresh=None, unavailable=None):
    """the records of SQL on the default plan (a read) and on each of ARMS: a list of dicts for schema.write. SQL is a statement or, for a maintenance or a load, a list of them, timed
    one after the other and added. PREPARE (before each repetition of a maintenance or a load, and before the run that counts its instructions: the table is as the repetition left it
    otherwise) and MONITOR as in the module docstring; an Arm may have its own prepare. A harness with several statements calls it once for each. HASH_TEXT is the text whose hash names
    the statement when a harness runs one question on tables of other names (the text with a placeholder for the table): the variants of a comparison then share one sql_hash.
    FRESH, a function of an arm that opens a new session with the settings of the lane and of the arm, makes every repetition run in a backend of its own (`-- fresh: yes`: the first statement of a backend):
    the timed run, the EXPLAIN and the instruction count each in a new one, while S holds the facts of the server. An arm for which none of the ways to force it (Arm.tries)
    gives the structure of its name is not measured: one row of mode unavailable says so in the file (its `structure` is what the plans named: the b-tree twin that cannot serve a statement is an
    answer, not a refusal), and UNAVAILABLE, a list, gets (arm, option, the structures the plans named)."""
    check_perf_free()
    pid = backend_pid(s)
    post = postmaster_pid(pid)
    catalog = catalog_of(s, subject.table)
    srv = server_env(s)
    pairs = settings_of(s)
    restore = session_base(s)
    read = subject.kind == 'read'
    vis, corr, structures = table_facts(s, subject.table, subject.key) if read else (None, None, ','.join(sorted({'heap'} | {a.arm for a in arms})))
    churn = start_state(s, subject) if read else {}
    if churn and not subject.table_state.startswith('churned'):
        churn['churn_history'] = None   # the tuples a fresh table's statistics counted are its load
    base = dict(run_id=env.run_id, tip=env.tip, harness=env.harness, dataset=subject.dataset, variant=subject.variant, rows=subject.rows, keys=subject.keys,
                rows_per_key=(subject.rows / subject.keys) if subject.keys else None, heap_order=subject.heap_order, heap_corr=corr, table_state=subject.table_state,
                allvisible_pct=vis, structures=subject.structures or structures, statement=statement, sql_hash=schema.sql_hash(hash_text or (sql if isinstance(sql, str) else ';\n'.join(sql))),
                kind=subject.kind, protocol=subject.protocol, cpus=env.cpus, settings=schema.settings_text(pairs), settings_hash=schema.settings_hash(pairs),
                maintenance=env.maintenance, settled=None if subject.settled is None else int(subject.settled), **churn, **srv)
    out, default = [], None
    for mode, arm in ([('default', Arm('sift', settings=[]))] if read else []) + [('forced', a) for a in arms]:
        text = arm.sql or sql
        if read and mode == 'forced' and arm.arm != NATIVE:
            forced, named = forceable(s, arm, text, catalog, restore)
            if forced is None:
                got = named[2].split(', ')
                out.append(dict(base, mode='unavailable', arm=arm.arm, arm_option=arm.option, picked=0, structure=got[0] if len(got) == 1 else 'mixed', temp='warm', rep=1,
                                workers_requested=0, workers_launched=0))
                if unavailable is not None:
                    unavailable.append(named)
                continue
            arm = forced
        for st in arm.settings:
            s.run(st)
        try:
            rows = arm_rows(s, text, subject, env, base, mode, arm, pid, post, catalog, arm.prepare or prepare, monitor, temps, fresh)
        except NotForced as e:
            out.append(dict(base, mode='unavailable', arm=arm.arm, arm_option=arm.option, picked=0, structure=e.structure, temp='warm', rep=1, workers_requested=0, workers_launched=0))
            if unavailable is not None:
                unavailable.append((arm.arm, arm.option, e.structure))
            continue
        except ArmTimeout as e:
            if mode == 'default':
                raise   # the plan the planner chooses is what the others are compared with: no arm to put in its place
            out.append(dict(base, mode='unavailable', arm=arm.arm, arm_option=arm.option, picked=0, structure=arm.arm, temp='warm', rep=1, workers_requested=0, workers_launched=0))
            if unavailable is not None:
                unavailable.append((arm.arm, arm.option, 'timeout: %s' % e))
            continue
        finally:
            s.run('RESET ALL')
            for st in restore:
                s.run(st)
        if mode == 'default':
            for r in rows:
                r['arm'], r['arm_option'], r['picked'] = r['structure'], subject.default_option, 1
            default = (rows[0]['arm'], subject.default_option)
        else:
            for r in rows:
                r['picked'] = 1 if (r['arm'], r['arm_option']) == default else 0
        out += rows
    if read:
        match_default(out)
    return out


def match_default(rows):
    """the arms that ran the plan the planner chose, matched by plan hash: their rows are picked, the others are not, and the default rows name the option of the arm that ran their plan
    (the one of the structure the plan names, `default` first). Without hashes, or when the planner's plan changed between its repetitions, the structure and option rule stands"""
    hashes = {r['plan_hash'] for r in rows if r['mode'] == 'default' and r.get('plan_hash')}
    if len(hashes) != 1:
        return
    ran = defaultdict(set)
    for r in rows:
        if r['mode'] == 'forced' and r.get('plan_hash') and r['arm'] != NATIVE:
            ran[(r['arm'], r['arm_option'])].add(r['plan_hash'])
    matched = {k for k, h in ran.items() if h == hashes}
    for r in rows:
        if r['mode'] == 'forced':
            r['picked'] = int((r['arm'], r['arm_option']) in matched)
    structure = next((r['structure'] for r in rows if r['mode'] == 'default'), None)
    options = sorted((k[1] != 'default', k[1]) for k in matched if k[0] == structure)
    if options:
        for r in rows:
            if r['mode'] == 'default':
                r['arm_option'] = options[0][1]


class NotForced(Exception):
    def __init__(self, structure):
        super().__init__(structure)
        self.structure = structure


def forceable(s, arm, sql, catalog, restore):
    """(ARM with the first of its settings under which the plan of SQL is the structure of its name, None) or (None, (arm, option, the structures the plans named))"""
    named = []
    for sets in arm.tries:
        for st in sets:
            s.run(st)
        try:
            _, rows, err, *_ = s.run('EXPLAIN (FORMAT JSON) ' + sql.rstrip().rstrip(';'))
            if err:
                raise RuntimeError('EXPLAIN: %s' % err)
            got = plan_structure(json.loads('\n'.join(rows))[0]['Plan'], *catalog)
        finally:
            s.run('RESET ALL')
            for st in restore:
                s.run(st)
        if got == arm.arm:
            arm.settings = sets
            return arm, None
        named.append(got)
    return None, (arm.arm, arm.option, ', '.join(sorted(set(named))))


def arm_rows(s, sql, subject, env, base, mode, arm, pid, post, catalog, prepare, monitor, temps, fresh=None):
    read = subject.kind == 'read'
    structure, cost, requested = arm.arm, None, 0
    explain = lambda: EXPLAIN + (sql if isinstance(sql, str) else sql[-1]).rstrip().rstrip(';')   # noqa: E731
    if read:
        run_all(s, sql)
        _, rows, err, *_ = s.run(explain())
        if err:
            raise failed('EXPLAIN', err)
        _, _, cost, structure, requested, _ = plan_facts(json.loads('\n'.join(rows))[0], *catalog)
        if mode == 'forced' and structure != arm.arm and arm.arm != NATIVE:
            raise NotForced(structure)   # the plan without ANALYZE named the arm, the executed one does not: a node that falls back to the heap when it runs
    out = []
    began = time.monotonic()

    @contextlib.contextmanager
    def on_session():   # the session of a repetition: S, or a backend of its own for a fresh lane
        t = fresh(arm) if fresh and not arm.keep else None
        try:
            yield t or s
        finally:
            if t:
                t.close()

    for temp in temps:
        for rep in range(1, env.reps + 1):
            if prepare:
                prepare()
            how = None
            if temp == 'cold':
                how = env.cool(s, [subject.table]) if env.cool else 'warm: no eviction'
                if cache_dropped(how) is None:
                    raise RuntimeError('the relations of %s could not be evicted: %s' % (subject.table, how))
            elif read:
                run_all(s, sql)
            p0 = pressure()
            b0, lsn0 = statio(s), wal_lsn(s)
            launched = 0
            with on_session() as t:
                if monitor:
                    with WorkerMonitor(monitor) as w:
                        ms = run_all(t, sql)
                    launched = w.most
                else:
                    ms = run_all(t, sql)
            b1, lsn1 = statio(s), wal_lsn(s)
            declared = arm.structures or subject.structures
            if not read and declared:   # a harness that says what its table holds is held to it: a CREATE INDEX that failed, or an index nobody listed, is no measurement
                held = table_structures(s, subject.table)
                if held is not None and set(held.split(',')) != set(declared.split(',')):
                    raise RuntimeError('%s holds %s after the statement, the harness declared %s' % (subject.table, held, declared))
            p1 = pressure()
            plan_ms = exec_ms = shape = None
            if read:
                with on_session() as t:
                    _, rows, err, *_ = t.run(explain())
                if err:
                    raise failed('EXPLAIN', err)
                doc = json.loads('\n'.join(rows))[0]
                plan_ms, exec_ms, cost2, structure, requested, launched = plan_facts(doc, *catalog)
                cost = cost2 if cost2 is not None else cost
                shape = plan_shape(doc['Plan'])
                if mode == 'forced' and structure != arm.arm and arm.arm != NATIVE:
                    raise NotForced(structure)
            shown = structure if mode == 'default' or arm.arm == NATIVE else arm.arm   # the structure the plan used
            table_bytes, index_bytes = structure_bytes(s, subject.table, shown)
            facts = index_facts(s, subject.table) if index_bytes is not None and shown == 'sift' else {}
            instr = cycles = leader = workers_m = None
            if temp == 'warm':
                if prepare and not read:
                    prepare()
                repeats = repeats_for(ms) if read and not fresh else 1   # a new backend for each run is measured once: its first statement is the point
                with on_session() as t:
                    instr, cycles = count_instructions(t, sql, backend_pid(t) if fresh else pid, post, repeats=repeats)
                if subject.kind == 'build':   # the leader apart, in a run of its own on a table prepared again: what the workers did is the rest
                    if prepare:
                        prepare()
                    leader = count_instructions(s, sql, pid, post, leader=True)[0]
                    workers_m = max(0.0, instr - leader)
            if not read:
                requested = launched   # a maintenance statement has no plan that asks for workers: the cap is the setting, the workers it started are what the record can say
            if temp == 'cold':
                plan_ms = exec_ms = None   # the EXPLAIN ran on a warm table after the statement
            r = dict(base, mode=mode, arm=arm.arm, arm_option=arm.option, picked=0, structure=structure, plan_hash=plan_hash(shape) if shape else None, plan_shape=shape[:1000] if shape else None, temp=temp, rep=rep, est_cost=cost, plan_ms=plan_ms, exec_ms=exec_ms, wall_ms=ms,
                     instr_m=instr, cycles_m=cycles, instr_scope='cluster' if instr is not None else None, instr_n=repeats if instr is not None and temp == 'warm' else None, instr_leader_m=leader, instr_workers_m=workers_m,
                     workers_requested=requested, workers_launched=launched, os_cache_dropped=cache_dropped(how) if how else None, wal_bytes=lsn1 - lsn0, table_bytes=table_bytes, index_bytes=index_bytes,
                     segments_declared=arm.segments, **facts, noise=int(max(x or 0 for x in p0 + p1) > check.NOISY_PSI), psi_cpu_start=p0[0], psi_io_start=p0[1], psi_cpu_end=p1[0], psi_io_end=p1[1])
            r.update(block_deltas(b0, b1))
            if arm.structures:
                r['structures'] = arm.structures
            if arm.keep and fresh:
                r['protocol'] = 'single'
            out.append(r)
            used = time.monotonic() - began
            if env and env.arm_budget and used > env.arm_budget and (temp, rep) != (temps[-1], env.reps):
                raise ArmTimeout('the time budget of %d s is used (%.0f s) after %d repetitions of %d' % (env.arm_budget, used, len(out), len(temps) * env.reps))
    return out
