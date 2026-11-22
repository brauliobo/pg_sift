#!/usr/bin/env python3
"""The tables of the scope index against tuned stock PostgreSQL, from the one results file of steps/scope_stock_arms.py.

    python3 -I bench/steps/scope_stock_tables.py render [--results FILE] [--root DIR]   rewrite the generated blocks of docs/BENCHMARKS.md
    python3 -I bench/steps/scope_stock_tables.py report [FILE]                          print every table (the full set, all arms and tenants)

No figure of those pages is typed: a block is a function of the results file, and scripts/gen-docs.py --check fails when a block differs from it. The file is
the format of steps/resultfile.py (`# key: value` lines, then version, case, arm, metric, value, min, max, n, load, note); a cell of a read is the case `tTENANT:CLASS`
with the metrics warm_ms, warm_minstr, warm_hit, cold_ms, cold_minstr, cold_hit, cold_read and md5_equal. A rival wins a cell when it is faster than the scope index
by more than 0.1 ms and by more than 10%; within either margin the cell is a tie.
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import resultfile  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
RESULTS = os.path.join(REPO, 'docs', 'results', 'scope-stock.tsv')
BASE = 'sift_scope'
TIE_MS, TIE_PCT = 0.1, 0.10
BLOCKS = {  # block name: (page, renderer)
    'scope-stock-tally': ('docs/BENCHMARKS.md', lambda R: tally(R) + ['', tally_note(R)]),
    'scope-stock-geomean': ('docs/BENCHMARKS.md', lambda R: geomean_table(R)),
    'scope-stock-costs': ('docs/BENCHMARKS.md', lambda R: write_table(R, R.arms)),
}


class Results:
    def __init__(self, path):
        self.meta, rows = resultfile.read(path)
        self.v = {(r['case'], r['arm'], r['metric']): float(r['value']) for r in rows if r['value'] not in ('', 'NA')}
        self.note = {(r['case'], r['arm'], r['metric']): r['note'] for r in rows if r['note']}
        self.arms = self.meta['arms'].split(',')
        self.tenants = [int(t) for t in self.meta['tenants'].split(',')]
        self.classes = self.meta['classes'].split(',')

    def get(self, tenant, cls, arm, metric):
        return self.v.get(('t%d:%s' % (tenant, cls), arm, metric))

    def cells(self, arm):
        """[(tenant, class)] the arm answers"""
        return [(t, c) for t in self.tenants for c in self.classes if self.get(t, c, arm, 'warm_ms') is not None]


def geomean(xs):
    return math.exp(sum(math.log(max(x, 1e-6)) for x in xs) / len(xs))


def faster(a, b):
    """a is faster than b: by more than 0.1 ms and by more than 10%"""
    return b - a > TIE_MS and a < b * (1 - TIE_PCT)


def fnum(x):
    return '{:,.0f}'.format(x) if x >= 100 else '%.1f' % x if x >= 10 else '%.2f' % x


def fcount(x):
    return '{:,}'.format(round(x))


def fpct(p):
    return ('%.1f' % p).rstrip('0').rstrip('.') if p >= 1 else '%.1f' % p


def fbytes(b):
    return '%.0f B' % b if b < 1e3 else '%.0f kB' % (b / 1e3) if b < 1e6 else '%.1f MB' % (b / 1e6)


def table(head, body, align=None):
    return resultfile.table(head, body, align or ['---'] + ['---:'] * (len(head) - 1))


# ---- tally ------------------------------------------------------------------------------------------------------------------------------

def tally_row(R, rival, metric):
    """(cells compared, scope faster, tie, rival faster, classes where the rival is faster) of the cells both arms answer"""
    both = [(t, c) for t, c in R.cells(BASE) if R.get(t, c, rival, metric) is not None]
    won, lost, where = 0, 0, {}
    for t, c in both:
        s, r = R.get(t, c, BASE, metric), R.get(t, c, rival, metric)
        if faster(s, r):
            won += 1
        elif faster(r, s):
            lost += 1
            where[c] = where.get(c, 0) + 1
    return len(both), won, len(both) - won - lost, lost, where


def tally(R):
    head = ['rival', 'cells', 'warm: %s faster / tie / rival faster' % BASE, 'cold: %s faster / tie / rival faster' % BASE, 'where the rival is faster (warm)']
    body = []
    for rival in (a for a in R.arms if a != BASE):
        n, w, t, l, where = tally_row(R, rival, 'warm_ms')
        _, cw, ct, cl, _ = tally_row(R, rival, 'cold_ms')
        body.append(['`%s`' % rival, str(n), '%d / %d / %d' % (w, t, l), '%d / %d / %d' % (cw, ct, cl),
                     ', '.join('`%s` %d' % kv for kv in sorted(where.items(), key=lambda kv: -kv[1])) or '-'])
    return table(head, body, ['---', '---:', '---:', '---:', '---'])


def tally_note(R):
    cells = len(R.cells(BASE))
    ok = sum(v for (case, arm, m), v in R.v.items() if m == 'md5_equal')
    total = sum(1 for (case, arm, m) in R.v if m == 'md5_equal')
    return ('Cells are a tenant and a class (%d of them for `%s`, in warm and in cold milliseconds); a rival is faster in a cell when it is faster by more than %.1f ms and by more than %d%%, '
            'and the cell is a tie within either margin. Every answer is md5-equal to the plain statement (%d of %d cells). Milliseconds are the executor time of `EXPLAIN (ANALYZE, TIMING OFF)`.'
            % (cells, BASE, TIE_MS, TIE_PCT * 100, ok, total))


# ---- reads ------------------------------------------------------------------------------------------------------------------------------

def geomean_table(R):
    common = [c for c in R.classes if all(R.get(R.tenants[0], c, a, 'warm_ms') is not None for a in R.arms)]
    head = ['tenant (share of the events)'] + R.arms
    body = []
    for t in R.tenants:
        share = R.v.get(('t%d' % t, 'fixture', 'events_pct'))
        body.append(['%d (%s%%)' % (t, fpct(share))] +
                    [fnum(geomean([R.get(t, c, a, 'warm_ms') for c in common])) for a in R.arms])
    return ['Warm milliseconds, geometric mean over the %d classes every arm answers (%s):' % (len(common), ', '.join('`%s`' % c for c in common)), ''] + table(head, body)


def read_cell(R, t, c, arm):
    w, k = R.get(t, c, arm, 'warm_ms'), R.get(t, c, arm, 'cold_ms')
    if w is None:
        return 'n/a'
    return '%s / %s · %s · %s / %s' % (fnum(w), fnum(k), fnum(R.get(t, c, arm, 'warm_minstr')), fcount(R.get(t, c, arm, 'warm_hit')), fcount(R.get(t, c, arm, 'cold_read')))


def tenant_table(R, t, arms):
    return table(['class'] + list(arms), [['`%s`' % c] + [read_cell(R, t, c, a) for a in arms] for c in R.classes], ['---'] + ['---:'] * len(arms))


def tenant_label(R, t):
    member_total, pct = R.v.get(('t%d' % t, 'fixture', 'member_total')), R.v.get(('t%d' % t, 'fixture', 'events_pct'))
    return 'tenant %d: %s member keys, %s%% of the events' % (t, fcount(member_total), fpct(pct))


# ---- writes -----------------------------------------------------------------------------------------------------------------------------

WRITES = (('insert 10k events', '10,000 inserts'), ('promotion of 10k events', 'promotion of those'), ('pair grant, median member', 'pair grant'),
          ('pair revoke, median member', 'pair revoke'), ('pair grant, largest member', 'pair grant, largest'), ('pair revoke, largest member', 'pair revoke, largest'))


def write_cell(R, arm, op):
    c = 'write:' + op
    if (c, arm, 'ms') not in R.v:
        return '-'
    return '%s · %s · %s' % (fnum(R.v[(c, arm, 'ms')]), fnum(R.v[(c, arm, 'minstr')]), fbytes(R.v[(c, arm, 'wal_bytes')]))


def write_table(R, arms):
    body = [['`%s`' % a, '%.1f s · %.0f MB' % (R.v[('build', a, 'build_s')], R.v[('build', a, 'added_bytes')] / 1e6)] + [write_cell(R, a, op) for op, _ in WRITES] for a in arms]
    return (['What each arm costs, as milliseconds · M instructions · WAL written of one statement (a pair edit: the mean of 10 member keys of about 400 events, and the largest member apart). '
             '`-`: the arm has no such step.', ''] +
            table(['arm', 'build · size added'] + [h for _, h in WRITES], body, ['---'] + ['---:'] * (len(WRITES) + 1)))


# ---- pages ------------------------------------------------------------------------------------------------------------------------------

def fixture_line(R):
    m = R.meta
    return ('%s events of %s member keys, %s tenants that reach member keys directly or through groups (%s distinct pairs)' %
            (fcount(float(m['facts'])), fcount(float(m['member_total'])), m['tenant_total'], fcount(float(m['pairs']))))


def report(R):
    out = ['# Scope index against tuned stock PostgreSQL: every table', '', fixture_line(R), '', '## Tally', ''] + tally(R) + ['', tally_note(R), '', '## Geometric mean', ''] + geomean_table(R)
    for t in R.tenants:
        out += ['', '## %s' % tenant_label(R, t), ''] + tenant_table(R, t, R.arms)
    return out + ['', '## Writes', ''] + write_table(R, R.arms)


def render(results, root):
    R = Results(results)
    for name, (page, fn) in BLOCKS.items():
        resultfile.replace_block(os.path.join(root, page), name, fn(R))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['render', 'report'])
    ap.add_argument('file', nargs='?')
    ap.add_argument('--results', default=RESULTS)
    ap.add_argument('--root', default=REPO)
    a = ap.parse_args()
    if a.cmd == 'render':
        render(a.results, a.root)
    else:
        print('\n'.join(report(Results(a.file or a.results))))


if __name__ == '__main__':
    main()
