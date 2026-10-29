#!/usr/bin/env python3
"""The README's "Limitations and trade-offs" (block `limits`): every number of bench/results where pg_sift is slower than the system it is compared with, beyond a 5% tie, in the order of the loss,
with both values; written by the generator so that a loss is never left out and a fixed one leaves the list on its own. Standard library only.

    python3 -I bench/steps/limits.py [--results DIR] [--readme FILE]      (steps/highlights.py runs it)
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aggregates_page  # noqa: E402
import bench_charts  # noqa: E402
import cases as cases_mod  # noqa: E402
import cases_render  # noqa: E402
import compare  # noqa: E402
import extras  # noqa: E402
import readme  # noqa: E402
import store_charts  # noqa: E402
import triple  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
TIE = compare.TIE
LABEL = compare.LABEL


def case_losses(results):
    path = os.path.join(results, 'cases.tsv')
    if not os.path.exists(path):
        return []
    rows, out = extras.read(path)[1], []
    ms = {(f['case'].id, f['metric']): f for f in cases_render.collect(rows, cases_mod.read_registry())}
    for f in ms.values():
        if not f['kept'] and f['native'] and f['sift'] and f['native'] / f['sift'] <= 1 - TIE:   # a case that runs PostgreSQL's plan has no ratio: the gap table of docs/BENCHMARKS.md
            c = f['case'].id
            time = lambda arm: (ms[c, 'ms'][arm] if (c, 'ms') in ms else f[arm])   # noqa: E731  (the time of the case beside a loss in another measure)
            out.append((f['sift'] / f['native'], '`%s` %s' % (c, cases_render.UNIT[f['metric']]), 'PostgreSQL', cases_render.case_fig(rows, c, 'sift', time('sift')), cases_render.case_fig(rows, c, 'native', time('native'))))
    return out


def store_losses(results):
    d = bench_charts.load(results)
    out = []
    for S, D, size in ((d['S'], d['D1'], 1_000_000), (d['SS'], d['D10'], 10_000_000)):
        tier = S.tier_of(size, 3) if S else None
        if not tier:
            continue
        for e in ('pg', 'ch', 'es', 'citus', 'duckdb'):
            for q in S.queries + list(store_charts.SEARCH):
                theirs = bench_charts.fig(S, D, tier, e, q)
                mine = (D.own(q, 'sift'), *triple.UNMEASURED) if e == 'duckdb' and D and D.own(q, 'sift') else triple.best(bench_charts.fig(S, D, tier, m, q) for m in compare.MINE)
                if theirs and mine and mine[0] and theirs[0] / mine[0] <= 1 - TIE:
                    out.append((mine[0] / theirs[0], '%s at %s orders' % (q, format(size, ',')), LABEL[e], mine, theirs))
    return out


def shape_losses(results):
    out = []
    for q, v, t in aggregates_page.shape_figs(results)[1] if aggregates_page.shape_figs(results) else []:
        for e, key in (('pg', 'heap'), ('ch', 'ch')):
            if t['col'] and t[key] and t[key][0] / t['col'][0] <= 1 - TIE:
                out.append((t['col'][0] / t[key][0], '%s, %s' % (q if v == '-' else f'{q} {v.replace("K", "")} tenants', 'whole-table report' if v == '-' else 'report of a set of tenants'), LABEL[e], t['col'], t[key]))
    return out


def rate_losses(results):
    d = bench_charts.load(results)
    S = d['S']
    tier = S.tier_of(1_000_000, 3)
    others = {'pg': ['pg'], 'ch': ['ch', 'ch_mut', 'ch_rmt'], 'es': ['es'], 'citus': ['citus']}
    out = []
    for name, variant, metric, inv, unit in (('update 10% of the rows (rows per second)', 'update 10%', 'phase_rows_s', False, ' rows/s'), ('delete 5% of the rows (rows per second)', 'delete 5%', 'phase_rows_s', False, ' rows/s'),
                                             ('load of 1,000,000 orders (seconds)', '-', 'load_s', True, ' s')):
        for e, pairs in compare.rate_pairs(S, tier, others, variant, metric, inv).items():
            _, ratio, mine, theirs = pairs[0]
            fmt = ',.1f' if unit == ' s' else ',.0f'
            if ratio <= 1 - TIE:
                out.append((1 / ratio, name, LABEL[e], f'{mine:{fmt}}{unit}{compare.MISSING}', f'{theirs:{fmt}}{unit}{compare.MISSING}'))
    mine = min(v for e in compare.MINE if (v := store_charts.sizes(S, tier, e)[2]))
    for e in ('pg', 'ch', 'es', 'citus'):
        s = store_charts.sizes(S, tier, e)[2] if e in S.engines(tier) else None
        if s and s / mine <= 1 - TIE:
            out.append((mine / s, 'size on disk, table and indexes (`sift_table`, the smaller layout)', LABEL[e], f'{mine:,.0f} MiB', f'{s:,.0f} MiB'))
    return out


def block(results):
    rows = sorted(case_losses(results) + store_losses(results) + shape_losses(results) + rate_losses(results), key=lambda r: -r[0])
    head = ['Every number of bench/results where pg_sift is slower than the system it is compared with, by more than a 5% tie, worst first: written by the generator from the same files as the tables above, so a loss is not left out and a fixed one leaves the list. '
            'Warm medians; the layout of pg_sift is the faster of its two. Writes, the load, the disk and the shapes of a query the scan does not help are the usual places.', '']
    if not rows:
        return head + ['No number of bench/results is slower.']
    cell = lambda f, ref=None: f if isinstance(f, str) else triple.cell(f, ref)   # noqa: E731  (a rate, a load time or a size is text)
    body = ['| what | against | slower by | pg_sift | the other |', '|---|---|---:|---:|---:|']
    body += ['| %s | %s | %s | %s | %s |' % (w, a, compare.x(r), cell(mine), cell(theirs, mine)) for r, w, a, mine, theirs in rows]
    figs = [f for r in rows for f in r[3:] if not isinstance(f, str)]
    return head + body + [''] + triple.legend(figs, 'MB read (the cases: MB of buffers hit and read; the application shapes: not recorded)') + [
        'The shapes are the application shapes of the 3.6 results (their CPU is the backend\'s). The rows of a write rate, a load and the disk carry their own unit and the CPU and IO of the write: `n/a`: not measured under 3.7 yet.']


def render(results, readme_path):
    if os.path.exists(readme_path) and '<!-- bench:limits:begin' in open(readme_path).read():
        readme.replace_block(readme_path, 'limits', block(results))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    a = ap.parse_args()
    render(a.results, a.readme)


if __name__ == '__main__':
    main()
