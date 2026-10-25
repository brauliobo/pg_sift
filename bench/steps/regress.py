"""Regression check of the benchmark results: every engine against its reference engine (bench/engines.tsv, last column), cell by cell.

    python3 -I steps/regress.py [--sigmas 2] [--min 0.05] [--noise 0.15] [--all] results.tsv [more.tsv ...]

Reads results.tsv files of `bench/bench report` (bench/results/<dataset>.tsv work too) and compares, per dataset, tier, class and
variant, an engine with its reference (sift with the build of an older git ref, sift_ref) for throughput (qps), latency (p50_ms,
cold_p50_ms), index size (index_mib) and CREATE INDEX time (build_s). A cell is a regression when the new value is worse (lower
throughput, higher latency, size or time) by more than --min of the reference (default 5%) AND by more than --sigmas combined standard
errors (default 2, sqrt(se_new^2 + se_ref^2), the stderr column: sd / sqrt(n) over the interleaved rounds). An index size is a
deterministic number (no standard error). Cells measured once (build_s, cold-cache cells) have no standard error: --noise is the
relative standard error assumed for them (default 15%, an assumption, not a measurement). A cell that runs on the reference but is NA or
an error on the new engine is a regression; the opposite counts as an improvement; a cell that fails on both is an ERROR (exit 2).
Exit status: 1 when a regression is found, 2 when a cell failed on both sides or no engine pair has results on both sides, else 0. --all prints the cells within the thresholds too.
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import engines  # noqa: E402  (steps/engines.py: bench/engines.tsv)
HIGHER_BETTER = {'qps'}
LOWER_BETTER = {'p50_ms', 'cold_p50_ms', 'index_mib', 'build_s'}   # no p95_ms: the tail of 1 s cells is too noisy for a 5% rule
DETERMINISTIC = {'index_mib'}


def references():
    """engine -> reference engine from bench/engines.tsv"""
    return {r['id']: r['reference'] for r in engines.rows() if r['reference'] != '-'}


def load(paths):
    """(dataset, tier, engine, class, variant, metric) -> (value, stderr, n); value None for NA / error, stderr None when absent"""
    rows = {}
    for path in paths:
        with open(path) as f:
            for line in f:
                if line.startswith('#') or line.startswith('dataset\t') or not line.strip():
                    continue
                c = line.rstrip('\n').split('\t')
                c += ['NA'] * (13 - len(c))
                num = lambda x: None if x in ('NA', 'error') else float(x)
                rows[tuple(c[:6])] = (c[6] if c[6] in ('NA', 'error') else float(c[6]), num(c[10]), num(c[11]))
    return rows


def verdict(metric, new, ref, args):
    """(verdict, ratio, combined standard error as a fraction of the reference) of one pair of cells"""
    (nv, ns, _), (rv, rs, _) = new, ref
    if isinstance(nv, str) or isinstance(rv, str):
        if isinstance(nv, str) and isinstance(rv, str):
            return ('ERROR' if 'error' in (nv, rv) else None), None, None   # both failed: nothing to compare, the run is broken
        return ('REGRESSION' if isinstance(nv, str) else 'improved'), None, None
    if metric in DETERMINISTIC:
        ns = rs = 0.0
    else:
        ns = nv * args.noise if ns is None else ns
        rs = rv * args.noise if rs is None else rs
    se = math.hypot(ns, rs)
    worse = (rv - nv) if metric in HIGHER_BETTER else (nv - rv)   # positive: the new value is worse
    ratio = nv / rv if rv else None
    if worse > args.min * rv and worse > args.sigmas * se:
        return 'REGRESSION', ratio, se / rv if rv else None
    if -worse > args.min * rv and -worse > args.sigmas * se:
        return 'improved', ratio, se / rv if rv else None
    return 'ok', ratio, se / rv if rv else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('results', nargs='+')
    ap.add_argument('--sigmas', type=float, default=2.0)
    ap.add_argument('--min', type=float, default=0.05)
    ap.add_argument('--noise', type=float, default=0.15)
    ap.add_argument('--all', action='store_true')
    args = ap.parse_args()
    rows, refs = load(args.results), references()
    out, regressions, errors, compared, skipped = [], 0, 0, 0, []
    for engine, ref in refs.items():
        keys = sorted(k for k in rows if k[2] == engine and k[5] in HIGHER_BETTER | LOWER_BETTER)
        pairs = [(k, (k[0], k[1], ref) + k[3:]) for k in keys if (k[0], k[1], ref) + k[3:] in rows]
        if not pairs:
            if keys:
                skipped.append(f'{engine}: no results of its reference {ref}')
            continue
        for k, rk in pairs:
            v, ratio, se = verdict(k[5], rows[k], rows[rk], args)
            if v is None:
                continue
            compared += 1
            regressions += v == 'REGRESSION'
            errors += v == 'ERROR'
            if v != 'ok' or args.all:
                f = lambda x: x if isinstance(x, str) else f'{x:.4g}'
                out.append((k[0], k[1], engine, ref, k[3], k[4], k[5], f(rows[k][0]), f(rows[rk][0]),
                            '-' if ratio is None else f'{ratio:.2f}x', '-' if se is None else f'{se * 100:.0f}%', v))
    for s in skipped:
        print('skipped:', s)
    if not compared:
        print('no engine pair with results on both sides (bench/bench regress --run measures sift and its reference)')
        return 2
    head = ('dataset', 'tier', 'engine', 'reference', 'class', 'variant', 'metric', 'new', 'reference value', 'new / ref', 'combined se', 'verdict')
    print('| ' + ' | '.join(head) + ' |')
    print('|' + '---|' * len(head))
    for r in out:
        print('| ' + ' | '.join(r) + ' |')
    print(f'\n{compared} cells compared, {regressions} regression(s): worse by more than {args.min:.0%} and {args.sigmas:g} combined standard errors '
          f'(single measurements: {args.noise:.0%} relative error assumed).')
    if errors:
        print(f'{errors} cell(s) failed on the new and the reference engine: the run is broken, not comparable')
    return 1 if regressions else 2 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
