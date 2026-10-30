# The charts of scale: growth against the rows with the log-log slope, and one panel for each question.
# slope() is the fit the README quotes.

import math
import statistics

from store_charts import SYSTEMS, ms_text

from .base import ORDER, SHORT, SYSTEMS, W, Wide
from .results import load, questions


# ---- scale ------------------------------------------------------------------------------------------------------------------------------

def slope(xs, ys):
    """the least-squares slope of log y on log x"""
    pts = [(math.log10(x), math.log10(y)) for x, y in zip(xs, ys) if x and y]
    if len(pts) < 2:
        return None
    mx, my = statistics.mean(p[0] for p in pts), statistics.mean(p[1] for p in pts)
    return sum((a - mx) * (b - my) for a, b in pts) / sum((a - mx) ** 2 for a, b in pts)


def scale_points(d):
    """{system: {question: [(rows, ms)]}} from store.json, store-scaling.json and the two pg_duckdb runs"""
    SS = d['SS']
    sizes = sorted({m['rows'] for m in SS.tiers.values() if m.get('cpus') == 3})
    tiers = {n: SS.tier_of(n, 3) for n in sizes}
    out = {}
    for e in ORDER:
        if e == 'duckdb':
            continue
        for q in questions(SS):
            pts = [(n, SS.lat(tiers[n], e, q)['value']) for n in sizes if SS.lat(tiers[n], e, q)]
            if len(pts) >= 2:
                out.setdefault(e, {})[q] = pts
    for q in questions(SS):
        pts = [(n, D.best(q)) for n, D in ((1_000_000, d['D1']), (10_000_000, d['D10'])) if D and D.best(q)]
        if len(pts) == 2:
            out.setdefault('duckdb', {})[q] = pts
    return sizes, out


def rows_label(v):
    return f'{v / 1_000_000:g}M' if v >= 1_000_000 else f'{v / 1000:g}k'


def growth_of(pts, sizes, qs):
    """per system: the median over the questions of the time at each size over the time at the smallest"""
    out = {}
    for e in pts:
        if e == 'duckdb':
            continue
        out[e] = [statistics.median([pts[e][q][i][1] / pts[e][q][0][1] for q in qs if q in pts[e] and len(pts[e][q]) == len(sizes)] or [float('nan')]) for i in range(len(sizes))]
    return out


def scale_takeaway(results):
    """the sentence under the scale charts: the growth at the largest size and the log-log slope of the median question, per system"""
    d = load(results)
    if not d['SS']:
        return None
    sizes, pts = scale_points(d)
    g = growth_of(pts, sizes, questions(d['SS']))
    names = {'sift': 'sift_col', 'pg': 'PostgreSQL', 'ch': 'ClickHouse', 'es': 'Elasticsearch', 'citus': 'Citus columnar', 'sift_table': 'sift_table'}
    f = lambda e: f'{g[e][-1]:.0f}x ({slope(sizes, g[e]):.2f})' if g[e][-1] >= 9.5 else f'{g[e][-1]:.1f}x ({slope(sizes, g[e]):.2f})'
    return (f'From {rows_label(sizes[0])} to {rows_label(sizes[-1])} orders ({sizes[-1] // sizes[0]}x the rows) the median question takes {f("sift")} as long on sift_col (log-log slope in parentheses) and {f("sift_table")} on sift_table; '
            f'on PostgreSQL {f("pg")}, ClickHouse {f("ch")}, Elasticsearch {f("es")}, Citus columnar {f("citus")}. A slope of 1 is time proportional to the rows, below it a system grows slower than its data')


def scale_growth(d, theme):
    """per system, the median over the questions of (time at n / time at 25,000 rows) against n, log-log: a slope of 1 is linear in the rows"""
    S = d['SS']
    c = Wide(theme)
    sizes, pts = scale_points(d)
    es = [e for e in ORDER if e in pts and e != 'duckdb']
    qs = questions(S)
    c.head('Growth with the number of orders: 25,000 to 10,000,000 (400x)',
           'For each system, the median over the questions of the time at n rows divided by the time at 25,000 rows, on logarithmic axes. The dashed line is time proportional to the rows (slope 1); below it a system grows slower than its data.')
    left, right, top, ph = 80, 190, 124, 520
    pw = W - left - right
    lo, hi = 0.5, 1000
    X = lambda n: left + (math.log10(n) - math.log10(sizes[0])) / (math.log10(sizes[-1]) - math.log10(sizes[0])) * pw
    Y = lambda v: top + ph - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * ph
    for g in (1, 10, 100, 1000):
        c.line(left, Y(g), left + pw, Y(g))
        c.text(left - 8, Y(g) + 4, f'{g:,}×', 12, 'end', c.t['muted'])
    for n in sizes:
        c.line(X(n), top, X(n), top + ph)
        c.text(X(n), top + ph + 20, rows_label(n), 13, 'middle', c.t['muted'])
    c.text(left + pw / 2, top + ph + 42, 'orders in the table', 12, 'middle', c.t['muted'])
    c.line(X(sizes[0]), Y(1), X(sizes[-1]), Y(sizes[-1] / sizes[0]), c.t['muted'], 1.5, dash=True)
    c.text_box(X(sizes[-1]) - 6, Y(sizes[-1] / sizes[0]) - 8, 'time proportional to the rows', 12, c.t['muted'], anchor='end')
    labels = []
    gr = growth_of(pts, sizes, qs)
    for e in es:
        growth = gr[e]
        line = [(X(n), Y(g)) for n, g in zip(sizes, growth) if g]
        c.polyline(line, c.color(e), 2.5)
        for (n, g), (x, y) in zip([(n, g) for n, g in zip(sizes, growth) if g], line):
            c.marker(x, y, e, f'{SYSTEMS[e][0]}: {g:.1f}× the time of 25,000 rows at {rows_label(n)}', r=5)
        sl = slope(sizes, growth)
        labels.append((line[-1][1], f'{SHORT[e]}  slope {sl:.2f}', e))
    labels.sort()
    prev = -99
    for y, text, e in labels:   # direct labels at the right end, pushed apart
        y = max(y, prev + 20)
        c.text(left + pw + 14, y + 4, text, 13, fill=c.t['ink'], weight='bold')
        prev = y
    end = c.foot(top + ph + 66, [f'Data: bench/results/store-scaling.json and store.json (3 CPUs per server, warm medians of {len(qs)} aggregate questions at each size), {S.host()}. The slope is the least-squares slope of log time on log rows over the four sizes.',
                                 'Questions that finish in a millisecond are dominated by fixed costs at the small sizes, which is why a slope below 1 is expected there; the per-question panels follow.'])
    return c.render(end + 8, 'Growth with the number of orders', 'Median growth of the time per question from 25,000 to 10,000,000 orders, per system, with the log-log slope.')


def scale_questions(d, theme):
    S = d['SS']
    c = Wide(theme)
    sizes, pts = scale_points(d)
    qs = questions(S)
    es = [e for e in ORDER if e in pts]
    c.head('Time per question from 25,000 to 10,000,000 orders',
           'Median warm milliseconds, logarithmic axes. The number after a system is the log-log slope of its line: 1.00 is time proportional to the rows, 0 is flat. pg_duckdb was measured at 1M and 10M only.')
    y = c.legend(20, 96, es)
    ncols, pw, ph, gy = 3, 230, 150, 78
    top = y + 40
    cell = (W - 40) / ncols
    for i, q in enumerate(qs):
        px, py = 20 + (i % ncols) * cell + 48, top + (i // ncols) * (ph + gy)
        vals = [v for e in es if q in pts[e] for _, v in pts[e][q]]
        lo, hi = 10 ** math.floor(math.log10(min(vals))), 10 ** math.ceil(math.log10(max(vals)))
        Y = lambda v: py + ph - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * ph
        X = lambda n: px + (math.log10(n) - math.log10(sizes[0])) / (math.log10(sizes[-1]) - math.log10(sizes[0])) * pw
        c.text(px - 40, py - 14, f'{q}', 14, weight='bold')
        g = lo
        while g <= hi * 1.001:
            c.line(px, Y(g), px + pw, Y(g))
            c.text(px - 6, Y(g) + 4, ms_text(g), 11, 'end', c.t['muted'])
            g *= 10
        for n in sizes:
            c.text(X(n), py + ph + 16, rows_label(n), 11, 'middle', c.t['muted'])
        ends = []
        for e in es:
            if q not in pts[e]:
                continue
            p = pts[e][q]
            line = [(X(n), Y(v)) for n, v in p]
            c.polyline(line, c.color(e), 2)
            for (n, v), (x, yy) in zip(p, line):
                c.marker(x, yy, e, f'{q}: {SYSTEMS[e][0]} {ms_text(v)} ms at {rows_label(n)} orders', r=3.5)
            sl = slope([n for n, _ in p], [v for _, v in p])
            ends.append((line[-1][1], e, sl))
        ends.sort()
        prev = -99
        for yy, e, sl in ends:
            yy = max(yy, prev + 12)
            c.text(px + pw + 5, yy + 4, f'{sl:.2f}', 11, fill=c.t['ink'])
            c.marker(px + pw + 36, yy, e, r=2.5)
            prev = yy
    rows_ = (len(qs) - 1) // ncols + 1
    end = c.foot(top + rows_ * (ph + gy) - 16, [f'Data: bench/results/store.json, store-scaling.json, duckdb.tsv, duckdb-10000000.tsv; {S.host()}; 3 CPUs per server; Elasticsearch with its request cache off.'])
    return c.render(end + 8, 'Time per question by number of orders', 'Median warm latency of the aggregate questions at four table sizes, with the log-log slope of each system.')
