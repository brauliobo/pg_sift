#!/usr/bin/env python3
"""The benchmark section of the README from the results of `bench cases` (and `bench generic` for the other engines): one case for each statement class of bench/classes.tsv, its
absolute figures with the factor beside them, a chart of the time against the rows for every class, and the cases where the baseline is ahead.

    python3 -I glance.py --results bench/results [--generic FILE] [--out bench/out]

Writes OUT/README-glance.md and OUT/img/at-a-glance.svg, slope.svg, cold-read.svg (self-contained: light and dark by prefers-color-scheme, a title and a description, no date and no host).
Results: every cases*.tsv of the directory whose header says `# orders: N` is the run at N rows; a case in several files at one size takes the last. The representative of a class is the
candidate with the largest absolute gap in milliseconds at the largest size measured, either way (so a class where the baseline is ahead is shown by the case where it is ahead most).
"""
import argparse
import glob
import math
import os
import re
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
SIZES = (100000, 300000, 1000000)
ARMS = ('native', 'btree', 'sift', 'pg_duckdb')


def num(x):
    try:
        return float(x)
    except ValueError:
        return None


def read(path):
    """(rows of the run or None, {key: value} of the `# key: value` lines, [(case, arm, metric, value)])"""
    rows, meta, out = None, {}, []
    for line in open(path):
        if line.startswith('#'):
            k, _, v = line[1:].partition(':')
            meta[k.strip()] = v.strip()
            continue
        f = line.rstrip('\n').split('\t')
        if len(f) > 4 and f[0] != 'version' and num(f[4]) is not None:
            out.append((f[1], f[2], f[3], num(f[4])))
    if meta.get('orders', '').isdigit():
        rows = int(meta['orders'])
    return rows, meta, out


def load(results):
    """data[case][rows][arm][metric]; the versions of the header lines"""
    data, versions = {}, {}
    for path in sorted(glob.glob(os.path.join(results, 'cases*.tsv'))):
        rows, meta, out = read(path)
        if rows is None:
            continue
        versions.update({k: v for k, v in meta.items() if k in ('postgresql', 'pg_sift', 'pg_duckdb', 'duckdb')})
        for case, arm, metric, value in out:
            data.setdefault(case, {}).setdefault(rows, {}).setdefault(arm, {})[metric] = value
    return data, versions


def classes(path=os.path.join(BENCH, 'classes.tsv')):
    """[(class, case, baseline arm, analytic)] in file order"""
    return [tuple(l.rstrip('\n').split('\t')) for l in open(path) if l.strip() and not l.startswith('#')]


def top(data, case):
    """the largest size with a sift row of the case"""
    return max((n for n in data.get(case, {}) if 'sift' in data[case][n]), default=None)


def gap(data, case, base):
    n = top(data, case)
    b = data.get(case, {}).get(n, {}).get(base, {}).get('ms') if n else None
    s = data.get(case, {}).get(n, {}).get('sift', {}).get('ms') if n else None
    return abs(b - s) if b is not None and s is not None else None


def representatives(data, table):
    """{class: (case, base)}: the candidate with the largest absolute gap, among those with the three sizes when there are any"""
    best = {}
    for cls, case, base, _ in table:
        g = gap(data, case, base)
        full = sum(1 for n in SIZES if n in data.get(case, {}) and base in data[case][n] and 'sift' in data[case][n]) == 3
        if g is not None and (cls not in best or (full, g) > best[cls][0]):
            best[cls] = ((full, g), case, base)
    return {cls: (c, b) for cls, (_, c, b) in best.items()}


def ms(v):
    return '–' if v is None else ('%.3g' % v if v < 100 else '{:,}'.format(round(v)))


def count(v):
    return '–' if v is None else ('%.3g' % v if v < 100 else '{:,}'.format(round(v)))


def factor(b, s):
    if not b or not s:
        return '–'
    r = b / s
    return '**%.2f× (slower)**' % r if r < 1 else ('%.1f×' % r if r < 10 else '{:,}×'.format(round(r)))


def pair(b, s, f=count):
    return '–' if b is None and s is None else '%s → %s' % (f(b), f(s))


def figures(data, case, n, base):
    d = data[case][n]
    g = lambda arm, m: d.get(arm, {}).get(m)
    mb = lambda v: '–' if v is None else ('%.3g' % v)
    return dict(b=g(base, 'ms'), s=g('sift', 'ms'), instr=pair(g(base, 'minstr'), g('sift', 'minstr')), blocks=pair(g(base, 'io_cold_read_blocks'), g('sift', 'io_cold_read_blocks')),
                mb=pair(g(base, 'io_cold_read_mb'), g('sift', 'io_cold_read_mb'), mb), ops=pair(g(base, 'io_cold_read_ops'), g('sift', 'io_cold_read_ops')),
                kb=pair(g(base, 'io_cold_read_avg_kb'), g('sift', 'io_cold_read_avg_kb'), mb), duck=g('pg_duckdb', 'ms'))


def statement(case):
    path = os.path.join(BENCH, 'cases', case + '.sql')
    if not os.path.exists(path):
        return ''
    m = re.search(r'--@ pg\n(.*?)(?=\n--@|\Z)', open(path).read(), re.S)
    return m.group(1).strip() if m else ''


def label(case):
    path = os.path.join(BENCH, 'cases', case + '.sql')
    return next((l[9:].strip() for l in open(path) if l.startswith('-- label:')), '') if os.path.exists(path) else ''


# ---- the charts
CSS = ('<style>svg{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;font-size:12px}.t{fill:#1f2328}.g{stroke:#d0d7de}.b{fill:#6e7781}.s{fill:#0e7c66}.lb{stroke:#6e7781;fill:none}'
       '.ls{stroke:#0e7c66;fill:none}.mb{fill:#6e7781}.ms{fill:#0e7c66}@media (prefers-color-scheme:dark){.t{fill:#e6edf3}.g{stroke:#30363d}.b{fill:#8b949e}.s{fill:#3fb98f}'
       '.lb{stroke:#8b949e}.ls{stroke:#3fb98f}.mb{fill:#8b949e}.ms{fill:#3fb98f}}</style>')


def svg(w, h, title, desc, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" role="img" aria-labelledby="t d"><title id="t">%s</title><desc id="d">%s</desc>%s%s</svg>\n'
            % (w, h, w, escape(title), escape(desc), CSS, body))


def tick(v):
    return '%g' % v if v < 1000 else '{:,}'.format(round(v))


def bars(items, title, desc, unit, base_name):
    """items: [(label, base value, sift value)]; paired horizontal bars on a log axis"""
    vals = [v for _, a, b in items for v in (a, b) if v]
    lo, hi = 10 ** math.floor(math.log10(min(vals))), 10 ** math.ceil(math.log10(max(vals)))
    W, L, R, rh = 760, 230, 80, 38
    H = 64 + rh * len(items) + 26
    X = lambda v: L + (W - L - R) * (math.log10(max(v, lo)) - math.log10(lo)) / max(math.log10(hi) - math.log10(lo), 1e-9)
    b, t = '', lo
    while t <= hi * 1.001:
        b += '<line class="g" x1="%.1f" x2="%.1f" y1="44" y2="%d"/><text class="t" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (X(t), X(t), H - 22, X(t), H - 8, tick(t))
        t *= 10
    b += '<text class="t" x="%d" y="16" font-weight="600">%s</text><text class="t" x="%d" y="33" text-anchor="end">%s, log scale</text>' % (L, escape(title), W - 10, unit)
    b += '<rect class="b" x="%d" y="24" width="10" height="10"/><text class="t" x="%d" y="33">%s</text><rect class="s" x="%d" y="24" width="10" height="10"/><text class="t" x="%d" y="33">pg_sift</text>' % (L, L + 14, escape(base_name), L + 150, L + 164)
    for i, (lab, a, s) in enumerate(items):
        y = 54 + i * rh
        b += '<text class="t" x="%d" y="%d" text-anchor="end">%s</text>' % (L - 8, y + 18, escape(lab))
        for k, (v, cls) in enumerate(((a, 'b'), (s, 's'))):
            if v:
                b += '<rect class="%s" x="%d" y="%d" width="%.1f" height="14"/><text class="t" x="%.1f" y="%d">%s</text>' % (cls, L, y + k * 17, max(X(v) - L, 1), X(v) + 4, y + k * 17 + 11, count(v))
    return svg(W, H, title, desc, b)


def panel(x0, y0, w, h, name, series, lo, hi):
    """one class: ms against rows for the baseline and pg_sift, 100k, 300k and 1M"""
    X = lambda n: x0 + 40 + (w - 52) * (math.log10(n) - 5) / 1
    Y = lambda v: y0 + 24 + (h - 44) * (1 - (math.log10(max(v, lo)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)))
    b = '<text class="t" x="%d" y="%d" font-weight="600">%s</text>' % (x0 + 40, y0 + 12, escape(name))
    t = lo
    while t <= hi * 1.001:
        b += '<line class="g" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/><text class="t" x="%d" y="%.1f" text-anchor="end">%s</text>' % (x0 + 40, x0 + w - 12, Y(t), Y(t), x0 + 36, Y(t) + 4, tick(t))
        t *= 100
    for n, lab in ((100000, '100k'), (300000, '300k'), (1000000, '1M')):
        b += '<text class="t" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (X(n), y0 + h - 4, lab)
    for pts, line, mark in ((series[0], 'lb', 'mb'), (series[1], 'ls', 'ms')):
        pts = [(n, v) for n, v in pts if v]
        b += '<polyline class="%s" stroke-width="2" points="%s"/>' % (line, ' '.join('%.1f,%.1f' % (X(n), Y(v)) for n, v in pts))
        b += ''.join('<circle class="%s" cx="%.1f" cy="%.1f" r="3"/>' % (mark, X(n), Y(v)) for n, v in pts)
    return b


def slope(reps, data):
    vals = [data[c][n][a].get('ms') for c, base in reps.values() for n in data[c] for a in (base, 'sift') if data[c][n].get(a, {}).get('ms')]
    lo, hi = 10 ** math.floor(math.log10(min(vals))), 10 ** math.ceil(math.log10(max(vals)))
    cols, pw, ph = 3, 250, 190
    rowsn = -(-len(reps) // cols)
    body = '<text class="t" x="10" y="16" font-weight="600">Time against rows, milliseconds on a log scale</text>'
    body += '<rect class="b" x="10" y="24" width="10" height="10"/><text class="t" x="24" y="33">baseline (the heap plan or the b-tree, per class)</text><rect class="s" x="330" y="24" width="10" height="10"/><text class="t" x="344" y="33">pg_sift</text>'
    for i, (cls, (case, base)) in enumerate(reps.items()):
        series = [[(n, data[case].get(n, {}).get(a, {}).get('ms')) for n in SIZES] for a in (base, 'sift')]
        body += panel(8 + (i % cols) * pw, 46 + (i // cols) * ph, pw - 6, ph - 6, cls, series, lo, hi)
    return svg(cols * pw + 10, 46 + rowsn * ph + 6, 'Time against rows for each statement class',
               'One panel for each class: the milliseconds of the baseline and of pg_sift at 100,000, 300,000 and 1,000,000 rows on log axes.', body)


# ---- the page
def page(data, versions, table, reps, generic):
    out = []
    w = out.append
    w('## Benchmarks\n')
    w('One node, PostgreSQL %s, pg_sift %s, default settings unless a case says otherwise; synthetic tables of N rows generated by `bench/sql/*/gen.sql`, the same rows for every engine. '
      'Every figure is a warm median of the statement; blocks, MB and read operations are those of the first run after the relations were emptied from the caches. '
      'Each class is shown by the case with the largest absolute gap in milliseconds, whichever way it goes.\n' % (versions.get('postgresql', '?'), versions.get('pg_sift', '?')))
    w('![Time per statement class](img/at-a-glance.svg)\n')
    w('### At a glance\n')
    w('| class | case | rows | baseline | baseline ms → pg_sift ms | factor | M instructions | cold blocks | cold MB | cold read operations |')
    w('|---|---|---:|---|---:|---:|---|---|---|---|')
    for cls, (case, base) in reps.items():
        n = top(data, case)
        f = figures(data, case, n, base)
        w('| %s | [`%s`](#%s) | %s | %s | %s → %s | %s | %s | %s | %s | %s |' % (cls, case, case.replace('_', '-'), '{:,}'.format(n), 'b-tree' if base == 'btree' else 'PostgreSQL plan', ms(f['b']), ms(f['s']),
                                                                                factor(f['b'], f['s']), f['instr'], f['blocks'], f['mb'], f['ops']))
    w('\nA figure in bold is one where pg_sift is slower. The baseline of a lookup or a page is the b-tree of the same rows when the case has one, and PostgreSQL\'s own plan otherwise.\n')
    analytic = [(cls, case, n) for cls, (case, base) in reps.items() for n in [top(data, case)] if (cls, case, base, 'yes') in table]
    if analytic or generic:
        w('### Analytic classes beside the other engines\n')
        w('| class | case | rows | PostgreSQL ms | pg_sift ms | pg_duckdb ms | DuckDB ms |')
        w('|---|---|---:|---:|---:|---:|---:|')
        for cls, case, n in analytic:
            d = data[case][n]
            w('| %s | `%s` | %s | %s | %s | %s | %s |' % (cls, case, '{:,}'.format(n), ms(d.get('native', {}).get('ms')), ms(d.get('sift', {}).get('ms')), ms(d.get('pg_duckdb', {}).get('ms')), ms(generic.get(case))))
        w('\n%s; the same table and hardware; a dash is a statement the engine does not run.\n' % ', '.join('%s %s' % (name, versions[key]) for name, key in (('PostgreSQL', 'postgresql'), ('pg_duckdb', 'pg_duckdb'), ('DuckDB', 'duckdb')) if key in versions))
    w('![Megabytes read on a cold start](img/cold-read.svg)\n')
    w('### Time against rows\n')
    w('![Time against rows for each class](img/slope.svg)\n')
    w('### Classes\n')
    for cls, (case, base) in reps.items():
        n = top(data, case)
        f = figures(data, case, n, base)
        w('<details id="%s"><summary><b>%s</b>: <code>%s</code>, %s at %s rows</summary>\n' % (case.replace('_', '-'), cls, case, factor(f['b'], f['s']).replace('**', ''), '{:,}'.format(n)))
        if label(case):
            w(label(case) + '\n')
        if statement(case):
            w('```sql\n%s\n```\n' % statement(case))
        w('| rows | baseline ms | pg_sift ms | factor | M instructions | cold blocks | cold MB | cold read operations |')
        w('|---:|---:|---:|---:|---|---|---|---|')
        for m in sorted(data[case]):
            g = figures(data, case, m, base)
            w('| %s | %s | %s | %s | %s | %s | %s | %s |' % ('{:,}'.format(m), ms(g['b']), ms(g['s']), factor(g['b'], g['s']), g['instr'], g['blocks'], g['mb'], g['ops']))
        w('\n</details>\n')
    w('### Where pg_sift is not faster\n')
    w('| case | class | rows | baseline | baseline ms → pg_sift ms | factor |')
    w('|---|---|---:|---|---:|---:|')
    losing = 0
    for cls, case, base, _ in table:
        n = top(data, case)
        if n is None or data[case][n].get(base, {}).get('ms') is None:
            continue
        f = figures(data, case, n, base)
        if f['s'] and f['b'] and f['b'] / f['s'] < 1:
            losing += 1
            w('| `%s` | %s | %s | %s | %s → %s | %s |' % (case, cls, '{:,}'.format(n), 'b-tree' if base == 'btree' else 'PostgreSQL plan', ms(f['b']), ms(f['s']), factor(f['b'], f['s'])))
    if not losing:
        w('| – | – | – | – | – | no case of the classes is slower |')
    w('\nA small LIMIT or a short OFFSET over a column with a b-tree is answered by the b-tree in a few index pages; pg_sift reads the sorted array of the key instead, which pays from a deeper OFFSET or a range.\n')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(BENCH, 'results'))
    ap.add_argument('--generic', help='generic.tsv of `bench generic`: the DuckDB arm of the analytic classes')
    ap.add_argument('--out', default=os.path.join(BENCH, 'out'))
    a = ap.parse_args()
    data, versions = load(a.results)
    table = classes()
    reps = representatives(data, table)
    if not reps:
        sys.exit('glance: no class has a case with a sift row in %s' % a.results)
    generic = {}
    if a.generic:
        for line in open(a.generic):
            f = line.rstrip('\n').split('\t')
            if len(f) > 4 and f[2] == 'duckdb' and f[3] == 'ms' and num(f[4]) is not None:
                generic[f[1]] = num(f[4])
    os.makedirs(os.path.join(a.out, 'img'), exist_ok=True)
    items = []
    cold = []
    for cls, (case, base) in reps.items():
        n = top(data, case)
        d = data[case][n]
        items.append(('%s (%s)' % (cls, '{:,}'.format(n)), d.get(base, {}).get('ms'), d['sift'].get('ms')))
        if d.get(base, {}).get('io_cold_read_mb') and d['sift'].get('io_cold_read_mb'):
            cold.append((cls, d[base]['io_cold_read_mb'], d['sift']['io_cold_read_mb']))
    with open(os.path.join(a.out, 'img', 'at-a-glance.svg'), 'w') as f:
        f.write(bars(items, 'Warm median time per statement', 'Paired horizontal bars on a log scale: milliseconds of the baseline (the heap plan or the b-tree) and of pg_sift for one case of each class.', 'milliseconds', 'baseline'))
    with open(os.path.join(a.out, 'img', 'cold-read.svg'), 'w') as f:
        f.write(bars(cold, 'Megabytes read by the first run after the caches were emptied', 'Paired horizontal bars on a log scale: megabytes read from storage by the baseline and by pg_sift for one case of each class.', 'megabytes', 'baseline'))
    with open(os.path.join(a.out, 'img', 'slope.svg'), 'w') as f:
        f.write(slope(reps, data))
    with open(os.path.join(a.out, 'README-glance.md'), 'w') as f:
        f.write(page(data, versions, table, reps, generic))
    print('glance: %d classes -> %s' % (len(reps), a.out))


if __name__ == '__main__':
    main()
