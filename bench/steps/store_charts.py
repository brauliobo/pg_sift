"""SVG charts of the aggregate benchmarks (docs/results/store.json, duckdb.tsv, store-scaling.json, scaling.tsv), standard library only.

Called by steps/charts.py (bench/bench charts; scripts/gen-docs.py). Every chart is written twice, `NAME-light.svg` and `NAME-dark.svg`, each with
the surface of its GitHub theme (README.md picks one with <picture>); the palette is the validated one of the dataviz guidance, the systems keep their
colour and marker everywhere, and every chart names its data file and the host it was measured on. Same results, same bytes.

  store-cpus         1 CPU against 3 CPUs: what each system gains from more CPUs
  store-size         bytes on disk per system, the heap and primary key apart from the index or columnar data, with the load time
  store-updates      size after the changes and after the maintenance step, against before
  scaling-partitions, scaling-workers   pg_sift against PostgreSQL by number of partitions and parallel workers (docs/results/scaling.tsv)
"""
import csv
import json
import math
import os
import re
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
QUERIES = os.path.join(os.path.dirname(HERE), 'sql', 'store', 'queries')

SEARCH = ('08', '09', '10')   # the full-text questions of the store set: measured with the search benchmarks, not part of the aggregate set
WIDTH = 880
FONT = 'font-family="Helvetica, Arial, sans-serif"'

THEMES = {
    'light': dict(bg='#ffffff', border='#d1d9e0', ink='#1f2328', muted='#59636e', grid='#e3e7eb', neutral='#eef0f2', up='#2a78d6', down='#eb6834'),
    'dark': dict(bg='#0d1117', border='#30363d', ink='#e6edf3', muted='#9198a1', grid='#262c34', neutral='#21262d', up='#3987e5', down='#d95926'),
}

# id -> (label, light, dark, marker); the colours are slots of the validated palette (light and dark steps), PostgreSQL is the neutral baseline
SYSTEMS = {
    'pg':         ('PostgreSQL', '#6e7781', '#8b949e', 'square'),
    'sift':       ('pg_sift sift_col', '#2a78d6', '#3987e5', 'circle'),
    'sift_table': ('pg_sift sift_table', '#1baf7a', '#199e70', 'diamond'),
    'es':         ('Elasticsearch', '#6250d6', '#9085e9', 'up'),
    'ch':         ('ClickHouse', '#eda100', '#c98500', 'down'),
    'citus':      ('Citus columnar', '#e87ba4', '#d55181', 'cross'),
    'flat':       ('PostgreSQL flat tables', '#8a5a2b', '#d4a373', 'diamond'),
    'duckdb_heap': ('pg_duckdb heap', '#eb6834', '#d95926', 'up'),
    'duckdb_parquet': ('pg_duckdb Parquet', '#eb6834', '#d95926', 'down'),
}
ORDER = ['pg', 'sift', 'sift_table', 'es', 'ch', 'citus']
SHORT = {'pg': 'PostgreSQL', 'sift': 'sift_col', 'sift_table': 'sift_table', 'es': 'Elastic', 'ch': 'ClickHouse', 'citus': 'Citus'}


def num(x):
    s = f'{x:.1f}'
    return s[:-2] if s.endswith('.0') else s


def ms_text(v):
    return f'{v:,.0f}' if v >= 100 else f'{v:.3g}'


def factor_text(r):
    return f'{r:.0f}×' if r >= 9.5 else f'{r:.1f}×' if r >= 1 else f'{r:.2f}×'


def tw(s, size):
    """the width of a text in pixels, an estimate for Helvetica"""
    return len(s) * size * 0.56


class Canvas:
    def __init__(self, theme):
        self.k, self.t, self.parts = theme, THEMES[theme], []

    def color(self, sid):
        return SYSTEMS[sid][1 if self.k == 'light' else 2]

    def text(self, x, y, s, size=12, anchor='start', fill=None, weight=None):
        w = f' font-weight="{weight}"' if weight else ''
        self.parts.append(f'<text x="{num(x)}" y="{num(y)}" font-size="{size}" text-anchor="{anchor}" fill="{fill or self.t["ink"]}"{w}>{escape(s)}</text>')

    def rect(self, x, y, w, h, fill, rx=0, title=None):
        t = f'<title>{escape(title)}</title>' if title else ''
        self.parts.append(f'<rect x="{num(x)}" y="{num(y)}" width="{num(w)}" height="{num(h)}" fill="{fill}" rx="{rx}">{t}</rect>' if t else
                          f'<rect x="{num(x)}" y="{num(y)}" width="{num(w)}" height="{num(h)}" fill="{fill}" rx="{rx}"/>')

    def line(self, x1, y1, x2, y2, stroke=None, width=1, dash=False, opacity=None):
        d = ' stroke-dasharray="4 4"' if dash else ''
        o = f' stroke-opacity="{opacity}"' if opacity else ''
        self.parts.append(f'<line x1="{num(x1)}" y1="{num(y1)}" x2="{num(x2)}" y2="{num(y2)}" stroke="{stroke or self.t["grid"]}" stroke-width="{width}"{d}{o}/>')

    def polyline(self, pts, stroke, width=2):
        self.parts.append('<polyline fill="none" stroke="%s" stroke-width="%s" stroke-linejoin="round" stroke-linecap="round" points="%s"/>' % (
            stroke, width, ' '.join(f'{num(x)},{num(y)}' for x, y in pts)))

    def marker(self, x, y, sid, title=None, r=4.5):
        """a dot of the system's shape and colour with a 2px ring of the surface colour"""
        c, bg, shape = self.color(sid), self.t['bg'], SYSTEMS[sid][3]
        ring = f'stroke="{bg}" stroke-width="2" paint-order="stroke"'
        t = f'<title>{escape(title)}</title>' if title else ''
        if shape == 'circle':
            el = f'<circle cx="{num(x)}" cy="{num(y)}" r="{r}" fill="{c}" {ring}>{t}</circle>'
        elif shape == 'square':
            el = f'<rect x="{num(x - r)}" y="{num(y - r)}" width="{num(2 * r)}" height="{num(2 * r)}" fill="{c}" {ring}>{t}</rect>'
        elif shape == 'cross':
            d = r * 0.9
            el = (f'<g stroke="{c}" stroke-width="2.5" stroke-linecap="round">{t}<line x1="{num(x - d)}" y1="{num(y - d)}" x2="{num(x + d)}" y2="{num(y + d)}"/>'
                  f'<line x1="{num(x - d)}" y1="{num(y + d)}" x2="{num(x + d)}" y2="{num(y - d)}"/></g>')
        else:
            pts = {'diamond': [(0, -r * 1.3), (r * 1.3, 0), (0, r * 1.3), (-r * 1.3, 0)], 'up': [(0, -r * 1.2), (r * 1.2, r), (-r * 1.2, r)],
                   'down': [(0, r * 1.2), (r * 1.2, -r), (-r * 1.2, -r)]}[shape]
            el = f'<polygon points="{" ".join(f"{num(x + a)},{num(y + b)}" for a, b in pts)}" fill="{c}" {ring}>{t}</polygon>'
        self.parts.append(el)

    def legend(self, x, y, sids, labels=None):
        """one row of marker and name; returns the y of the next free line"""
        for sid in sids:
            label = (labels or {}).get(sid) or SYSTEMS[sid][0]
            if x + 18 + tw(label, 12) > WIDTH - 20:
                x, y = 20, y + 20
            self.marker(x + 5, y - 4, sid)
            self.text(x + 16, y, label, 12, fill=self.t['ink'])
            x += 16 + tw(label, 12) + 16
        return y

    def head(self, title, sub):
        self.text(20, 30, title, 16, weight='bold')
        self.text(20, 48, sub, 12, fill=self.t['muted'])

    def foot(self, y, lines):
        wrapped = []
        for s in lines:
            cur = ''
            for w in s.split(' '):
                if cur and tw(cur + ' ' + w, 11) > WIDTH - 40:
                    wrapped.append(cur)
                    cur = w
                else:
                    cur = (cur + ' ' + w).strip()
            wrapped.append(cur)
        for i, s in enumerate(wrapped):
            self.text(20, y + 16 * i, s, 11, fill=self.t['muted'])
        return y + 16 * len(wrapped)

    def render(self, height, title, desc):
        bg, border = self.t['bg'], self.t['border']
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" viewBox="0 0 {WIDTH} {height}" role="img" aria-labelledby="t d" {FONT}>\n'
                f'<title id="t">{escape(title)}</title>\n<desc id="d">{escape(desc)}</desc>\n<rect x="0.5" y="0.5" width="{WIDTH - 1}" height="{height - 1}" rx="6" fill="{bg}" stroke="{border}"/>\n'
                + '\n'.join(self.parts) + '\n</svg>\n')


def mix(a, b, t):
    pa, pb = [int(a[i:i + 2], 16) for i in (1, 3, 5)], [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return '#' + ''.join(f'{round(x + (y - x) * t):02x}' for x, y in zip(pa, pb))


def luminance(c):
    f = lambda v: (v / 255 / 12.92) if v / 255 <= 0.03928 else ((v / 255 + 0.055) / 1.055) ** 2.4
    r, g, b = [f(int(c[i:i + 2], 16)) for i in (1, 3, 5)]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


# ---- the results -------------------------------------------------------------------------------------------------------------------------

class Store:
    """docs/results/store.json (and store-scaling.json): rows by (tier, engine, class, variant, metric)"""

    def __init__(self, *paths):
        self.rows, self.meta, self.tiers = {}, None, {}
        for p in paths:
            d = json.load(open(p))
            self.meta = self.meta or d['meta']
            self.tiers.update(d['meta']['tiers'])
            for r in d['rows']:
                self.rows[(r['tier'], r['engine'], r['class'], r['variant'], r['metric'])] = r
        self.queries = sorted({k[2] for k in self.rows if k[4] == 'latency_ms' and k[2] not in ('-',) + SEARCH})

    def get(self, tier, engine, cls='-', variant='-', metric=None):
        return self.rows.get((tier, engine, cls, variant, metric))

    def v(self, *a, **k):
        r = self.get(*a, **k)
        return r['value'] if r else None

    def lat(self, tier, engine, q, variant='warm'):
        return self.get(tier, engine, q, variant, 'latency_ms')

    def tier_of(self, rows, cpus):
        """the result set with that many rows and CPUs ('1M-c3')"""
        return next((t for t, m in self.tiers.items() if m['rows'] == rows and m.get('cpus') == cpus), None)

    def engines(self, tier):
        return [e for e in ORDER if any(k[0] == tier and k[1] == e and k[4] == 'latency_ms' for k in self.rows)]

    def host(self):
        h = self.meta['host']
        return f'{h["cpu"].replace(" 8-Core Processor", "")}, {h["threads"]} threads'


def picture(name, alt, prefix='docs/img/'):
    """a chart in its light and dark variant (the one of the reader's GitHub theme is shown); prefix: where the images are, seen from the page"""
    return f'<picture><source media="(prefers-color-scheme: dark)" srcset="{prefix}{name}-dark.svg"><img alt="{alt}" src="{prefix}{name}-light.svg"></picture>'


def question_slugs():
    """Q01 point lookup ...: the names of the query files"""
    return {m.group(1): m.group(2).replace('_', ' ') for f in sorted(os.listdir(QUERIES)) if (m := re.match(r'(\d+)_(\w+)\.sql$', f))}


def source_lines(S, tier, what, files='docs/results/store.json'):
    t = S.tiers[tier]
    s = t['servers']
    return [f'Data: {files}, {t["parts"]["warm"]["date"][:10]}. Host: {S.host()}, {S.meta["host"]["memory"]} per server.',
            f'PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]}, Elasticsearch {s["es"]}, ClickHouse {s["ch"]}, Citus {s["citus_columnar"]}. {what}']


# ---- charts --------------------------------------------------------------------------------------------------------------------------------

def strip_color(c, r, span=2.0):
    """a factor over the baseline: blue when faster, orange when slower, gray around 1 (log scale, saturated at 10^span)"""
    t = min(1, abs(math.log10(r)) / span)
    if abs(math.log10(r)) < math.log10(1.15):
        return c.t['neutral']
    return mix(c.t['neutral'], c.t['up'] if r > 1 else c.t['down'], 0.18 + 0.82 * t)


def strip_cells(c, x, y, cols, rows_, cw, rh, title_fn):
    """cells[(row, col)] = ratio or None (n/a); draws the cells with their factor"""
    for ri, row in enumerate(rows_):
        for ci, col in enumerate(cols):
            r = row[ci]
            cx, cy = x + ci * cw, y + ri * rh
            if r is None:
                c.rect(cx + 1, cy + 1, cw - 2, rh - 2, c.t['bg'], 3)
                c.text(cx + cw / 2, cy + rh / 2 + 4, 'n/a', 11, 'middle', c.t['muted'])
                continue
            fill = strip_color(c, r)
            ink = '#1f2328' if luminance(fill) > 0.28 else '#ffffff'
            c.rect(cx + 1, cy + 1, cw - 2, rh - 2, fill, 3, title_fn(ri, ci, r))
            c.text(cx + cw / 2, cy + rh / 2 + 4, factor_text(r), 12, 'middle', ink, 'bold' if r < 0.95 else None)


def cpus_chart(S, theme):
    c, slug = Canvas(theme), question_slugs()
    t1, t3 = S.tier_of(1_000_000, 1), S.tier_of(1_000_000, 3)
    engines = [e for e in S.engines(t3) if e in S.engines(t1)]
    qs = S.queries
    c.head('What a second and third CPU buys', 'warm median at 1 CPU divided by the median at 3 CPUs per server; 1× means no gain, 3× a perfect one, orange: slower with 3 CPUs')
    left, cw, rh, top = 200, 100, 28, 80
    for ci, e in enumerate(engines):
        c.marker(left + ci * cw + 10, top - 14, e)
        c.text(left + ci * cw + 20, top - 10, SYSTEMS[e][0].replace('pg_sift ', ''), 11)
    grid = []
    for q in qs:
        grid.append([(S.lat(t1, e, q)['value'] / S.lat(t3, e, q)['value']) if S.lat(t1, e, q) and S.lat(t3, e, q) else None for e in engines])
        c.text(22, top + len(grid) * rh - rh / 2 + 4, f'Q{q} {slug[q]}', 12)
    for ri, row in enumerate(grid):
        for ci, r in enumerate(row):
            if r is None:
                continue
            fill = strip_color(c, r, 0.6)
            ink = '#1f2328' if luminance(fill) > 0.28 else '#ffffff'
            c.rect(left + ci * cw + 1, top + ri * rh + 1, cw - 2, rh - 2, fill, 3, f'Q{qs[ri]} {slug[qs[ri]]}: {SYSTEMS[engines[ci]][0]} takes {1 / r:.2f}× the time with 3 CPUs that it takes with 1')
            c.text(left + ci * cw + cw / 2, top + ri * rh + rh / 2 + 4, f'{r:.2f}×' if r < 0.95 else f'{r:.1f}×', 12, 'middle', ink, 'bold' if r < 0.95 else None)
    worst = min((r, ri, ci) for ri, row in enumerate(grid) for ci, r in enumerate(row) if r)
    odd = f' {SYSTEMS[engines[worst[2]]][0]} Q{qs[worst[1]]} takes {1 / worst[0]:.0f}x as long at 3 CPUs: the planner chose a different plan.' if worst[0] < 0.5 else ''
    end = c.foot(top + rh * len(qs) + 24, source_lines(S, t3, 'Questions that finish in about a millisecond have nothing left to split across CPUs.' + odd))
    return c.render(end + 6, 'Gain from 1 to 3 CPUs per question', 'Warm median at 1 CPU divided by the median at 3 CPUs, per question and system.')


def sizes(S, tier, e):
    """(heap and primary key, index or columnar data, total) in MiB; the first None when the system holds everything itself"""
    total = S.v(tier, e, metric='size_mib')
    table = S.v(tier, e, variant='table', metric='size_mib')
    if table is None:
        return None, total, total
    free = S.v(tier, e, variant='free', metric='size_mib') or 0
    idx = {k[3][6:]: r['value'] for k, r in S.rows.items() if k[0] == tier and k[1] == e and k[4] == 'size_mib' and k[3].startswith('index ')}
    pk = idx.pop('orders_pkey')
    return (table + pk, sum(idx.values()), total) if e in ('pg', 'sift') else (pk, table - free + sum(idx.values()), total)


def size_chart(S, theme, tier):
    c = Canvas(theme)
    engines = S.engines(tier)
    c.head('Size on disk and load time, 1,000,000 orders', 'MiB; the heap and primary key apart from the index or columnar data the system keeps beside them')
    top, rh, left, right = 104, 40, 170, 130
    data = {e: sizes(S, tier, e) for e in engines}
    vmax = max(d[2] for d in data.values())
    pw = WIDTH - left - right
    X = lambda v: v / vmax * pw
    c.rect(20, 66, 12, 12, c.t['muted'], 2)
    c.text(38, 77, 'heap and primary key', 12)
    c.marker(190, 72, 'sift')
    c.text(202, 77, 'index or columnar data (system colour)', 12)
    for i, e in enumerate(engines):
        y = top + i * rh
        heap, idx, total = data[e]
        c.text(20, y + 21, SYSTEMS[e][0], 12)
        x = left
        if heap:
            c.rect(x, y + 6, X(heap) - 1, 22, mix(c.t['muted'], c.t['bg'], 0.45), 3, f'{SYSTEMS[e][0]}: heap and primary key {heap:.1f} MiB')
            x += X(heap)
        c.rect(x, y + 6, X(idx), 22, c.color(e), 3, f'{SYSTEMS[e][0]}: index or columnar data {idx:.1f} MiB')
        load = S.v(tier, e, metric='load_s')
        c.text(left + X(total) + 8, y + 22, f'{total:,.0f} MiB', 12, fill=c.t['ink'], weight='bold')
        c.text(WIDTH - 20, y + 22, f'load {load:.1f} s' if load is not None else '', 12, 'end', c.t['muted'])
    note = ['sift_col shares PostgreSQL\'s heap and primary key: its column index and its text index are what it adds.']
    end = c.foot(top + rh * len(engines) + 16, note + source_lines(S, tier, 'Load: data, indexes, merge, one client.'))
    return c.render(end + 6, 'Size on disk and load time', 'Bytes on disk per system, heap and primary key against index or columnar data, with the load time.')


def updates_chart(S, theme, tier):
    c = Canvas(theme)
    steps = (('after churn', 'after the changes', 'circle'), ('after maintenance', 'after VACUUM / merge / OPTIMIZE', 'diamond'))
    engines = [e for e in ('pg', 'sift', 'sift_table', 'es', 'ch', 'ch_mut', 'ch_rmt') if S.v(tier, e, '-', 'before', 'size_mib')]
    names = {'ch': 'ClickHouse, lightweight UPDATE', 'ch_mut': 'ClickHouse, ALTER UPDATE', 'ch_rmt': 'ClickHouse, ReplacingMergeTree'}
    label = lambda e: names.get(e) or SYSTEMS[e][0]
    c.head('Size after 1% and 10% updated and 5% deleted', 'size divided by the size before the changes (1× = no growth); the maintenance step is VACUUM, merge or OPTIMIZE')
    left, right, top, rh = 230, 40, 100, 34
    pw = WIDTH - left - right
    lo, hi = 0.8, 1.7
    X = lambda v: left + (v - lo) / (hi - lo) * pw
    c.marker(20 + 5, 72, 'sift')
    c.text(36, 77, steps[0][1], 12)
    c.marker(240, 72, 'sift_table')
    c.text(252, 77, steps[1][1], 12)
    for v in (0.8, 1.0, 1.2, 1.4, 1.6):
        c.line(X(v), top - 6, X(v), top + rh * len(engines), c.t['muted'] if v == 1.0 else None)
        c.text(X(v), top - 12, f'{v:g}×', 11, 'middle', c.t['muted'])
    for i, e in enumerate(engines):
        cy = top + i * rh + rh / 2
        c.text(20, cy + 4, label(e), 12)
        vals = [(S.v(tier, e, '-', k, 'size_mib') / S.v(tier, e, '-', 'before', 'size_mib'), k, shape) for k, _, shape in steps]
        c.line(X(min(v for v, _, _ in vals)), cy, X(max(v for v, _, _ in vals)), cy, c.t['muted'], 2, opacity=0.5)
        for (v, k, shape), dy in zip(vals, (-6, 6)):
            sid = 'sift' if shape == 'circle' else 'sift_table'
            c.marker(X(v), cy + dy, sid, f'{label(e)} {k}: {v:.2f}× the size before ({S.v(tier, e, "-", k, "size_mib"):.0f} MiB against {S.v(tier, e, "-", "before", "size_mib"):.0f} MiB)')
        c.text(X(max(v for v, _, _ in vals)) + 12, cy + 4, ' → '.join(f'{v:.2f}×' for v, _, _ in vals), 11, fill=c.t['muted'])
    end = c.foot(top + rh * len(engines) + 16, source_lines(S, tier, 'Sizes are the files on disk after each step; the two numbers beside a row are the ratios after the changes and after maintenance.'))
    return c.render(end + 6, 'Size growth under updates', 'Size after the changes and after the maintenance step against before, per system.')


def read_tsv(path):
    meta, lines = {}, []
    for line in open(path):
        if line.startswith('#'):
            m = re.match(r'# ([\w ]+): (.*)', line.rstrip('\n'))
            if m:
                meta[m.group(1)] = m.group(2)
        elif line.strip():
            lines.append(line.rstrip('\n'))
    return meta, list(csv.DictReader(lines, delimiter='\t'))


def lines_panels(c, panels, xs, top, ncols=5, pw=150, ph=88, xfmt=None, yfmt=ms_text):
    """small multiples: panels [(title, {sid: [y per x or None]})], log-log lines; shared x positions xs; y axis per panel"""
    lx = lambda v: math.log10(v)
    gx = (WIDTH - 40) / ncols
    for i, (title, data) in enumerate(panels):
        px, py = 20 + (i % ncols) * gx + 34, top + (i // ncols) * (ph + 62)
        vals = [v for ys in data.values() for v in ys if v]
        lo, hi = 10 ** math.floor(math.log10(min(vals))), 10 ** math.ceil(math.log10(max(vals)))
        if hi == lo:
            hi = lo * 10
        Y = lambda v: py + ph - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * ph
        X = lambda v: px + (lx(v) - lx(xs[0])) / (lx(xs[-1]) - lx(xs[0])) * (pw - 20)
        c.text(px - 30, py - 12, title, 11, fill=c.t['ink'], weight='bold')
        d = lo
        while d <= hi * 1.001:
            c.line(px, Y(d), px + pw - 14, Y(d))
            c.text(px - 4, Y(d) + 3, yfmt(d), 9, 'end', c.t['muted'])
            d *= 10
        for sid, ys in data.items():
            pts = [(X(x), Y(y)) for x, y in zip(xs, ys) if y]
            if len(pts) > 1:
                c.polyline(pts, c.color(sid), 2)
            for (x, y), xv, yv in zip(pts, [x for x, y in zip(xs, ys) if y], [y for y in ys if y]):
                c.marker(x, y, sid, f'{title}: {SYSTEMS[sid][0]} {yfmt(yv)} ms at {xfmt(xv) if xfmt else xv}', r=3.5)
        for xv in (xs[0], xs[len(xs) // 2], xs[-1]):
            c.text(X(xv), py + ph + 14, xfmt(xv) if xfmt else f'{xv:,}', 9, 'middle', c.t['muted'])
    return top + ((len(panels) - 1) // ncols + 1) * (ph + 62)


def rows_label(v):
    return f'{v / 1_000_000:g}M' if v >= 1_000_000 else f'{v / 1000:g}k'


def scaling_tsv_chart(theme, path, dim, title, sub):
    """pg_sift against PostgreSQL by partitions or workers (docs/results/scaling.tsv): one panel per question"""
    c = Canvas(theme)
    meta, rows = read_tsv(path)
    rows = [r for r in rows if r['case'].startswith(dim + ' ') and r['metric'] == 'median_ms']
    xs = sorted({int(r['case'].split()[1]) for r in rows}, key=int)
    qs = sorted({r['arm'].split('_')[0] for r in rows})
    names = {'q02': 'Q02 orders per country', 'q03': 'Q03 revenue per month', 'q04': 'Q04 top categories', 'q06': 'Q06 customers per month', 'q05': 'Q05 average shipped order', 'q01': 'Q01 point lookup'}
    c.head(title, sub)
    y = c.legend(20, 74, ['pg', 'sift'])
    panels = []
    for q in qs:
        data = {}
        for sid in ('pg', 'sift'):
            data[sid] = [next((float(r['value']) for r in rows if r['case'] == f'{dim} {x}' and r['arm'] == f'{q}_{sid}'), None) for x in xs]
        panels.append((names.get(q, q), data))
    end = lines_panels(c, panels, [max(x, 1) for x in xs], y + 36, ncols=min(4, len(panels)), pw=190, xfmt=lambda v: f'{v:,}')
    end = c.foot(end - 22, [f'Data: docs/results/scaling.tsv, {meta.get("date", "?")}. Host: {meta.get("cpu", "?").split(" (")[0].replace(" 8-Core Processor", "")}, {meta.get("postgresql", "PostgreSQL")}, pg_sift {meta.get("sift", "?")}.',
                            meta.get('method', '')[:230]])
    return c.render(end + 6, title, sub)


# ---- the set ---------------------------------------------------------------------------------------------------------------------------

def make(results):
    """{file name: svg} of every chart whose results exist in the directory"""
    out = {}
    store = os.path.join(results, 'store.json')
    if not os.path.exists(store):
        return out
    S = Store(store)
    tier = S.tier_of(1_000_000, 3)
    for theme in THEMES:
        add = lambda name, svg: out.__setitem__(f'{name}-{theme}.svg', svg)
        if S.tier_of(1_000_000, 1):
            add('store-cpus', cpus_chart(S, theme))
        add('store-size', size_chart(S, theme, tier))
        add('store-updates', updates_chart(S, theme, tier))
        sc = os.path.join(results, 'scaling.tsv')
        if os.path.exists(sc):
            add('scaling-partitions', scaling_tsv_chart(theme, sc, 'partitions', 'Aggregates by number of partitions, 1,000,000 orders', 'median milliseconds, logarithmic axes; the table is split by a hash of the id into N partitions'))
            add('scaling-workers', scaling_tsv_chart(theme, sc, 'workers', 'Aggregates by number of parallel workers, 1,000,000 orders', 'median milliseconds, logarithmic axes; max_parallel_workers_per_gather = N'))
    return out
