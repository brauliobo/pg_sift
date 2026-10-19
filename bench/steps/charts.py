#!/usr/bin/env python3
"""SVG charts of the committed benchmark tables (docs/results/*.tsv), standard library only.

Usage: python3 -I bench/steps/charts.py [--results DIR] [--out DIR] [--tier slim]
       (bench/bench charts; bench/bench report --docs runs it after writing the tables)

Writes into the output directory (default docs/img), all of them grouped bars drawn by one helper (`grouped`):
  index-size.svg          index size per engine and dataset
  build-time.svg          CREATE INDEX time per engine and dataset
  articles-throughput.svg queries per second per workload on the articles dataset (higher is better)
  articles-speedup.svg    the same relative to pg_search
  places-latency.svg      p50 latency per query class on the places dataset (lower is better)

The engines, their order, labels and colors are bench/engines.tsv, shared with bench/bench and steps/tables.py; an
engine with `chart = no` there is never drawn, any other appears in a chart when the dataset has rows for it (an engine without committed
rows is not drawn: there is no fake data). A workload the engine cannot run (NA) or a dataset it was not run on leaves a gap marked NA,
never a zero-height bar. A cell with a standard error (stderr column of the tables, one sd / sqrt(n) over the interleaved rounds) carries an
error bar of one standard error; a bar whose relative error is above 10% is drawn in red. Cells without one (one build, rows written before
the column existed) have no error bar. The charts carry their own white background and
border so they read the same on GitHub's light and dark themes. The output is deterministic: the same tables
give the same bytes.
"""
import argparse
import math
import os
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

DATASETS = ('articles', 'places')
FULL_SIZES = {'articles': 218058, 'places': 236262}   # the tier whose rows predate the corpus rows
TIER_NAMES = {'slim': 'Slim tier', 'ab': 'Intermediate tier', 'full': 'Full-size runs'}


def describe(rows, tier):
    """{dataset: {'name', 'rounds'}}: what the tables do not say, from the rows of the tier (the corpus rows of steps/tables.py: documents; the
    n of the cells: rounds)"""
    out = {}
    for d, what in (('articles', 'Simple English Wikipedia, %s articles'), ('places', 'GeoNames %s, %s places')):
        corpus = rows.get((d, tier, '-', '-', '-', 'corpus'))
        n = int(corpus[0]) if corpus and corpus[0] else FULL_SIZES[d] if tier == 'full' else 0
        rounds = sorted({int(v[4]) for k, v in rows.items() if k[:2] == (d, tier) and k[5] in ('qps', 'p50_ms') and v[4].isdigit() and int(v[4]) > 0})
        name = what % ((f'{n:,}',) if d == 'articles' else ('cities500' if tier == 'full' else 'cities15000', f'{n:,}'))
        out[d] = {'name': name, 'rounds': '%d-%d' % (rounds[0], rounds[-1]) if rounds and rounds[0] != rounds[-1] else str(rounds[0]) if rounds else '?'}
    return out


def session_date(rows, tier):
    """Oct 2026: the month of the session row of the tier"""
    import datetime
    for (d, t, e, _c, var, metric) in rows:
        if (t, e, metric) == (tier, '-', 'session'):
            kv = dict(w.split('=', 1) for w in var.split(';') if '=' in w)
            if kv.get('date'):
                return datetime.date.fromisoformat(kv['date']).strftime('%b %Y')
    return 'Oct 2026'


def engine_table():
    """id -> (chart label, color) of the engines with chart = yes, in the order of bench/engines.tsv"""
    with open(os.path.join(os.path.dirname(HERE), 'engines.tsv')) as f:
        rows = [l.rstrip('\n').split('\t') for l in f if l.strip() and not l.startswith('#')]
    return {r[0]: (r[1], r[3]) for r in rows if r[4] == 'yes'}


ENGINES = engine_table()
ARTICLE_CLASSES = [('and', 'and'), ('or', 'or'), ('phrase', 'phrase'), ('mixed', 'mixed'), ('count', 'count(*)'),
                   ('filter', 'filter'), ('join', 'join')]
PLACE_CLASSES = ['p1', 'p2', 'p3', 'p4', 'w1', 'a2', 'a2s', 'a3', 'x2']
PLACE_KEY = 'p1-p4: 1-4 letter prefix; w1: whole word; a2, a2s: word + 2 or 1 letter prefix; a3, x2: 3 or 2 prefixes'
PLACE_NOTES = ['pg_textsearch: emulation (edge n-gram text, it has no native prefix scoring), see docs/BENCHMARKS.md.']
BUILD_NOTES = ['places: pg_textsearch and rum show the native index; their emulated workloads add indexes (docs/BENCHMARKS.md).']
BM25_BUILD_NOTES = ['places: pgx-bm25 also stores two generated columns, the table grows (docs/BENCHMARKS.md).']
ES_BUILD_NOTES = ['Elasticsearch: bulk load and refresh; its force-merge to one segment is not in the build time.']
ES_ARTICLE_NOTES = ['Elasticsearch: a JVM server over HTTP; its join is a term filter on a denormalized region field.']
ES_PLACE_NOTES = ['Elasticsearch: a JVM server over HTTP, edge n-grams of the typed words expanded by the client.']
NA_NOTE = 'NA: no bar, the engine cannot run the workload or was not run on the dataset.'

WIDTH = 860
FONT = 'font-family="Helvetica, Arial, sans-serif"'
INK, MUTED, GRID, BG, BORDER = '#24292f', '#57606a', '#d8dee4', '#ffffff', '#d0d7de'
WARN = '#b42318'   # an error bar above FLAG of its value
FLAG = 0.10
ERR_NOTE = 'Error bars: one standard error over the interleaved rounds (rough for 2-3); red: relative error above 10%.'


def num(x):
    s = f'{x:.1f}'
    return s[:-2] if s.endswith('.0') else s


class Svg:
    def __init__(self, title, desc):
        self.title, self.desc, self.parts = title, desc, []

    def text(self, x, y, s, size=12, anchor='start', fill=INK, weight='normal', halo=False, rotate=False):
        w = f' font-weight="{weight}"' if weight != 'normal' else ''
        h = f' stroke="{BG}" stroke-width="3" paint-order="stroke"' if halo else ''
        at = f'transform="translate({num(x)} {num(y)}) rotate(-90)"' if rotate else f'x="{num(x)}" y="{num(y)}"'
        self.parts.append(f'<text {at} font-size="{size}" text-anchor="{anchor}" fill="{fill}"{w}{h}>{escape(s)}</text>')

    def rect(self, x, y, w, h, fill):
        self.parts.append(f'<rect x="{num(x)}" y="{num(y)}" width="{num(w)}" height="{num(h)}" fill="{fill}"/>')

    def line(self, x1, y1, x2, y2, stroke=GRID, width=1, dash=False):
        d = ' stroke-dasharray="4 3"' if dash else ''
        self.parts.append(f'<line x1="{num(x1)}" y1="{num(y1)}" x2="{num(x2)}" y2="{num(y2)}" stroke="{stroke}" stroke-width="{width}"{d}/>')

    def caption(self, y, s, notes=()):
        assert max(len(x) for x in (s, *notes)) * 11 * 0.56 < WIDTH - 40, f'caption too long: {s}'
        for i, note in enumerate(reversed(notes)):
            self.text(20, y - 18 * (i + 1), note, 11, fill=MUTED)
        self.text(20, y, s, 11, fill=MUTED)

    def render(self, height):
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" viewBox="0 0 {WIDTH} {height}" '
                f'role="img" aria-labelledby="t d" {FONT}>\n<title id="t">{escape(self.title)}</title>\n<desc id="d">{escape(self.desc)}</desc>\n'
                f'<rect x="0.5" y="0.5" width="{WIDTH - 1}" height="{height - 1}" rx="6" fill="{BG}" stroke="{BORDER}"/>\n')
        return head + '\n'.join(self.parts) + '\n</svg>\n'


def load(path):
    """(dataset, tier, engine, class, variant, metric) -> (value, min, max, stderr); value and stderr None for NA / error / absent."""
    rows = {}
    with open(path) as f:
        for line in f:
            if line.startswith('#') or line.startswith('dataset\t') or not line.strip():
                continue
            d, tier, eng, cls, var, metric, val, lo, hi, _unit, se, _n, _values = line.rstrip('\n').split('\t')
            if metric in ('session', 'wall_s', 'corpus'):   # host rows: strings and counts, not measurements
                rows[(d, tier, eng, cls, var, metric)] = (val, lo, hi, None, _n)
                continue
            rows[(d, tier, eng, cls, var, metric)] = (None if val in ('NA', 'error') else float(val), lo, hi, None if se == 'NA' or val in ('NA', 'error') else float(se), _n)
    return rows


def value(rows, tier, dataset, engine, cls, variant, metric):
    r = rows.get((dataset, tier, engine, cls, variant, metric))
    return None if r is None else r[0]


def stderr(rows, tier, dataset, engine, cls, variant, metric):
    r = rows.get((dataset, tier, engine, cls, variant, metric))
    return None if r is None else r[3]


def engines_of(rows, tier, datasets):
    """the engines of engines.tsv with rows in any of the datasets, in table order"""
    return [e for e in ENGINES if any(k[:3] == (d, tier, e) for k in rows for d in datasets)]


def load_range(rows, tier, datasets):
    lo = min(float(rows[(d, tier, '-', '-', '-', 'load_1m')][1]) for d in datasets)
    hi = max(float(rows[(d, tier, '-', '-', '-', 'load_1m')][2]) for d in datasets)
    return f'{lo:.0f}-{hi:.0f}'


def late_notes(rows, tier, dataset):
    """the classes merged from a later run (bench --merge) and the load of its cells"""
    return [f"{k[4].replace(',', ', ')}: measured in a later run (load avg {float(v[1]):.0f}-{float(v[2]):.0f})."
            for k, v in rows.items() if k[:4] == (dataset, tier, '-', '-') and k[4] != '-' and k[5] == 'load_1m']


def nice_axis(vmax, max_ticks=6):
    """(step, count): the smallest step in 1, 2, 2.5, 5 x 10^k with count = ceil(vmax / step) <= max_ticks."""
    k = math.floor(math.log10(vmax / max_ticks)) - 1
    while True:
        for m in (1, 2, 2.5, 5):
            step = m * 10 ** k
            if math.ceil(vmax / step) <= max_ticks:
                return step, math.ceil(vmax / step)
        k += 1


def tick(v):
    return f'{v:,.0f}' if v >= 10 or v == int(v) else f'{v:g}'


def legend(svg, y, engines, baseline):
    x = 20
    for e in engines:
        label, color = ENGINES[e]
        svg.rect(x, y - 10, 12, 12, color)
        svg.text(x + 17, y, label, 12)
        x += 17 + len(label) * 6.3 + 14
    if baseline:
        svg.line(x, y - 4, x + 18, y - 4, INK, 1.5, dash=True)
        svg.text(x + 23, y, 'pg_search = 1.0x', 12)


def grouped(title, direction, groups, series, fmt, ylabel, caption, baseline=None, notes=(), log=False, errors=None):
    """The one chart: vertical grouped bars. groups: [(label, sublabel)]; series: [(engine, [value or None per group])].
    A None value leaves a gap marked NA; the value labels turn vertical when the bars are narrow. log: a logarithmic axis
    (one gridline per decade) for values that span orders of magnitude. errors: {engine: [standard error or None per group]}, drawn
    as an error bar of one standard error on the bar (red above FLAG of the value)."""
    errors = errors or {}
    err = lambda e, gi: (errors.get(e) or [None] * len(groups))[gi]
    svg = Svg(title, '; '.join(f'{g[0]}: ' + ', '.join(f'{ENGINES[e][0]} {fmt(v[i])}' for e, v in series if v[i] is not None)
                               for i, g in enumerate(groups)))
    svg.text(20, 30, title, 16, weight='bold')
    svg.text(20, 48, direction, 12, fill=MUTED)
    legend(svg, 72, [e for e, _ in series], baseline is not None)
    left, right, top, ph = 74, 24, 100, 270
    pw = WIDTH - left - right
    gw = pw / len(groups)
    bw = min(30, gw * 0.84 / len(series))
    narrow = bw < 26
    values = [v for _, vs in series for v in vs if v is not None]
    vmax = max(v + (err(e, gi) or 0) for e, vs in series for gi, v in enumerate(vs) if v is not None)
    if log:
        lo, hi = 10 ** math.floor(math.log10(min(values) * 0.9)), 10 ** math.ceil(math.log10(vmax * (1.2 if narrow else 1.15)))
        ticks, height = [lo * 10 ** i for i in range(round(math.log10(hi / lo)) + 1)], lambda v: math.log10(v / lo) / math.log10(hi / lo) * ph
    else:
        step, cnt = nice_axis(max(vmax, baseline or 0) * (1.2 if narrow else 1.15))
        ticks, height = [i * step for i in range(cnt + 1)], lambda v: v * ph / (step * cnt)
    for t in ticks:
        gy = top + ph - height(t)
        svg.line(left, gy, left + pw, gy)
        svg.text(left - 8, gy + 4, tick(t), 10, 'end', MUTED)
    svg.parts.append(f'<text transform="translate(18 {num(top + ph / 2)}) rotate(-90)" font-size="11" text-anchor="middle" fill="{MUTED}">{escape(ylabel)}</text>')
    for gi, (label, sub) in enumerate(groups):
        gx = left + gi * gw + (gw - bw * len(series)) / 2
        for si, (e, vs) in enumerate(series):
            v, cx = vs[gi], gx + si * bw + bw / 2
            if v is None:
                svg.text(cx, top + ph - 5, 'NA', 9, 'middle', MUTED)
                continue
            svg.rect(gx + si * bw + 1, top + ph - height(v), bw - 2, height(v), ENGINES[e][1])
            se, label_y = err(e, gi), top + ph - height(v) - 5
            if se:
                lo_v = max(v - se, lo if log else 0)
                y1, y2, color = top + ph - height(v + se), top + ph - height(lo_v), WARN if se / v > FLAG else INK
                svg.line(cx, y1, cx, y2, color, 1.5)
                svg.line(cx - 3, y1, cx + 3, y1, color, 1.5)
                svg.line(cx - 3, y2, cx + 3, y2, color, 1.5)
                label_y = y1 - 5
            if narrow:
                svg.text(cx + 3.5, label_y, fmt(v), 10, halo=True, rotate=True)
            else:
                svg.text(cx, label_y, fmt(v), 10, 'middle', halo=True)
        cx = left + gi * gw + gw / 2
        svg.text(cx, top + ph + 18, label, 12, 'middle', weight='bold')
        if sub:
            svg.text(cx, top + ph + 32, sub, 10, 'middle', MUTED)
    if baseline is not None:
        by = top + ph - height(baseline)
        svg.line(left, by, left + pw, by, INK, 1.5, dash=True)
    svg.line(left, top + ph, left + pw, top + ph, MUTED)
    if any(v is None for _, vs in series for v in vs):
        notes = [*notes, NA_NOTE]
    if any(se for ses in errors.values() for se in ses):
        notes = [*notes, ERR_NOTE]
    bottom = top + ph + 56 + 18 * len(notes)
    svg.caption(bottom - 8, caption, notes)
    return svg.render(bottom + 8)


def host(rows, tier, datasets):
    """the machine behind a chart (the session row of tables.py): CPU model and threads"""
    for (d, t, e, _c, var, metric) in rows:
        if (t, e, metric) == (tier, '-', 'session') and d in datasets:
            kv = dict(w.split('=', 1) for w in var.split(';') if '=' in w)
            if kv.get('cpu'):
                return f"{kv['cpu']}, {kv.get('threads', '?')} threads"
    return 'host not recorded'


def caption(rows, tier, datasets, what):
    return f'{what}; {host(rows, tier, datasets)} (load avg {load_range(rows, tier, datasets)}); {session_date(rows, tier)}.'


def errors_of(rows, tier, dataset, engines, keys, metric):
    """{engine: [standard error per key]} for keys (class, variant)"""
    return {e: [stderr(rows, tier, dataset, e, c, v, metric) for c, v in keys] for e in engines}


def build_chart(rows, tier, title, direction, metric, fmt, ylabel):
    """index size and build time: one group per dataset"""
    engines = engines_of(rows, tier, DATASETS)
    series = [(e, [value(rows, tier, d, e, '-', '-', metric) for d in DATASETS]) for e in engines]
    groups = [(d, spec['name']) for d, spec in describe(rows, tier).items()]
    notes = BUILD_NOTES + (BM25_BUILD_NOTES if 'bm25' in engines else []) + (ES_BUILD_NOTES if 'elasticsearch' in engines else [])
    return grouped(title, direction, groups, series, fmt, ylabel, caption(rows, tier, DATASETS, f'{TIER_NAMES.get(tier, tier)}, one build per engine'), notes=notes)


def make(rows, tier):
    out = {}
    out['index-size.svg'] = build_chart(rows, tier, 'Index size', 'lower is better', 'index_mib', lambda v: f'{v:.1f}', 'MiB')
    out['build-time.svg'] = build_chart(rows, tier, 'Index build time (CREATE INDEX)', 'lower is better', 'build_s', lambda v: f'{v:.1f}', 'seconds')

    a = describe(rows, tier)['articles']
    keys = [(c, '-') for c, _ in ARTICLE_CLASSES]
    groups = [(label, '') for _, label in ARTICLE_CLASSES]
    engines = engines_of(rows, tier, ['articles'])
    qps = {e: [value(rows, tier, 'articles', e, c, '-', 'qps') for c, _ in ARTICLE_CLASSES] for e in engines}
    se = errors_of(rows, tier, 'articles', engines, keys, 'qps')
    cap = caption(rows, tier, ['articles'], f"{a['name']}, 8 clients, median of {a['rounds']} rounds")
    notes = ES_ARTICLE_NOTES if 'elasticsearch' in engines else []
    out['articles-throughput.svg'] = grouped('Throughput by workload', 'queries per second, higher is better', groups, list(qps.items()),
                                             lambda v: f'{v:,.0f}', 'queries per second', cap, notes=notes, errors=se)
    ratio = {e: [None if q is None else q / b for q, b in zip(vs, qps['pg_search'])] for e, vs in qps.items() if e != 'pg_search'}
    # the standard error of a quotient: the relative errors add in quadrature
    ratio_se = {e: [None if None in (s, t, q, b) else (q / b) * math.hypot(s / q, t / b)
                    for q, b, s, t in zip(qps[e], qps['pg_search'], se[e], se['pg_search'])] for e in ratio}
    out['articles-speedup.svg'] = grouped('Throughput relative to pg_search', 'queries per second divided by pg_search, higher is better', groups,
                                          list(ratio.items()), lambda v: f'{v:.2f}x', 'speedup over pg_search', cap, baseline=1.0, notes=notes, errors=ratio_se)

    p = describe(rows, tier)['places']
    keys = [(c, 'top') for c in PLACE_CLASSES]
    engines = engines_of(rows, tier, ['places'])
    lat = {e: [value(rows, tier, 'places', e, c, 'top', 'p50_ms') for c in PLACE_CLASSES] for e in engines}
    cap = caption(rows, tier, ['places'], f"{p['name']}, top-20, 1 client, median of {p['rounds']} rounds")
    out['places-latency.svg'] = grouped('Autocomplete latency by query class', 'p50 milliseconds, logarithmic axis, lower is better', [(c, '') for c in PLACE_CLASSES],
                                        list(lat.items()), lambda v: f'{v:,.0f}' if v >= 100 else f'{v:.2f}', 'p50 latency (ms)', cap, notes=[PLACE_KEY] + PLACE_NOTES + (ES_PLACE_NOTES if 'elasticsearch' in engines else []) + late_notes(rows, tier, 'places'),
                                        log=True, errors=errors_of(rows, tier, 'places', engines, keys, 'p50_ms'))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'docs', 'results'))
    ap.add_argument('--out', default=os.path.join(REPO, 'docs', 'img'))
    ap.add_argument('--tier', default='slim')
    args = ap.parse_args()
    rows = {}
    for d in DATASETS:
        rows.update(load(os.path.join(args.results, f'{d}.tsv')))
    os.makedirs(args.out, exist_ok=True)
    for name, svg in make(rows, args.tier).items():
        with open(os.path.join(args.out, name), 'w', newline='\n') as f:
            f.write(svg)
        print(os.path.join(args.out, name))


if __name__ == '__main__':
    sys.exit(main())
