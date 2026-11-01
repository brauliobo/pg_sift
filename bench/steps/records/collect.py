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
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import schema  # noqa: E402

PERF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'perf.sh')
EXPLAIN = 'EXPLAIN (ANALYZE, BUFFERS, SUMMARY, COSTS, FORMAT JSON) '
FORCING = {
    'heap': ['SET enable_indexscan = off', 'SET enable_indexonlyscan = off', 'SET enable_bitmapscan = off', 'SET sift.enable_agg = off', 'SET sift.enable_col_scan = off'],
    'btree': ['SET enable_seqscan = off', 'SET sift.enable_agg = off', 'SET sift.enable_col_scan = off'],
    'gin': ['SET enable_seqscan = off', 'SET sift.enable_agg = off', 'SET sift.enable_col_scan = off'],
    'sift': ['SET enable_seqscan = off'],
}   # the settings that make the planner use a structure when the table holds more than one; a harness with a table for each structure gives its own (Arm.settings) and a search_path


class Subject:
    """the data of the measurement: dataset name, rows, keys, how the heap holds the key (arrival, sorted, hash, other), the table state (schema.STATES) and the protocol of a repetition"""

    def __init__(self, dataset, rows, heap_order, table_state, table, key=None, keys=None, protocol='single', structures=None, kind='read', default_option='default'):
        self.default_option = default_option
        self.dataset, self.rows, self.heap_order, self.table_state = dataset, rows, heap_order, table_state
        self.table, self.key, self.keys, self.protocol, self.structures, self.kind = table, key, keys, protocol, structures, kind


class Arm:
    """a structure forced on the data: arm (heap, btree, gin, sift), option (default, key_order=ts, ...), the settings that force it (FORCING[arm]), an optional SQL of its own"""

    def __init__(self, arm, option='default', settings=None, sql=None):
        self.arm, self.option, self.sql = arm, option, sql
        self.settings = FORCING[arm] if settings is None else settings


class Env:
    """the build and the harness: the commit (12 hex digits), the harness name, the run id, the cpu quota and the repetitions"""

    def __init__(self, tip, harness, run, cpus, reps=3, perf=True, cool=None):
        self.tip, self.harness, self.run_id, self.cpus, self.reps, self.perf, self.cool = tip, '%s %s' % (harness, schema.LIBRARY), run, cpus, reps, perf, cool


# --- pure parts

def plan_nodes(plan):
    yield plan
    for p in plan.get('Plans', []):
        yield from plan_nodes(p)


def plan_structure(plan, index_am):
    """heap, btree, gin, sift or mixed for the plan of EXPLAIN (FORMAT JSON): INDEX_AM maps an index name to its access method (btree, gin, sift...). The scan nodes name the
    structure; a plan with no scan of a table (a Result) is mixed, which no arm is"""
    found = set()
    for n in plan_nodes(plan):
        t = n.get('Node Type', '')
        if t == 'Seq Scan' or t == 'Tid Scan':
            found.add('heap')
        elif t in ('Index Scan', 'Index Only Scan', 'Bitmap Index Scan'):
            am = index_am.get(n.get('Index Name'), '?')
            found.add('sift' if am.startswith('sift') else am if am in ('btree', 'gin') else 'other:' + am)
        elif t == 'Custom Scan' and 'sift' in (n.get('Custom Plan Provider', '') or '').lower():
            found.add('sift')
        elif t == 'Custom Scan':
            found.add('other:custom')
    return found.pop() if len(found) == 1 and not next(iter(found)).startswith('other:') else 'mixed'


def plan_facts(doc, index_am):
    """(plan_ms, exec_ms, cost, structure, workers_requested, workers_launched) of the JSON document of EXPLAIN (ANALYZE, SUMMARY)"""
    plan = doc['Plan']
    gathers = [n for n in plan_nodes(plan) if n.get('Node Type') in ('Gather', 'Gather Merge')]
    return (doc.get('Planning Time'), doc.get('Execution Time'), plan.get('Total Cost'), plan_structure(plan, index_am),
            sum(n.get('Workers Planned', 0) for n in gathers), sum(n.get('Workers Launched', 0) for n in gathers))


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

def one(s, sql):
    _, rows, err, *_ = s.run(sql)
    if err:
        raise RuntimeError('%s: %s' % (sql[:80], err))
    return rows[0].split('|') if rows else []


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


def host_cpus(pid):
    """the cpu quota of the server process PID: its cgroup's cpu.max against its affinity"""
    group = re.search(r'^0::(.*)$', read_text('/proc/%d/cgroup' % pid), re.M)
    quota = read_text('/sys/fs/cgroup%s/cpu.max' % (group.group(1) if group else ''))
    return cpu_quota(quota, len(os.sched_getaffinity(pid)))


def server_env(s):
    """the settings the figures depend on: full_page_writes, shared_buffers, work_mem, the version"""
    fpw, sb, wm, ver = one(s, "SELECT current_setting('full_page_writes'), current_setting('shared_buffers'), current_setting('work_mem'), current_setting('server_version')")
    return dict(fpw=1 if fpw == 'on' else 0, shared_buffers_mb=pg_bytes_mb(sb), work_mem_mb=pg_bytes_mb(wm), pg_version=ver.split()[0])


def index_methods(s):
    """{index name: access method} of the database"""
    return dict(r.split('|') for r in s.run("SELECT c.relname || '|' || am.amname FROM pg_class c JOIN pg_am am ON am.oid = c.relam WHERE c.relkind = 'i'")[1] if r)


def table_facts(s, table, key=None):
    """(allvisible_pct, heap_corr, structures) of a table, measured: pages all-visible, the correlation of KEY with the heap, and the structures it holds (heap, plus the access methods of its indexes)"""
    pages, vis = one(s, "SELECT relpages, relallvisible FROM pg_class WHERE oid = '%s'::regclass" % table)
    corr = one(s, "SELECT correlation FROM pg_stats WHERE schemaname || '.' || tablename IN ('%s', 'public.%s') AND attname = '%s'" % (table, table, key))[0:1] if key else []
    ams = {kind_of(r) for r in s.run("SELECT am.amname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am am ON am.oid = c.relam WHERE i.indrelid = '%s'::regclass" % table)[1] if r}
    structures = ','.join(sorted({'heap'} | {k for k in ams if k != 'other'}))
    return (100.0 * int(vis) / int(pages) if int(pages) else None, float(corr[0]) if corr and corr[0] else None, structures)


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


def wal_lsn(s):
    return int(one(s, "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')::bigint")[0])


def pressure():
    return psi(read_text('/proc/pressure/cpu')), psi(read_text('/proc/pressure/io'))


def count_instructions(s, sql, pid, post):
    """(millions of instructions, millions of cycles) of one run of SQL on the backend PID and the postmaster POST (the workers are its children, forked after perf attached), or
    (None, None) when perf cannot count: the record then has no instructions, never a count of the backend alone"""
    p = subprocess.Popen(['bash', PERF, '%d,%d' % (pid, post)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    time.sleep(1)
    s.run(sql)
    p.send_signal(signal.SIGTERM)
    try:
        out, _ = p.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        return None, None
    f = out.split()
    return (int(f[1]) / 1e6, int(f[0]) / 1e6) if len(f) == 3 and f[0].isdigit() and f[1].isdigit() else (None, None)


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

def measure_statement(s, statement, sql, subject, arms, env, prepare=None, monitor=None, temps=('warm', 'cold')):
    """the records of SQL on the default plan and on each of ARMS: a list of dicts for schema.write. PREPARE (before each repetition of a maintenance or a load) and MONITOR as in the
    module docstring. A harness with several statements calls it once for each."""
    pid = backend_pid(s)
    post = postmaster_pid(pid)
    index_am = index_methods(s)
    srv = server_env(s)
    vis, corr, structures = table_facts(s, subject.table, subject.key)
    base = dict(run_id=env.run_id, tip=env.tip, harness=env.harness, dataset=subject.dataset, rows=subject.rows, keys=subject.keys,
                rows_per_key=(subject.rows / subject.keys) if subject.keys else None, heap_order=subject.heap_order, heap_corr=corr, table_state=subject.table_state,
                allvisible_pct=vis, structures=subject.structures or structures, statement=statement, sql_hash=schema.sql_hash(sql), kind=subject.kind, protocol=subject.protocol,
                cpus=env.cpus, **srv)
    out, default = [], None
    for mode, arm in [('default', Arm('sift', settings=[]))] + [('forced', a) for a in arms]:
        text = arm.sql or sql
        for st in arm.settings:
            s.run(st)
        try:
            rows = arm_rows(s, text, subject, env, base, mode, arm, pid, post, index_am, prepare, monitor, temps)
        finally:
            s.run('RESET ALL')
        if mode == 'default':
            for r in rows:
                r['arm'], r['arm_option'], r['picked'] = r['structure'], subject.default_option, 1
            default = (rows[0]['arm'], subject.default_option)
        else:
            for r in rows:
                r['picked'] = 1 if (r['arm'], r['arm_option']) == default else 0
        out += rows
    return out


def arm_rows(s, sql, subject, env, base, mode, arm, pid, post, index_am, prepare, monitor, temps):
    read = subject.kind == 'read'
    structure, cost, requested = arm.arm, None, 0
    if read:
        s.run(sql)
        _, rows, err, *_ = s.run(EXPLAIN + sql.rstrip().rstrip(';'))
        if err:
            raise RuntimeError('EXPLAIN: %s' % err)
        _, _, cost, structure, requested, _ = plan_facts(json.loads('\n'.join(rows))[0], index_am)
    else:
        requested = int(one(s, "SELECT current_setting('max_parallel_maintenance_workers')")[0])
    out = []
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
                s.run(sql)
            p0 = pressure()
            b0, lsn0 = statio(s), wal_lsn(s)
            launched = 0
            if monitor:
                with WorkerMonitor(monitor) as w:
                    ms = s.run(sql)[0]
                launched = w.most
            else:
                ms = s.run(sql)[0]
            b1, lsn1 = statio(s), wal_lsn(s)
            p1 = pressure()
            plan_ms = exec_ms = instr = cycles = None
            if read:
                _, rows, err, *_ = s.run(EXPLAIN + sql.rstrip().rstrip(';'))
                plan_ms, exec_ms, cost2, structure, requested, launched = plan_facts(json.loads('\n'.join(rows))[0], index_am)
                cost = cost2 if cost2 is not None else cost
            if temp == 'warm' and env.perf:
                instr, cycles = count_instructions(s, sql, pid, post)
            if temp == 'cold':
                plan_ms = exec_ms = None   # the EXPLAIN ran on a warm table after the statement
            r = dict(base, mode=mode, arm=arm.arm, arm_option=arm.option, picked=0, structure=structure, temp=temp, rep=rep, est_cost=cost, plan_ms=plan_ms, exec_ms=exec_ms, wall_ms=ms,
                     instr_m=instr, cycles_m=cycles, instr_scope='cluster' if instr is not None else None, workers_requested=requested, workers_launched=launched,
                     os_cache_dropped=cache_dropped(how) if how else None, wal_bytes=lsn1 - lsn0, psi_cpu_start=p0[0], psi_io_start=p0[1], psi_cpu_end=p1[0], psi_io_end=p1[1])
            r.update(block_deltas(b0, b1))
            out.append(r)
    return out
