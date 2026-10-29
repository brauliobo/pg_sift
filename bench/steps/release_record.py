#!/usr/bin/env python3
"""The record of the 3.6.0 release pass (bench/results/RELEASE-3.6.0.md): its host and builds, stage times, the 200 cases with their IO, the plans that changed, the cost of the IO runs, host load,
disk writes and what was not measured again, from the frozen raw files of bench/results/release-3.6.0/. Nothing here measures and nothing reads the live results of the next releases, so the record stays what
the pass produced; the files the pass wrote to bench/results are kept at the tag v3.6.0.

    python3 -I bench/steps/release_record.py [--results DIR] [--doc FILE]
"""
import argparse
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extras  # noqa: E402
import readme  # noqa: E402  (replace_block)

REPO = os.path.dirname(os.path.dirname(HERE))
RAW = 'release-3.6.0'
NOT_AGAIN = [  # results files the pass did not write: file, what it holds
    ('cases-wm4g.tsv', 'the cases at work_mem 4GB (the README chart under the table of cases)'),
    ('sets.tsv', 'filtering by a set of ids'), ('rollup.tsv', 'element rollups'), ('tablewrites.tsv', 'sift_table loads, updates, deletes and VACUUM'),
    ('writepath.tsv', 'single-row INSERT loops'), ('bm25size.tsv', 'BM25 index size'), ('fields.tsv', 'field weights and facets'), ('buildcost.tsv', 'build and write cost per operation (the write-cost table)'),
    ('scaling.tsv', 'aggregates by partitions and parallel workers'), ('table-calendar.tsv', 'sift_table calendar columns')]


def raw(results, name):
    return os.path.join(results, RAW, name)


def tsv(path):
    """(comment lines, rows as dicts) of a file with # lines and a header line"""
    lines = [l.rstrip('\n') for l in open(path) if l.strip()]
    rows = [l for l in lines if not l.startswith('#')]
    return [l[2:] for l in lines if l.startswith('#')], list(csv.DictReader(rows, delimiter='\t'))


def cell(v):
    return str(v).replace('|', '\\|') if v not in ('', None) else '-'


def table(head, body, align=None):
    return extras.table(head, [[cell(c) for c in r] for r in body], align)


def fig(rows, case, arm, metric):
    r = extras.get(rows, case=case, arm=arm, metric=metric)
    return float(r['value']) if r and r['value'] not in ('', 'NA', 'error') else None


def num(v, spec='%.1f'):
    return '-' if v is None else extras.ms(v) if spec == 'ms' else spec % v


def host_lines(results):
    meta, rows = extras.read(raw(results, 'cases.tsv'))
    first = extras.read(raw(results, 'cases-pass1.tsv'))[0]
    store = json.load(open(os.path.join(results, 'store.json')))['meta']['host']
    upd = json.load(open(os.path.join(results, 'updates.json')))['meta']
    rows_ = [('measured', meta['date']), ('hardware', '%s; kernel %s; Docker %s; governor %s' % (meta['cpu'], store['kernel'], store['docker'], store['governor'])),
             ('PostgreSQL', meta['postgresql']), ('pg_sift', meta['pg_sift']),
             ('build of the final run of the cases', meta['build']), ('build of the first run of the cases', first['build']),
             ('build of the stages run before the fix of the cold run (updates, pg_duckdb; the pass was pinned at it)', upd['sha']), ('build of the cases by size', extras.read(os.path.join(results, 'cases-scale.tsv'))[0]['build']), ('orders', '{:,}'.format(int(meta['orders']))), ('work_mem', meta['work_mem']), ('method of the cases', meta['method']),
             ('IO', meta['io_cold'])]
    return ['| item | value |', '|---|---|'] + ['| %s | %s |' % (k, cell(v)) for k, v in rows_]


def clock(t):
    h, m, sec = (int(x) for x in t.split(':'))
    return h * 3600 + m * 60 + sec


def stage_lines(results):
    def text(name):
        return open(raw(results, name)).read().rstrip('\n').split('\n')
    steps, stages = text('steps.txt'), text('final-stages.txt')
    start, done = steps[0].split()[0], next(l for l in steps if 'ALL DONE' in l).split()[0]
    wall = clock(done) - clock(start)
    return (['The pass took %d minutes from `%s` to `ALL DONE` (%s to %s local time, UTC-3). The steps, as the driver wrote them (`bench final` first; its cases stage stopped on the cases whose plan was not the expected one, '
             'which ran again with `--any-plan`; then the sizes, the merge of the slopes and pg_duckdb at 10,000,000 orders; after `ALL DONE` the cases ran again on the second build for the cold rows of every lane, and '
             'the next morning the headline cases by size):' % (wall // 60, steps[0].split(' ', 1)[1], start, done), '', '```'] + steps + ['```', '',
             'The stage log of `bench final`: the plan of the budget, the minutes each stage took, and the lines of the stages that the summary does not list:', '', '```'] + stages + ['```'])


def case_lines(results):
    meta, rows = extras.read(raw(results, 'cases.tsv'))
    ids = list(dict.fromkeys(r['case'] for r in rows))
    body = []
    for c in ids:
        unit = 'ms'
        n, s = fig(rows, c, 'native', unit), fig(rows, c, 'sift', unit)
        plan = (extras.get(rows, case=c, arm='sift', metric=unit) or {'note': ''})['note'].replace(' NOISY', '')
        body.append(['`%s`' % c, num(n, 'ms'), num(s, 'ms'), num(fig(rows, c, 'native', 'io_cold_read_mb')), num(fig(rows, c, 'sift', 'io_cold_read_mb')),
                     '%s / %s' % (num(fig(rows, c, 'native', 'io_warm_read_blocks'), '%d'), num(fig(rows, c, 'sift', 'io_warm_read_blocks'), '%d')),
                     '%s / %s' % (num(fig(rows, c, 'native', 'io_cold_hit_pct')), num(fig(rows, c, 'sift', 'io_cold_hit_pct'))), '`%s`' % plan[:70].replace('|', '\\|') if plan else '-'])
    head = ['case', 'PostgreSQL ms', 'pg_sift ms', 'PostgreSQL MB cold', 'pg_sift MB cold', 'blocks read warm', 'hit % cold', 'plan of pg_sift']
    noisy = sorted({r['case'] for r in rows if 'NOISY' in (r['note'] or '')})
    return ['%d cases, %s orders, build %s; `ms` is the median of the interleaved runs, `MB cold` what the first run after the tables were emptied from the caches read (`pg_stat_io`), `blocks read warm` the shared reads of the next run (`PostgreSQL / pg_sift`), '
            '`hit %% cold` the share of the blocks of that first run found in shared buffers. The load cases (`bulk_copy_wal` and the like) have no cold read: their IO is the file growth and the writes in the results file. '
            'A case whose plan is PostgreSQL\'s shows it in the last column. Rows measured while the host was busy: %s.' % (
                len(ids), '{:,}'.format(int(meta['orders'])), meta['build'], ', '.join(noisy) or 'none'), ''] + table(head, body, ['---'] + ['---:'] * 6 + ['---'])


def triage_lines(results):
    meta, rows = extras.read(raw(results, 'cases.tsv'))
    notes, tri = tsv(raw(results, 'triage.tsv'))
    body = []
    for t in tri:
        c = t['case']
        pin = '%s ms, %s MB cold; PostgreSQL %s ms, %s MB' % (num(fig(rows, c, 'sift', 'ms'), 'ms'), num(fig(rows, c, 'sift', 'io_cold_read_mb')), num(fig(rows, c, 'native', 'ms'), 'ms'), num(fig(rows, c, 'native', 'io_cold_read_mb')))
        before = '`%s`: %s ms%s' % (t['before_ref'], t['before_ms'], ', %s MB' % t['before_cold_mb'] if t['before_cold_mb'] else '') if t['before_ms'] else '-'
        flip = '`%s`: %s ms, %s MB (PostgreSQL %s ms)' % (t['flip_ref'], t['flip_ms'], t['flip_cold_mb'], t['flip_pg_ms']) if t['flip_ms'] else '-'
        body.append(['`%s`' % c, t['verdict'], t['flipped_by'], before, flip, pin, t['cause']])
    head = ['case', 'verdict', 'changed by', 'before (pg_sift)', 'at the change', 'pinned build %s' % meta['build'], 'cause']
    return [' '.join(notes), ''] + table(head, body)


def io_lines(results):
    times = [l.split() for l in open(raw(results, 'io1m-times.txt')) if l.split()[0] in ('cold', 'off')]
    mean = lambda m: sum(float(t[1]) for t in times if t[0] == m) / sum(1 for t in times if t[0] == m)  # noqa: E731
    cases = {r['case'] for r in tsv(raw(results, 'io1m-cold-1.tsv'))[1]}
    d = mean('cold') - mean('off')
    return ['One run of %d cases at 1,000,000 orders (2 rounds of 3 runs) twice with `--io cold` and twice with `--io off`, before the pass: %.2f s against %.2f s, so the IO runs cost %.2f s a case '
            '(%d%% of the run).' % (len(cases), mean('cold'), mean('off'), d / len(cases), round(100 * d / mean('off'))), '',
            '| mode | seconds of the four runs | mean |', '|---|---|---:|'] + ['| `--io %s` | %s | %.2f |' % (m, ', '.join(t[1][:5] for t in times if t[0] == m), mean(m)) for m in ('cold', 'off')]


def load_lines(results):
    _, rows = tsv(raw(results, 'load.tsv'))
    return table(['stage', 'part', 'when', 'load at the start', 'load at the end', 'pressure (CPU and IO)', 'noisy', 'note'], [[r[k] for k in ('stage', 'part', 'when', 'load_start', 'load_end', 'pressure', 'noisy', 'note')] for r in rows])


def disk_lines(results):
    notes, rows = tsv(raw(results, 'disk-growth.tsv'))
    return ['%d samples, a minute apart: the data volume grew by at most %d MB and the root volume by at most %d MB during the pass; %s' % (
        len(rows), max(int(r['data_volume_mb']) for r in rows), max(int(r['root_mb']) for r in rows), notes[1][0].lower() + notes[1][1:])]


def not_again_lines(results):
    body = []
    for name, what in NOT_AGAIN:
        meta = extras.read(os.path.join(results, name))[0]
        body.append(['`%s`' % name, what, meta.get('pg_sift', '-'), meta.get('build', '-')[:40], meta.get('date', '-')])
    return table(['file', 'what it holds', 'pg_sift', 'build', 'measured'], body)


BLOCKS = {'release-host': host_lines, 'release-stages': stage_lines, 'release-cases': case_lines, 'release-triage': triage_lines, 'release-io': io_lines, 'release-load': load_lines,
          'release-disk': disk_lines, 'release-not-again': not_again_lines}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--doc')
    a = ap.parse_args()
    doc = a.doc or os.path.join(a.results, 'RELEASE-3.6.0.md')
    if not os.path.exists(doc):
        return
    for name, render in BLOCKS.items():
        readme.replace_block(doc, name, render(a.results))


if __name__ == '__main__':
    main()
