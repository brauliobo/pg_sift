#!/usr/bin/env python3
"""The checker of the records: refuses a set that cannot be compared, with the reason and the lines.

    python3 -I check.py FILE...        exit 1 and one line per problem

One comparison is the rows of one statement on one dataset and size (`statement`, `dataset`, `rows`). Within it the checker requires:
  one run         a single run_id: the arms were measured together, under one load, on one build, one table and one set of settings
  one state       the same table state and churn history (updates, inserts, deletes, rounds, VACUUMs and autovacuums on record, settled), heap order, keys, visibility, dead tuples within DEAD_SPREAD points and the same server settings (shared_buffers, work_mem, version, cpus, full_page_writes)
  one text        one sql_hash for the statement name, one protocol (a round is not an average of rounds)
  a true heap     a declared heap order is the one measured (sorted: correlation above 0.9, hash: under 0.1); one table (variant) has one heap order
  same settings   one settings_hash (every setting that is not the default) and one maintenance label; a switch that turns the maintenance off (autovacuum, sift.col_auto_merge,
                  sift.async_maintenance, sift.test_*) is refused unless the set is labelled `maintenance disabled`, and the label without a switch is refused
  true arms       a forced arm is the structure its EXPLAIN names, and a heap, b-tree, gin or sift arm is never a mix
  a picked flag   the default plan is in the set for each variant (mode default), and each forced arm says whether it is that plan (same structure and option); a maintenance or a load has no plan to pick: forced, picked 0
  instructions    every warm row has a count
  a true churn    a churned read says what was applied; 'settled' needs a VACUUM or a pass on record
  whole counts    instructions say their scope: a backend count with workers launched is refused, so is a plan whose workers did not all start or a quota under the processes
  a noisy host    never a refusal (the load of the host is a fact about the run, not about the data): a record whose host pressure was above `NOISY_PSI` carries noise = 1, a cell above it or with a CV
                  above `NOISY_CV` is marked noisy by the report, which warns when the arms of a comparison were measured under pressure more than `PSI_SPREAD` points apart
  enough runs     MIN_REPS repetitions of every arm and temperature
  separate times  planning and execution apart for a read; cold rows say whether the OS cache was emptied
  one structure   a maintenance, load or build figure is of a table that holds the arm's structure alone
  true segments   a sift arm that has index bytes has its segment, immutable and write buffer counts and its option set (schema 6 on); a count the case declares is the one measured
  a true build    a build has index bytes and WAL, the instructions of its leader and of its workers (their sum the cluster count) and was measured on a cluster with no sift.test_* setting
"""
import os
import re
import statistics
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

MIN_REPS = 3
PSI_SPREAD = 10.0   # points of host pressure between the arms of a comparison: the report warns, nothing is refused
NOISY_PSI = 10.0
NOISY_CV = 10.0
SAME = ('table_state', 'churn_history', 'vacuums', 'autovacuums', 'settled', 'settings_hash', 'maintenance', 'keys', 'rows_per_key', 'shared_buffers_mb', 'work_mem_mb', 'pg_version', 'cpus', 'fpw', 'kind', 'tip', 'harness')
ALLVISIBLE_SPREAD = 5.0
DEAD_SPREAD = 5.0   # points of dead tuples between the tables of one comparison
SAME_VARIANT = ('heap_order',)   # a variant is a table: two tables of a comparison may hold the heap in different orders, one table does not


def where(r):
    return '%s:%s' % (os.path.basename(r.get('_file', '?')), r.get('_line', '?'))


def one_record(r):
    out = []
    for c in schema.REQUIRED:
        if r.get(c) in (None, '') and not (c == 'wall_ms' and r.get('mode') == 'unavailable'):
            out.append('%s: %s is empty' % (where(r), c))
    for c, allowed in (('table_state', schema.STATES), ('heap_order', schema.ORDERS), ('kind', schema.KINDS), ('mode', schema.MODES), ('temp', schema.TEMPS), ('structure', schema.STRUCTURES + ('mixed',)), ('instr_scope', schema.SCOPES)):
        if r.get(c) not in (None,) + allowed:
            out.append('%s: %s "%s" is none of %s' % (where(r), c, r.get(c), '/'.join(allowed)))
    if r.get('maintenance') not in (None,) + schema.MAINTENANCE:
        out.append('%s: maintenance "%s" is none of %s' % (where(r), r.get('maintenance'), '/'.join(schema.MAINTENANCE)))
    pairs = dict(kv.split('=', 1) for kv in (r.get('settings') or '').split(';') if '=' in kv)
    switches = schema.maintenance_switches(pairs)
    if switches and r.get('maintenance') == 'default':
        out.append('%s: TEST SETTINGS NOT LABELLED: %s turns the maintenance off, so the state measured may not exist with the defaults; label the set maintenance disabled' % (where(r), ', '.join(switches)))
    if not switches and r.get('maintenance') == 'disabled':
        out.append('%s: LABELLED DISABLED WITHOUT A SWITCH: no setting turns the maintenance off' % where(r))
    if r.get('arm') not in schema.STRUCTURES + (('mixed',) if r.get('mode') == 'default' else ()):
        out.append('%s: arm "%s" is none of %s' % (where(r), r.get('arm'), '/'.join(schema.STRUCTURES)))
    churned = (r.get('table_state') or '').startswith('churned')
    if churned and r.get('kind') == 'read' and not r.get('churn_history'):
        out.append('%s: CHURN HISTORY MISSING: table state %s with no churn_history (what was applied, or the tuples the statistics counted)' % (where(r), r.get('table_state')))
    if r.get('settled') == 1 and not ((r.get('vacuums') or 0) + (r.get('autovacuums') or 0) or r.get('settled_s') is not None):
        out.append('%s: SETTLED WITHOUT A VACUUM: the harness waited for the maintenance to settle but no VACUUM, autovacuum or pass is on record' % where(r))
    if (r.get('rep') or 0) < 1:
        out.append('%s: rep %s is below 1' % (where(r), r.get('rep')))
    return out


SPLIT_TOLERANCE = 0.01   # leader + workers against the cluster count: two counts of two runs of one statement, within this share


def segment_problems(name, r):
    """the facts of a sift index and of a build in a row of schema 6 or later (an older row has none and is not refused for it): a sift arm that has index bytes has its segments, the kinds
    add up, a count the harness declared is the count measured, and a build says what its leader and its workers executed on a cluster that runs no test setting"""
    if r.get('_schema', schema.VERSION) < 6 or r['mode'] == 'unavailable':
        return []
    out = []
    at = where(r)
    segments, immutable, buffer = (r.get(c) for c in ('segments', 'immutable_segments', 'buffer_segments'))
    if r['arm'] == 'sift' and r.get('index_bytes') is not None and None in (segments, immutable, buffer, r.get('index_options')):
        out.append('%s: SEGMENTS MISSING at %s: a sift arm with index bytes has no segment count, immutable count, write buffer count or option set' % (name, at))
    if None not in (segments, immutable, buffer) and immutable + buffer > segments:
        out.append('%s: SEGMENT KINDS at %s: %d immutable and %d write buffer segments of %d' % (name, at, immutable, buffer, segments))
    if r.get('segments_declared') is not None and r['segments_declared'] != segments:
        out.append('%s: SEGMENTS CONTRADICT at %s: the case says %d, the index directory holds %s' % (name, at, r['segments_declared'], segments))
    if r['kind'] != 'build':
        return out
    for c in ('index_bytes', 'wal_bytes'):
        if r.get(c) is None:
            out.append('%s: BUILD WITHOUT %s at %s' % (name, c, at))
    test = [k for k in (r.get('settings') or '').split(';') if k.startswith('sift.test_')]
    if test:
        out.append('%s: BUILD ON TEST SETTINGS at %s: %s change what a build does; a build is measured on the defaults' % (name, at, ', '.join(test)))
    if r['temp'] == 'warm':
        leader, workers, total = (r.get(c) for c in ('instr_leader_m', 'instr_workers_m', 'instr_m'))
        if None in (leader, workers, total):
            out.append('%s: BUILD WITHOUT SPLIT at %s: the instructions of the leader and of the workers are both required' % (name, at))
        elif abs(leader + workers - total) > SPLIT_TOLERANCE * total:
            out.append('%s: BUILD SPLIT at %s: leader %g + workers %g is not the %g of the cluster' % (name, at, leader, workers, total))
        elif (r.get('workers_launched') or 0) == 0 and workers > SPLIT_TOLERANCE * total:
            out.append('%s: BUILD WORKERS at %s: no worker launched but the cluster executed %g M beyond the leader' % (name, at, workers))
    return out


def comparison_problems(key, rs, min_reps):
    name = '%s on %s, %s rows' % (key[0], key[1], format(key[2], ',') if isinstance(key[2], int) else key[2])
    out = []
    runs = sorted({r['run_id'] for r in rs})
    if len(runs) > 1:
        out.append('%s: MIXED RUNS %s: the arms were not measured together (lines %s)' % (name, ' and '.join(runs[:3]), ', '.join(where(r) for r in rs[:1] + [x for x in rs if x['run_id'] != rs[0]['run_id']][:1])))
    if len({r['sql_hash'] for r in rs}) > 1:
        a = rs[0]
        b = next(r for r in rs if r['sql_hash'] != a['sql_hash'])
        out.append('%s: TWO STATEMENTS under one name: %s has hash %s, %s has %s' % (name, where(a), a['sql_hash'], where(b), b['sql_hash']))
    for c in SAME:
        vals = {r.get(c) for r in rs}
        if len(vals) > 1:
            a = rs[0]
            b = next(r for r in rs if r.get(c) != a.get(c))
            what = 'MIXED STATE' if c in ('table_state', 'churn_history', 'vacuums', 'autovacuums', 'settled', 'protocol', 'maintenance', 'heap_order', 'keys', 'rows_per_key', 'kind') else 'MIXED SETTINGS'
            out.append('%s: %s: %s is %s at %s and %s at %s' % (name, what, c, a.get(c), where(a), b.get(c), where(b)))
    gone = [r for r in rs if r['mode'] == 'unavailable']
    rs = [r for r in rs if r['mode'] != 'unavailable']
    if not rs:
        return out + ['%s: NO ARM MEASURED: every row is unavailable' % name]
    for r in gone:
        if any(r['arm'] == x['arm'] and r['arm_option'] == x['arm_option'] and r['variant'] == x['variant'] and x['mode'] == 'forced' for x in rs):
            out.append('%s: UNAVAILABLE ARM %s %s at %s is also measured' % (name, r['arm'], r['arm_option'], where(r)))
    protocols = {r['protocol'] for r in rs}
    if len(protocols - {'single', 'fresh backend'}) > 1 or (protocols - {'single', 'fresh backend'} and protocols & {'single', 'fresh backend'}):   # a new backend and the second statement of a kept one are compared on purpose; rounds are not
        a = rs[0]
        b = next(r for r in rs if r['protocol'] != a['protocol'])
        out.append('%s: MIXED STATE: protocol is %s at %s and %s at %s' % (name, a['protocol'], where(a), b['protocol'], where(b)))
    vis = [r['allvisible_pct'] for r in rs if r.get('allvisible_pct') is not None]
    if vis and max(vis) - min(vis) > ALLVISIBLE_SPREAD:
        out.append('%s: MIXED STATE: all-visible pages from %.1f%% to %.1f%%' % (name, min(vis), max(vis)))
    for col, label in (('dead_pct', 'dead tuples'), ('tombstone_pct', 'tombstones')):
        vals = [r[col] for r in rs if r.get(col) is not None]
        if vals and max(vals) - min(vals) > DEAD_SPREAD:
            out.append('%s: MIXED STATE: %s from %.1f%% to %.1f%% of the rows' % (name, label, min(vals), max(vals)))
    for variant in sorted({r['variant'] for r in rs}):
        vs = [r for r in rs if r['variant'] == variant]
        label = name + (' (variant %s)' % variant if variant != 'main' or len({r['variant'] for r in rs}) > 1 else '')
        for c in SAME_VARIANT:
            vals = {r.get(c) for r in vs}
            if len(vals) > 1:
                a = vs[0]
                b = next(r for r in vs if r.get(c) != a.get(c))
                out.append('%s: MIXED STATE: %s is %s at %s and %s at %s' % (label, c, a.get(c), where(a), b.get(c), where(b)))
        if vs[0]['kind'] != 'read':
            for r in vs:
                if r['mode'] != 'forced' or r.get('picked') != 0:
                    out.append('%s: PICKED FLAG of %s: a %s has no plan the planner chooses, so its rows are mode forced and picked 0' % (label, where(r), r['kind']))
            continue
        default = [r for r in vs if r['mode'] == 'default']
        if not default:
            out.append('%s: NO PICKED: no row of mode default, so which arm the planner chooses is not recorded' % label)
            continue
        d = default[0]
        for r in default:
            if r['arm'] != r['structure']:
                out.append('%s: DEFAULT ROW %s names arm %s but its plan is %s' % (label, where(r), r['arm'], r['structure']))
            if r.get('picked') != 1:
                out.append('%s: DEFAULT ROW %s has picked = %s, not 1' % (label, where(r), r.get('picked')))
        for r in vs:
            if r['mode'] == 'forced':
                want = 1 if (r['arm'], r['arm_option']) == (d['arm'], d['arm_option']) else 0
                if r.get('picked') != want:
                    out.append('%s: PICKED FLAG of %s is %s but the default plan is %s %s' % (label, where(r), r.get('picked'), d['arm'], d['arm_option']))
    for r in rs:
        corr = r.get('heap_corr')
        if corr is not None and ((r['heap_order'] == 'sorted' and abs(corr) < 0.9) or (r['heap_order'] == 'hash' and abs(corr) > 0.1)):
            out.append('%s: HEAP ORDER NOT AS DECLARED at %s: %s, measured correlation %.2f' % (name, where(r), r['heap_order'], corr))
        if r['mode'] == 'forced' and r['arm'] != r['structure']:
            out.append('%s: WRONG ARM at %s: arm %s but the EXPLAIN names %s' % (name, where(r), r['arm'], r['structure']))
        if r['temp'] == 'warm' and r.get('instr_m') is None:
            out.append('%s: INSTRUCTIONS MISSING at %s: a warm row has no instruction count (perf counters unavailable: the collector raises PerfUnavailable and writes no such row; a harness of its own or an older library left it)' % (name, where(r)))
        if r.get('instr_m') is not None:
            if not r.get('instr_scope'):
                out.append('%s: INSTRUCTIONS WITHOUT SCOPE at %s' % (name, where(r)))
            elif r['instr_scope'] == 'backend' and (r.get('workers_launched') or 0) > 0:
                out.append('%s: INSTRUCTIONS WITHOUT WORKERS at %s: %d workers launched, the count is of the backend alone' % (name, where(r), r['workers_launched']))
        if (r.get('workers_launched') or 0) < (r.get('workers_requested') or 0):
            out.append('%s: WORKERS SHORT at %s: %d requested, %d launched' % (name, where(r), r['workers_requested'], r['workers_launched']))
        if (r.get('workers_launched') or 0) > 0 and (r.get('cpus') or 0) < r['workers_launched'] + 1:
            out.append('%s: QUOTA UNDER THE PROCESSES at %s: %s cpus for %d workers and the backend' % (name, where(r), r.get('cpus'), r['workers_launched']))
        if r['kind'] == 'read' and r['temp'] == 'warm' and (r.get('plan_ms') is None or r.get('exec_ms') is None):
            out.append('%s: PLANNING NOT SEPARATE at %s: plan_ms and exec_ms are both required for a read' % (name, where(r)))
        if r['temp'] == 'cold' and r.get('os_cache_dropped') is None:
            out.append('%s: COLD WITHOUT os_cache_dropped at %s' % (name, where(r)))
        out += segment_problems(name, r)
        if r['kind'] in schema.WRITES:
            extra = set(filter(None, (r.get('structures') or '').split(','))) - {'heap', 'constraint', r['arm']}   # `constraint`: the indexes of a primary key or a unique constraint, the table's definition
            if extra:
                out.append('%s: OTHER STRUCTURES at %s: a %s figure of a table that also holds %s' % (name, where(r), r['kind'], ','.join(sorted(extra))))
    writes = {'constraint' in (r.get('structures') or '').split(',') for r in rs if r['kind'] in schema.WRITES}
    if len(writes) > 1:
        out.append('%s: COMPANION DIFFERS: some write figures of the comparison are of a table with a constraint index (primary key, unique) and some are of a table without: the arms differ in more than their structure' % name)
    cells = defaultdict(list)
    for r in rs:
        cells[(r['variant'], r['mode'], r['arm'], r['arm_option'], r['temp'])].append(r)
    for k, v in sorted(cells.items()):
        if len(v) < min_reps:
            out.append('%s: TOO FEW RUNS: %s has %d repetitions, %d are required' % (name, ' '.join(k), len(v), min_reps))
        if len({r['rep'] for r in v}) != len(v):
            out.append('%s: DUPLICATE REPETITIONS in %s' % (name, ' '.join(k)))
    return out


def check(records, min_reps=MIN_REPS):
    out = []
    if not records:
        return ['NO RECORDS: an empty set proves nothing']
    for r in records:
        out += one_record(r)
    for tip in sorted({r.get('tip') for r in records if r.get('tip') and not re.fullmatch('[0-9a-f]{12}', r['tip'])}):
        out.append('TIP UNKNOWN: the revision "%s" is not a commit (12 hexadecimal digits): the code measured cannot be found again' % tip)
    if out:
        return out
    groups = defaultdict(list)
    for r in records:
        groups[(r['statement'], r['dataset'], r['rows'])].append(r)
    for key, rs in sorted(groups.items(), key=lambda kv: str(kv[0])):
        out += comparison_problems(key, rs, min_reps)
    return out


def main(argv):
    if not argv:
        sys.exit(__doc__)
    records = []
    records += schema.read_files(argv, say=lambda m: print('note: ' + m, file=sys.stderr))
    problems = check(records)
    for p in problems:
        print(p)
    print('records: %d rows, %d problems' % (len(records), len(problems)), file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))


def load_warnings(records):
    """a line for each comparison whose arms were measured under host CPU pressure more than PSI_SPREAD points apart: a warning of the report, never a refusal"""
    groups = defaultdict(lambda: defaultdict(list))
    for r in records:
        if r['mode'] != 'unavailable' and (r.get('psi_cpu_start') is not None or r.get('psi_cpu_end') is not None):
            groups[(r['statement'], r['dataset'], r['rows'])][(r['variant'], r['mode'], r['arm'], r['arm_option'], r['temp'])].append(max(r.get('psi_cpu_start') or 0, r.get('psi_cpu_end') or 0))
    out = []
    for key, cells in sorted(groups.items(), key=lambda kv: str(kv[0])):
        load = {k: statistics.median(v) for k, v in cells.items()}
        hi, lo = max(load, key=load.get), min(load, key=load.get)
        if load[hi] - load[lo] > PSI_SPREAD:
            out.append('%s on %s, %s rows: host CPU pressure %.1f during %s and %.1f during %s' % (key[0], key[1], format(key[2], ',') if isinstance(key[2], int) else key[2], load[hi], ' '.join(hi), load[lo], ' '.join(lo)))
    return out
