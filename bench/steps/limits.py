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
import bench_charts  # noqa: E402
import cases as cases_mod  # noqa: E402
import cases_render  # noqa: E402
import compare  # noqa: E402
import extras  # noqa: E402
import readme  # noqa: E402
import store_charts  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
TIE = compare.TIE
LABEL = compare.LABEL


def ms(v):
    return '%.1f ms' % v if v < 100 else '{:,.0f} ms'.format(v)


def case_losses(results):
    path = os.path.join(results, 'cases.tsv')
    if not os.path.exists(path):
        return []
    out = []
    for f in cases_render.collect(extras.read(path)[1], cases_mod.read_registry()):
        if f['native'] and f['sift'] and f['native'] / f['sift'] <= 1 - TIE:
            unit = cases_render.UNIT[f['metric']]
            out.append((f['sift'] / f['native'], '`%s` %s%s' % (f['case'].id, unit, ' (the planner kept the same plan)' if f['kept'] else ''), 'PostgreSQL', ms(f['sift']), ms(f['native'])))
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
                if e == 'duckdb':
                    theirs, mine = (D.best(q) if D else None), (D.own(q, 'sift') if D else None)
                else:
                    r = S.lat(tier, e, q, 'warm')
                    theirs, mine = (r['value'] if r else None), compare.best_mine(S, tier, q)
                if theirs and mine and theirs / mine <= 1 - TIE:
                    out.append((mine / theirs, '%s at %s orders' % (compare.SLUG.get(q, q), format(size, ',')), LABEL[e], ms(mine), ms(theirs)))
    return out


def shape_losses(results):
    out = []
    for what, noun in (('whole', 'whole-table report'), ('set', 'report of a set of tenants')):
        for e, pairs in compare.shape_pairs(results, what).items():
            for name, r in pairs:
                if r <= 1 - TIE:
                    out.append((1 / r, '%s, %s' % (name, noun), LABEL[e], None, None))
    return out


def rate_losses(results):
    d = bench_charts.load(results)
    S = d['S']
    tier = S.tier_of(1_000_000, 3)
    others = {'pg': ['pg'], 'ch': ['ch', 'ch_mut', 'ch_rmt'], 'es': ['es'], 'citus': ['citus']}
    out = []
    for name, variant, metric, inv in (('update 10% of the rows (rows per second)', 'update 10%', 'phase_rows_s', False), ('delete 5% of the rows (rows per second)', 'delete 5%', 'phase_rows_s', False),
                                       ('load of 1,000,000 orders (seconds)', '-', 'load_s', True)):
        for e, pairs in compare.rate_pairs(S, tier, others, variant, metric, inv).items():
            if pairs[0][1] <= 1 - TIE:
                out.append((1 / pairs[0][1], name, LABEL[e], None, None))
    mine = min(v for e in compare.MINE if (v := store_charts.sizes(S, tier, e)[2]))
    for e in ('pg', 'ch', 'es', 'citus'):
        s = store_charts.sizes(S, tier, e)[2] if e in S.engines(tier) else None
        if s and s / mine <= 1 - TIE:
            out.append((mine / s, 'size on disk, table and indexes (`sift_table`, the smaller layout)', LABEL[e], None, None))
    return out


def block(results):
    rows = sorted(case_losses(results) + store_losses(results) + shape_losses(results) + rate_losses(results), key=lambda r: -r[0])
    head = ['Every number of bench/results where pg_sift is slower than the system it is compared with, by more than a 5% tie, worst first: written by the generator from the same files as the tables above, so a loss is not left out and a fixed one leaves the list. '
            'Warm medians; the layout of pg_sift is the faster of its two. Writes, the load, the disk and the shapes of a query the scan does not help are the usual places.', '']
    if not rows:
        return head + ['No number of bench/results is slower.']
    body = ['| what | against | slower by | pg_sift | the other |', '|---|---|---:|---:|---:|']
    body += ['| %s | %s | %s | %s | %s |' % (w, a, compare.x(r), mine or '-', theirs or '-') for r, w, a, mine, theirs in rows]
    return head + body


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
