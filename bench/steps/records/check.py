#!/usr/bin/env python3
"""The checker of the records: refuses a set that cannot be compared, with the reason and the lines.

    python3 -I check.py FILE...        exit 1 and one line per problem

One comparison is the rows of one statement on one dataset and size (`statement`, `dataset`, `rows`). Within it the checker requires:
  one run         a single run_id: the arms were measured together, under one load, on one build, one table and one set of settings
  one state       the same table state, heap order, keys, visibility and the same server settings (shared_buffers, work_mem, version, cpus, full_page_writes)
  one text        one sql_hash for the statement name, one protocol (a round is not an average of rounds)
  a true heap     a declared heap order is the one measured (sorted: correlation above 0.9, hash: under 0.1); one table (variant) has one heap order
  same settings   one settings_hash (every setting that is not the default) and one maintenance label; a switch that turns the maintenance off (autovacuum, sift.col_auto_merge,
                  sift.async_maintenance, sift.test_*) is refused unless the set is labelled `maintenance disabled`, and the label without a switch is refused
  true arms       a forced arm is the structure its EXPLAIN names, and a heap, b-tree, gin or sift arm is never a mix
  a picked flag   the default plan is in the set for each variant (mode default), and each forced arm says whether it is that plan (same structure and option)
  whole counts    instructions say their scope: a backend count with workers launched is refused, so is a plan whose workers did not all start or a quota under the processes
  a calm host     the host pressure of the arms within `PSI_SPREAD` points of each other; a run above `NOISY_PSI` or with a CV above `NOISY_CV` is marked noisy by the report
  enough runs     MIN_REPS repetitions of every arm and temperature
  separate times  planning and execution apart for a read; cold rows say whether the OS cache was emptied
  one structure   a maintenance or load figure is of a table that holds the arm's structure alone
"""
import os
import statistics
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

MIN_REPS = 3
PSI_SPREAD = 10.0
NOISY_PSI = 10.0
NOISY_CV = 10.0
SAME = ('table_state', 'protocol', 'settings_hash', 'maintenance', 'keys', 'rows_per_key', 'shared_buffers_mb', 'work_mem_mb', 'pg_version', 'cpus', 'fpw', 'kind', 'tip', 'harness')
ALLVISIBLE_SPREAD = 5.0
SAME_VARIANT = ('heap_order',)   # a variant is a table: two tables of a comparison may hold the heap in different orders, one table does not


def where(r):
    return '%s:%s' % (os.path.basename(r.get('_file', '?')), r.get('_line', '?'))


def one_record(r):
    out = []
    for c in schema.REQUIRED:
        if r.get(c) in (None, ''):
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
    if r.get('arm') not in schema.STRUCTURES:
        out.append('%s: arm "%s" is none of %s' % (where(r), r.get('arm'), '/'.join(schema.STRUCTURES)))
    if (r.get('rep') or 0) < 1:
        out.append('%s: rep %s is below 1' % (where(r), r.get('rep')))
    return out


def comparison_problems(key, rs, min_reps, psi_spread):
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
            what = 'MIXED STATE' if c in ('table_state', 'protocol', 'maintenance', 'heap_order', 'keys', 'rows_per_key', 'kind') else 'MIXED SETTINGS'
            out.append('%s: %s: %s is %s at %s and %s at %s' % (name, what, c, a.get(c), where(a), b.get(c), where(b)))
    vis = [r['allvisible_pct'] for r in rs if r.get('allvisible_pct') is not None]
    if vis and max(vis) - min(vis) > ALLVISIBLE_SPREAD:
        out.append('%s: MIXED STATE: all-visible pages from %.1f%% to %.1f%%' % (name, min(vis), max(vis)))
    for variant in sorted({r['variant'] for r in rs}):
        vs = [r for r in rs if r['variant'] == variant]
        label = name + (' (variant %s)' % variant if variant != 'main' or len({r['variant'] for r in rs}) > 1 else '')
        for c in SAME_VARIANT:
            vals = {r.get(c) for r in vs}
            if len(vals) > 1:
                a = vs[0]
                b = next(r for r in vs if r.get(c) != a.get(c))
                out.append('%s: MIXED STATE: %s is %s at %s and %s at %s' % (label, c, a.get(c), where(a), b.get(c), where(b)))
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
        if r['kind'] in ('maintenance', 'load'):
            extra = set(filter(None, (r.get('structures') or '').split(','))) - {'heap', r['arm']}
            if extra:
                out.append('%s: OTHER STRUCTURES at %s: a %s figure of a table that also holds %s' % (name, where(r), r['kind'], ','.join(sorted(extra))))
    cells = defaultdict(list)
    for r in rs:
        cells[(r['variant'], r['mode'], r['arm'], r['arm_option'], r['temp'])].append(r)
    for k, v in sorted(cells.items()):
        if len(v) < min_reps:
            out.append('%s: TOO FEW RUNS: %s has %d repetitions, %d are required' % (name, ' '.join(k), len(v), min_reps))
        if len({r['rep'] for r in v}) != len(v):
            out.append('%s: DUPLICATE REPETITIONS in %s' % (name, ' '.join(k)))
    load = {}
    for k, v in cells.items():
        p = [max(r['psi_cpu_start'] or 0, r['psi_cpu_end'] or 0) for r in v if r.get('psi_cpu_start') is not None or r.get('psi_cpu_end') is not None]
        if p:
            load[k] = statistics.median(p)
    if load and max(load.values()) - min(load.values()) > psi_spread:
        hi, lo = max(load, key=load.get), min(load, key=load.get)
        out.append('%s: DIFFERENT LOAD: host CPU pressure %.1f during %s and %.1f during %s' % (name, load[hi], ' '.join(hi), load[lo], ' '.join(lo)))
    return out


def check(records, min_reps=MIN_REPS, psi_spread=PSI_SPREAD):
    out = []
    if not records:
        return ['NO RECORDS: an empty set proves nothing']
    for r in records:
        out += one_record(r)
    if out:
        return out
    groups = defaultdict(list)
    for r in records:
        groups[(r['statement'], r['dataset'], r['rows'])].append(r)
    for key, rs in sorted(groups.items(), key=lambda kv: str(kv[0])):
        out += comparison_problems(key, rs, min_reps, psi_spread)
    return out


def main(argv):
    if not argv:
        sys.exit(__doc__)
    records = []
    for f in argv:
        records += schema.read(f)
    problems = check(records)
    for p in problems:
        print(p)
    print('records: %d rows, %d problems' % (len(records), len(problems)), file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
