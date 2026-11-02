#!/usr/bin/env python3
"""Tables generated from records (check.py must pass: a set it refuses has no table).

    python3 -I report.py [--tsv] FILE...

One table for each comparison (statement, dataset, rows) with the run, the table state and the settings above it: a row for each arm and for the plan the planner chooses (marked `*`),
absolute figures only: planning, execution and whole-statement ms, millions of instructions (workers included), blocks of the heap and of each kind of index, the planner's cost and
the microseconds the execution took for each unit of it. Then the cold rows: the blocks read after the shared buffers were evicted and whether the OS cache was too.
A median, the CV of the runs and a `noisy` mark where it is above NOISY_CV or the host pressure above NOISY_PSI.
"""
import os
import statistics
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402
import schema  # noqa: E402


def med(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def cv(vals):
    vals = [v for v in vals if v is not None]
    return 100 * statistics.pstdev(vals) / statistics.mean(vals) if len(vals) > 1 and statistics.mean(vals) else 0.0


def fmt(v, digits=2):
    if v is None:
        return '-'
    if abs(v) >= 1000:
        return format(round(v), ',')
    text = '%.*f' % (digits, v)
    return text.rstrip('0').rstrip('.') if '.' in text else text


def blocks(rs, kinds, parts=('hit', 'read')):
    per = [sum(r.get('%s_%s' % (k, p)) or 0 for k in kinds for p in parts) if any(r.get('%s_%s' % (k, p)) is not None for k in kinds for p in parts) else None for r in rs]
    return med(per)


def cells(rs):
    out = defaultdict(list)
    for r in rs:
        if r['mode'] != 'unavailable':
            out[(r['variant'], r['mode'], r['arm'], r['arm_option'], r['temp'])].append(r)
    return out


def rows_of(rs, temp):
    order = sorted((k for k in cells(rs) if k[4] == temp), key=lambda k: (k[0], k[1] != 'default', k[2], k[3]))
    return [(k, cells(rs)[k]) for k in order]


def label(k, rs):
    variant, mode, arm, option = k[:4]
    mark = '*' if rs[0].get('picked') == 1 else ''
    text = 'default plan: %s' % arm if mode == 'default' else '%s%s' % (arm, '' if option == 'default' else ' ' + option)
    named = ' (%s)' % rs[0]['structure'] if mode == 'default' and rs[0]['structure'] != arm else ''
    return ('' if variant == 'main' else '%s: ' % variant) + text + mark + named


def warm_table(rs):
    head = ['arm', 'plan ms', 'exec ms', 'wall ms', 'Minstr', 'heap blk', 'btree blk', 'gin blk', 'sift blk', 'cost', 'us/cost', 'n', 'CV %', 'noisy']
    out = [head]
    for k, v in rows_of(rs, 'warm'):
        ex, cost = med([r.get('exec_ms') for r in v]), med([r.get('est_cost') for r in v])
        noisy = cv([r['wall_ms'] for r in v]) > check.NOISY_CV or any(r.get('noise') for r in v) or max([max(r.get('psi_cpu_start') or 0, r.get('psi_cpu_end') or 0) for r in v]) > check.NOISY_PSI
        out.append([label(k, v), fmt(med([r.get('plan_ms') for r in v]), 3), fmt(ex, 3), fmt(med([r['wall_ms'] for r in v]), 3), fmt(med([r.get('instr_m') for r in v]), 1),
                    fmt(blocks(v, ['heap'])), fmt(blocks(v, ['btree'])), fmt(blocks(v, ['gin'])), fmt(blocks(v, ['sift'])), fmt(cost, 1), fmt(1000 * ex / cost if ex and cost else None, 3),
                    str(len(v)), fmt(cv([r['wall_ms'] for r in v]), 1), 'noisy' if noisy else ''])
    return out


def cold_table(rs):
    head = ['arm', 'wall ms', 'heap read', 'btree read', 'gin read', 'sift read', 'OS cache dropped', 'n']
    out = [head]
    for k, v in rows_of(rs, 'cold'):
        dropped = {r.get('os_cache_dropped') for r in v}
        out.append([label(k, v), fmt(med([r['wall_ms'] for r in v]), 3), fmt(blocks(v, ['heap'], ('read',))), fmt(blocks(v, ['btree'], ('read',))), fmt(blocks(v, ['gin'], ('read',))),
                    fmt(blocks(v, ['sift'], ('read',))), 'yes' if dropped == {1} else 'no, shared buffers only' if dropped == {0} else 'mixed', str(len(v))])
    return out if len(out) > 1 else []


def table_md(t):
    if not t:
        return ''
    w = ['| ' + ' | '.join(row) + ' |' for row in t]
    return '\n'.join([w[0], '|' + '|'.join(['---'] + ['--:'] * (len(t[0]) - 1)) + '|'] + w[1:]) + '\n'


def report(records, tsv=False):
    problems = check.check(records)
    if problems:
        raise ValueError('the records are refused:\n' + '\n'.join(problems))
    groups = defaultdict(list)
    for r in records:
        groups[(r['statement'], r['dataset'], r['rows'])].append(r)
    out = []
    for key, rs in sorted(groups.items(), key=lambda kv: str(kv[0])):
        a = rs[0]
        extra = ', settings %s%s%s' % (a['settings_hash'], (' (%s)' % a['settings'].replace(';', ', ')) if a.get('settings') else ' (all default)',
                                      ', MAINTENANCE DISABLED' if a['maintenance'] == 'disabled' else '') + \
            (', NOT VACUUMED AFTER THE CHURN (every arm reads the table as it is: %s%% of its pages all-visible, so an index-only scan fetches heap rows)' % fmt(a.get('allvisible_pct'), 0) if a['table_state'] == 'churned_unvacuumed' else '')
        warn = [w for w in check.load_warnings(rs)]
        gone = ''.join('\n\nWARNING: measured under different host load: ' + w for w in warn) + ''.join('\n\nno plan for %s%s: the plans name %s' % ('' if r['variant'] == 'main' else r['variant'] + ': ', r['arm'] + ('' if r['arm_option'] == 'default' else ' ' + r['arm_option']), r['structure'])
                       for r in sorted((x for x in rs if x['mode'] == 'unavailable'), key=lambda x: (x['variant'], x['arm'], x['arm_option'])))
        for title, t in (('warm', warm_table(rs)), ('cold', cold_table(rs))):
            if not t:
                continue
            if tsv:
                out += ['\t'.join([a['run_id'], a['statement'], a['dataset'], str(a['rows']), title] + row) for row in t[1:]]
            else:
                out.append('**%s**, %s, %s rows, %s: run %s, %s, heap %s, %s%% all-visible, shared_buffers %s MB, work_mem %s MB, %s cpus, PostgreSQL %s%s\n\n%s' % (
                    a['statement'], a['dataset'], format(a['rows'], ','), title, a['run_id'], a['table_state'].replace('_', ' '), a['heap_order'], fmt(a.get('allvisible_pct'), 0), fmt(a['shared_buffers_mb'], 0),
                    fmt(a['work_mem_mb'], 0), fmt(a['cpus'], 1), a['pg_version'], extra, table_md(t) + (gone if title == 'warm' else '')))
    return '\n'.join(out) + ('\n' if tsv else '')


def main(argv):
    tsv = '--tsv' in argv
    files = [a for a in argv if a != '--tsv']
    if not files:
        sys.exit(__doc__)
    records = []
    records += schema.read_files(files, say=lambda m: print('note: ' + m, file=sys.stderr))
    try:
        print(report(records, tsv), end='')
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
