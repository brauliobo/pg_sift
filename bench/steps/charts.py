#!/usr/bin/env python3
"""SVG charts of the committed benchmark tables (docs/results/*.tsv), standard library only.

Usage: python3 -I bench/steps/charts.py [--results DIR] [--out DIR] [--tier full]
       (bench/bench charts; bench/bench report --docs runs it after writing the tables)

Writes into the output directory (default docs/img):
  index-size.svg          index size per engine and dataset
  build-time.svg          CREATE INDEX time per engine and dataset
  articles-throughput.svg queries per second per workload on the articles dataset (higher is better)
  articles-speedup.svg    the same relative to pg_search
  places-latency.svg      p50 latency per query class on the places dataset (lower is better)

The charts carry their own white background and border so they read the same on GitHub's light and
dark themes. An engine without rows in the tables (rum until it is measured) is labeled "pending"
and never drawn as a bar. The output is deterministic: the same tables give the same bytes.
"""
import argparse
import math
import os
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# What the tables do not say: the scale of each dataset, the rounds and the host class of the session.
DATASETS = {
    'articles': {'name': 'Simple English Wikipedia, 218,058 articles', 'engines': ['sift', 'sift_pairs', 'pg_search', 'rum'], 'rounds': 6},
    'places': {'name': 'GeoNames cities500, 236,262 places', 'engines': ['sift', 'pg_search', 'rum'], 'rounds': 6},
}
HOST = '8-core desktop, shared host'
DATE = 'Oct 2026'

ENGINES = {  # label, Okabe-Ito colors (distinguishable with color vision deficiencies)
    'sift': ('sift', '#0072B2'),
    'sift_pairs': ('sift pairs', '#009E73'),
    'pg_search': ('pg_search', '#D55E00'),
    'rum': ('rum', '#7f7f7f'),
}
ARTICLE_CLASSES = [('and', 'and'), ('or', 'or'), ('phrase', 'phrase'), ('mixed', 'mixed'), ('count', 'count(*)'),
                   ('filter', 'filter'), ('join', 'join')]
PLACE_CLASSES = ['p1', 'p2', 'p3', 'p4', 'w1', 'a2', 'a2s', 'a3', 'x2']
PLACE_KEY = 'p1-p4: 1-4 letter prefix; w1: whole word; a2, a2s: word + 2 or 1 letter prefix; a3, x2: 3 or 2 prefixes'

WIDTH = 860
FONT = 'font-family="Helvetica, Arial, sans-serif"'
INK, MUTED, GRID, BG, BORDER = '#24292f', '#57606a', '#d8dee4', '#ffffff', '#d0d7de'


def num(x):
    s = f'{x:.1f}'
    return s[:-2] if s.endswith('.0') else s


class Svg:
    def __init__(self, title, desc):
        self.title, self.desc, self.parts = title, desc, []

    def text(self, x, y, s, size=12, anchor='start', fill=INK, weight='normal', halo=False):
        w = f' font-weight="{weight}"' if weight != 'normal' else ''
        h = f' stroke="{BG}" stroke-width="3" paint-order="stroke"' if halo else ''
        self.parts.append(f'<text x="{num(x)}" y="{num(y)}" font-size="{size}" text-anchor="{anchor}" fill="{fill}"{w}{h}>{escape(s)}</text>')

    def rect(self, x, y, w, h, fill, stroke=None, dash=False):
        s = f' stroke="{stroke}" stroke-width="1.5"' if stroke else ''
        d = ' stroke-dasharray="4 3"' if dash else ''
        self.parts.append(f'<rect x="{num(x)}" y="{num(y)}" width="{num(w)}" height="{num(h)}" fill="{fill}"{s}{d}/>')

    def line(self, x1, y1, x2, y2, stroke=GRID, width=1, dash=False):
        d = ' stroke-dasharray="4 3"' if dash else ''
        self.parts.append(f'<line x1="{num(x1)}" y1="{num(y1)}" x2="{num(x2)}" y2="{num(y2)}" stroke="{stroke}" stroke-width="{width}"{d}/>')

    def caption(self, y, s, note=None):
        assert max(len(s), len(note or '')) * 11 * 0.56 < WIDTH - 40, f'caption too long: {s}'
        if note:
            self.text(20, y - 18, note, 11, fill=MUTED)
        self.text(20, y, s, 11, fill=MUTED)

    def render(self, height):
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" viewBox="0 0 {WIDTH} {height}" '
                f'role="img" aria-labelledby="t d" {FONT}>\n<title id="t">{escape(self.title)}</title>\n<desc id="d">{escape(self.desc)}</desc>\n'
                f'<rect x="0.5" y="0.5" width="{WIDTH - 1}" height="{height - 1}" rx="6" fill="{BG}" stroke="{BORDER}"/>\n')
        return head + '\n'.join(self.parts) + '\n</svg>\n'


def load(path):
    """(dataset, tier, engine, class, variant, metric) -> (value, min, max); value None for NA / error."""
    rows = {}
    with open(path) as f:
        for line in f:
            if line.startswith('#') or line.startswith('dataset\t') or not line.strip():
                continue
            d, tier, eng, cls, var, metric, val, lo, hi, _unit = line.rstrip('\n').split('\t')
            rows[(d, tier, eng, cls, var, metric)] = (None if val in ('NA', 'error') else float(val), lo, hi)
    return rows


def value(rows, tier, dataset, engine, cls, variant, metric):
    r = rows.get((dataset, tier, engine, cls, variant, metric))
    return None if r is None else r[0]


def load_range(rows, tier, datasets):
    lo = min(float(rows[(d, tier, '-', '-', '-', 'load_1m')][1]) for d in datasets)
    hi = max(float(rows[(d, tier, '-', '-', '-', 'load_1m')][2]) for d in datasets)
    return f'{lo:.0f}-{hi:.0f}'


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


def legend(svg, y, engines, pending, baseline=False):
    x = 20
    for e in engines:
        label, color = ENGINES[e]
        svg.rect(x, y - 10, 12, 12, color)
        svg.text(x + 17, y, label, 12)
        x += 17 + len(label) * 7 + 18
    if pending:
        svg.rect(x, y - 10, 12, 12, BG, MUTED, dash=True)
        svg.text(x + 17, y, 'rum: pending (no data yet)', 12, fill=MUTED)
        x += 17 + 26 * 7 + 18
    if baseline:
        svg.line(x, y - 4, x + 18, y - 4, INK, 1.5, dash=True)
        svg.text(x + 23, y, 'pg_search = 1.0x', 12)


def header(svg, title, direction):
    svg.text(20, 30, title, 16, weight='bold')
    svg.text(20, 48, direction, 12, fill=MUTED)


def hbars(rows, tier, metric, title, direction, unit_fmt, axis_unit, caption):
    """One panel per dataset, one horizontal bar per engine; engines without data are labeled pending."""
    panels = []
    for d, spec in DATASETS.items():
        vals = [(e, value(rows, tier, d, e, '-', '-', metric)) for e in spec['engines']]
        panels.append((spec['name'], vals))
    present = [e for _, vals in panels for e, v in vals if v is not None]
    pending = any(v is None for _, vals in panels for e, v in vals if e == 'rum')
    engines = [e for e in ENGINES if e in present]
    svg = Svg(title, '; '.join(f'{name}: ' + ', '.join(f'{ENGINES[e][0]} {unit_fmt(v)}' for e, v in vals if v is not None) for name, vals in panels))
    header(svg, title, direction)
    legend(svg, 72, engines, pending)
    x0, x1, row_h = 150, WIDTH - 130, 28
    y = 100
    for name, vals in panels:
        svg.text(20, y + 4, name, 13, weight='bold')
        y += 14
        step, cnt = nice_axis(max(v for _, v in vals if v is not None) * 1.25)
        top, bottom = y, y + row_h * len(vals)
        scale = (x1 - x0) / (step * cnt)
        for i in range(cnt + 1):
            gx = x0 + i * step * scale
            svg.line(gx, top, gx, bottom + 4)
            svg.text(gx, bottom + 17, tick(i * step), 10, 'middle', MUTED)
        svg.text(x1 + 20, bottom + 17, axis_unit, 10, 'start', MUTED)
        for i, (e, v) in enumerate(vals):
            ry = top + i * row_h + 4
            svg.text(x0 - 10, ry + 14, ENGINES[e][0], 12, 'end')
            if v is None:
                svg.rect(x0, ry, 150, 20, BG, MUTED, dash=True)
                svg.text(x0 + 8, ry + 14, 'pending: no data yet', 11, fill=MUTED)
                continue
            svg.rect(x0, ry, v * scale, 20, ENGINES[e][1])
            svg.text(x0 + v * scale + 6, ry + 14, unit_fmt(v), 11)
        svg.line(x0, top, x0, bottom + 4, MUTED)
        y = bottom + 42
    svg.caption(y - 6, caption)
    return svg.render(y + 10)


def grouped(title, direction, groups, series, fmt, ylabel, caption, pending, baseline=None, note=None):
    """Vertical grouped bars. groups: [(label, sublabel)]; series: [(engine, [value or None per group])]."""
    svg = Svg(title, '; '.join(f'{g[0]}: ' + ', '.join(f'{ENGINES[e][0]} {fmt(v[i])}' for e, v in series if v[i] is not None)
                               for i, g in enumerate(groups)))
    header(svg, title, direction)
    legend(svg, 72, [e for e, _ in series], pending, baseline is not None)
    left, right, top, ph = 74, 24, 100, 270
    pw = WIDTH - left - right
    vmax = max(v for _, vs in series for v in vs if v is not None)
    step, cnt = nice_axis(max(vmax, baseline or 0) * 1.15)
    scale = ph / (step * cnt)
    for i in range(cnt + 1):
        gy = top + ph - i * step * scale
        svg.line(left, gy, left + pw, gy)
        svg.text(left - 8, gy + 4, tick(i * step), 10, 'end', MUTED)
    svg.parts.append(f'<text transform="translate(18 {num(top + ph / 2)}) rotate(-90)" font-size="11" text-anchor="middle" fill="{MUTED}">{escape(ylabel)}</text>')
    gw = pw / len(groups)
    bw = min(30, gw * 0.84 / len(series))
    for gi, (label, sub) in enumerate(groups):
        gx = left + gi * gw + (gw - bw * len(series)) / 2
        for si, (e, vs) in enumerate(series):
            v = vs[gi]
            if v is None:
                continue
            h = v * scale
            svg.rect(gx + si * bw + 1, top + ph - h, bw - 2, h, ENGINES[e][1])
            svg.text(gx + si * bw + bw / 2, top + ph - h - 5, fmt(v), 10, 'middle', halo=True)
        cx = left + gi * gw + gw / 2
        svg.text(cx, top + ph + 18, label, 12, 'middle', weight='bold')
        if sub:
            svg.text(cx, top + ph + 32, sub, 10, 'middle', MUTED)
    if baseline is not None:
        by = top + ph - baseline * scale
        svg.line(left, by, left + pw, by, INK, 1.5, dash=True)
    svg.line(left, top + ph, left + pw, top + ph, MUTED)
    bottom = top + ph + (74 if note else 56)
    svg.caption(bottom - 8, caption, note)
    return svg.render(bottom + 8)


def caption(rows, tier, datasets, what):
    return f'{what}; {HOST} (load avg {load_range(rows, tier, datasets)}); {DATE}.'


def make(rows, tier):
    out = {}
    both = list(DATASETS)
    cap_build = caption(rows, tier, both, 'Full-size runs, one build per engine')
    out['index-size.svg'] = hbars(rows, tier, 'index_mib', 'Index size', 'lower is better', lambda v: f'{v:.1f} MiB', 'MiB', cap_build)
    out['build-time.svg'] = hbars(rows, tier, 'build_s', 'Index build time (CREATE INDEX)', 'lower is better', lambda v: f'{v:.1f} s', 'seconds', cap_build)

    a = DATASETS['articles']
    groups = [(label, '') for _, label in ARTICLE_CLASSES]
    qps = {e: [value(rows, tier, 'articles', e, c, '-', 'qps') for c, _ in ARTICLE_CLASSES] for e in a['engines']}
    engines = [e for e in a['engines'] if any(v is not None for v in qps[e])]
    pending = 'rum' not in engines
    cap = caption(rows, tier, ['articles'], f"{a['name']}, 8 clients, median of {a['rounds']} rounds")
    out['articles-throughput.svg'] = grouped('Throughput by workload', 'queries per second, higher is better', groups,
                                             [(e, qps[e]) for e in engines], lambda v: f'{v:,.0f}', 'queries per second', cap, pending)
    ratio = {e: [None if q is None else q / b for q, b in zip(qps[e], qps['pg_search'])] for e in engines if e != 'pg_search'}
    out['articles-speedup.svg'] = grouped('Throughput relative to pg_search', 'queries per second divided by pg_search, higher is better', groups,
                                          [(e, ratio[e]) for e in ratio], lambda v: f'{v:.2f}x', 'speedup over pg_search', cap, pending, baseline=1.0)

    p = DATASETS['places']
    lat = {e: [value(rows, tier, 'places', e, c, 'top', 'p50_ms') for c in PLACE_CLASSES] for e in p['engines']}
    engines = [e for e in p['engines'] if any(v is not None for v in lat[e])]
    cap = caption(rows, tier, ['places'], f"{p['name']}, top-20, 1 client, median of {p['rounds']} rounds")
    out['places-latency.svg'] = grouped('Autocomplete latency by query class', 'p50 milliseconds, lower is better', [(c, '') for c in PLACE_CLASSES],
                                        [(e, lat[e]) for e in engines], lambda v: f'{v:.2f}', 'p50 latency (ms)', cap, 'rum' not in engines, note=PLACE_KEY)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'docs', 'results'))
    ap.add_argument('--out', default=os.path.join(REPO, 'docs', 'img'))
    ap.add_argument('--tier', default='full')
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
