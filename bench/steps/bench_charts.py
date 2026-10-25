"""The charts of the README's benchmarks, standard library only: wide, one message each, direct labels, light and dark.

Called by steps/charts.py (bench/bench charts), which writes every `NAME-light.svg` and `NAME-dark.svg` into docs/img. The systems keep the colour and
marker of store_charts.SYSTEMS in every chart (pg_duckdb is one more system among them); every chart names its data files and the host in its foot. Same results, same bytes.

  agg-latency      the seven aggregate questions, one dot per system on a log axis (store.json, duckdb.tsv)
  agg-speedup      the same medians as a factor over PostgreSQL, warm and cold (duckdb.tsv has its own PostgreSQL for its factor)
  scale-growth     how much longer the median question takes at 400 times the rows, per system, with the log-log slope (store.json, store-scaling.json, duckdb*.tsv)
  scale-questions  the same data, one panel per question, with the slope of each line
  cases-speedup    every case of bench/cases.tsv: PostgreSQL's time over pg_sift's, by family (cases.tsv)
  cases-ecdf       the share of cases (and of the real query shapes) that are at least as fast as x times PostgreSQL
  shapes-speedup   the real query shapes by the size of the filter set (analytics.json)
  cases-scale      time against rows for the case families, with the log-log slope of pg_sift and of PostgreSQL (cases-scale.tsv)
  cases-workmem    pg_sift and PostgreSQL at work_mem 64MB and at a large 4GB (cases-wm4g.tsv)
  cost-per-row     index bytes per row, load time per row and update rate (store.json)
"""
import json
import math
import os
import re
import statistics

import cases as cases_mod
import cases_render
import extras
import store_charts as sc
from store_charts import SEARCH, SYSTEMS, THEMES, Canvas, Store, factor_text, ms_text, num, strip_cells, strip_color, tw

SYSTEMS['duckdb'] = ('pg_duckdb', '#eb6834', '#d95926', 'up')
ORDER = ['pg', 'sift', 'sift_table', 'es', 'ch', 'citus', 'duckdb']
SHORT = dict(sc.SHORT, duckdb='pg_duckdb', es='Elasticsearch')
HEADER = dict(SHORT, es='Elastic', sift='sift_col', sift_table='sift_table')   # the column heads of a heat strip are narrower
W = 1000


class Wide(Canvas):
    """a Canvas that is W pixels wide (the old charts are 880)"""

    def legend(self, x, y, sids, labels=None):
        for sid in sids:
            label = (labels or {}).get(sid) or SYSTEMS[sid][0]
            if x + 18 + tw(label, 13) > W - 20:
                x, y = 20, y + 22
            self.marker(x + 5, y - 4, sid)
            self.text(x + 16, y, label, 13)
            x += 16 + tw(label, 13) + 18
        return y

    def head(self, title, sub):
        self.text(20, 32, title, 18, weight='bold')
        for i, line in enumerate(self.wrap(sub, 13, W - 40)):
            self.text(20, 54 + 18 * i, line, 13, fill=self.t['muted'])
        self.sub_lines = len(self.wrap(sub, 13, W - 40))

    def wrap(self, s, size, width):
        out, cur = [], ''
        for w in s.split(' '):
            if cur and tw(cur + ' ' + w, size) > width:
                out.append(cur)
                cur = w
            else:
                cur = (cur + ' ' + w).strip()
        return out + [cur]

    def foot(self, y, lines):
        n = 0
        for s in lines:
            for line in self.wrap(s, 12, W - 40):
                self.text(20, y + 17 * n, line, 12, fill=self.t['muted'])
                n += 1
        return y + 17 * n

    def render(self, height, title, desc):
        bg, border = self.t['bg'], self.t['border']
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{height}" viewBox="0 0 {W} {height}" role="img" aria-labelledby="t d" {sc.FONT}>\n'
                f'<title id="t">{sc.escape(title)}</title>\n<desc id="d">{sc.escape(desc)}</desc>\n<rect x="0.5" y="0.5" width="{W - 1}" height="{height - 1}" rx="6" fill="{bg}" stroke="{border}"/>\n'
                + '\n'.join(self.parts) + '\n</svg>\n')

    def text_box(self, x, y, s, size=12, fill=None, weight=None, anchor='start'):
        """a text on a surface-coloured plate, so a label over a grid line stays readable"""
        w = tw(s, size) + 6
        x0 = x - w / 2 if anchor == 'middle' else x - w + 3 if anchor == 'end' else x - 3
        self.rect(x0, y - size, w, size + 5, self.t['bg'], 3)
        self.text(x, y, s, size, anchor, fill, weight)


def dot(c, x, y, faster, title, r=6):
    """a dot in the colour of the outcome (blue: faster, orange: slower), ringed with the surface colour: no system owns it"""
    fill = c.t['up'] if faster else c.t['down']
    t = f'<title>{sc.escape(title)}</title>'
    shape = (f'<circle cx="{num(x)}" cy="{num(y)}" r="{r}" fill="{fill}" stroke="{c.t["bg"]}" stroke-width="2" paint-order="stroke">{t}</circle>' if faster else
             f'<polygon points="{num(x)},{num(y - r * 1.3)} {num(x + r * 1.2)},{num(y + r)} {num(x - r * 1.2)},{num(y + r)}" fill="{fill}" stroke="{c.t["bg"]}" stroke-width="2" paint-order="stroke">{t}</polygon>')
    c.parts.append(shape)


# ---- the results ------------------------------------------------------------------------------------------------------------------------

class Duck:
    """bench/results/duckdb.tsv (and duckdb-10000000.tsv): pg_duckdb's own run, with its own PostgreSQL and pg_sift next to it"""

    def __init__(self, path):
        self.meta, self.rows = sc.read_tsv(path)
        self.v = {}
        for r in self.rows:
            if r['value'] not in ('NA', 'error', ''):
                self.v[(r['case'].split(' ')[0], r['arm'], r['metric'])] = float(r['value'])

    def best(self, q, metric='warm_ms'):
        vals = [self.v[(q, a, metric)] for a in ('duckdb_heap', 'duckdb_parquet') if (q, a, metric) in self.v]
        return min(vals) if vals else None

    def own(self, q, arm, metric='warm_ms'):
        return self.v.get((q, arm, metric))


def load(results):
    store = os.path.join(results, 'store.json')
    scaling = os.path.join(results, 'store-scaling.json')
    d = {'S': Store(store), 'SS': Store(store, scaling) if os.path.exists(scaling) else None}
    for k, f in (('D1', 'duckdb.tsv'), ('D10', 'duckdb-10000000.tsv')):
        d[k] = Duck(os.path.join(results, f)) if os.path.exists(os.path.join(results, f)) else None
    return d


def questions(S):
    return S.queries


def slug(q):
    return sc.question_slugs()[q]


def engines_of(S, tier, D):
    es = [e for e in ORDER if e in S.engines(tier) or (e == 'duckdb' and D)]
    return es


def lat(S, D, tier, e, q, variant='warm'):
    """(value, min, max) of a system on a question; pg_duckdb: the faster of its two modes"""
    if e == 'duckdb':
        v = D.best(q, 'warm_ms' if variant == 'warm' else 'cold_ms') if D else None
        return (v, v, v) if v else None
    r = S.lat(tier, e, q, variant)
    return (r['value'], r['min'], r['max']) if r else None


def foot_lines(S, tier, D, what):
    t = S.tiers[tier]
    s = t['servers']
    host = S.host()
    out = [f'Data: bench/results/store.json{", duckdb.tsv" if D else ""}, {t["parts"]["warm"]["date"][:10]}, {host}, {S.meta["host"]["memory"]} per server, {t["cpus"]} CPUs.',
           f'PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]}, Elasticsearch {s["es"]}, ClickHouse {s["ch"]}, Citus {s["citus_columnar"]}' + (f', pg_duckdb {D.meta["pg_duckdb"].split(",")[0]}.' if D else '.') + ' ' + what]
    return out


# ---- the aggregate questions ------------------------------------------------------------------------------------------------------------

def agg_latency(d, theme):
    S, D = d['S'], d['D1']
    tier = S.tier_of(1_000_000, 3)
    c = Wide(theme)
    qs, es = questions(S), engines_of(S, tier, D)
    c.head(f'{len(qs)} aggregate questions on 1,000,000 orders: time per question',
           'Median of the warm runs in milliseconds on a logarithmic axis, left is faster; the line of a dot runs from the fastest to the slowest run. Every question is answered by every system.')
    y = c.legend(20, 96, es)
    left, right, rowh, top = 250, 250, 52, y + 50
    pw = W - left - right
    lo, hi = 0.1, 1000
    X = lambda v: left + (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * pw
    for g in (0.1, 1, 10, 100, 1000):
        c.line(X(g), top - 10, X(g), top + rowh * len(qs))
        c.text(X(g), top - 16, f'{g:,g} ms', 12, 'middle', c.t['muted'])
    c.text(W - 20, top - 30, 'best pg_sift layout against the', 11, 'end', c.t['muted'])
    c.text(W - 20, top - 16, 'fastest other system', 11, 'end', c.t['muted'])
    off = {e: (i - (len(es) - 1) / 2) * 5.5 for i, e in enumerate(es)}
    for i, q in enumerate(qs):
        cy = top + rowh * i + rowh / 2
        if i % 2 == 0:
            c.rect(14, cy - rowh / 2, W - 28, rowh, c.t['neutral'], 4)
        vals = {e: lat(S, D, tier, e, q) for e in es}
        c.text(22, cy - 2, f'Q{q} {slug(q)}', 14)
        best_mine = min((e for e in ('sift', 'sift_table') if vals.get(e)), key=lambda e: vals[e][0], default=None)
        if best_mine and vals.get('pg'):
            c.text(22, cy + 15, f'PostgreSQL {ms_text(vals["pg"][0])} ms, {SHORT[best_mine]} {ms_text(vals[best_mine][0])} ms', 11, fill=c.t['muted'])
        for e in es:
            if not vals[e]:
                continue
            v, mn, mx = vals[e]
            ey = cy + off[e]
            c.line(X(max(mn, lo)), ey, X(min(mx, hi)), ey, c.color(e), 2, opacity=0.55)
            c.marker(X(v), ey, e, f'Q{q} {slug(q)}: {SYSTEMS[e][0]} {ms_text(v)} ms (fastest {ms_text(mn)}, slowest {ms_text(mx)})', r=4.5)
        mine = {e: vals[e][0] for e in ('sift', 'sift_table') if vals.get(e)}
        others = {e: vals[e][0] for e in es if e not in ('sift', 'sift_table') and vals[e]}
        if mine and others:
            best, rival = min(mine, key=mine.get), min(others, key=others.get)
            ratio = others[rival] / mine[best]
            word = f'{factor_text(ratio)} faster' if ratio >= 1.05 else 'tie' if ratio > 0.95 else f'{factor_text(1 / ratio)} slower'
            c.text(W - 20, cy + 5, f'{word} than {SHORT[rival]}', 13, 'end', c.t['ink'] if ratio >= 0.95 else c.t['down'], 'bold' if ratio < 0.95 else None)
    end = c.foot(top + rowh * len(qs) + 38, foot_lines(S, tier, D, 'One client, the same rows in every system; Elasticsearch with its request cache off; pg_duckdb is its own run (the better of heap and Parquet, a different PostgreSQL build).'))
    return c.render(end + 8, 'Time per aggregate question on 1,000,000 orders', 'Median warm latency of seven aggregate questions on PostgreSQL, pg_sift, Elasticsearch, ClickHouse, Citus columnar and pg_duckdb.')


def agg_speedup(d, theme):
    S, D = d['S'], d['D1']
    tier = S.tier_of(1_000_000, 3)
    c = Wide(theme)
    qs = questions(S)
    es = [e for e in engines_of(S, tier, D) if e != 'pg']
    c.head(f'{len(qs)} aggregate questions: how many times faster than PostgreSQL',
           'Median time of PostgreSQL divided by the system\'s, warm and on the first run after the memory was emptied. Blue: faster, orange: slower, gray: within 15%.')
    left, cw, rh, gap, top = 250, 58, 34, 22, 150
    for pi, (variant, name) in enumerate((('warm', 'warm'), ('cold', 'cold (first run)'))):
        x0 = left + pi * (len(es) * cw + gap)
        c.text(x0 + len(es) * cw / 2, top - 62, name, 14, 'middle', c.t['ink'], 'bold')
        for ci, e in enumerate(es):
            c.marker(x0 + ci * cw + cw / 2, top - 40, e)
            c.text(x0 + ci * cw + cw / 2, top - 18, HEADER[e], 11, 'middle', c.t['muted'])
        rows_ = []
        for q in qs:
            base = lat(S, D, tier, 'pg', q, variant)
            row = []
            for e in es:
                v = lat(S, D, tier, e, q, variant)
                if e == 'duckdb':   # its own PostgreSQL ran next to it
                    own = D.own(q, 'pg', 'warm_ms' if variant == 'warm' else 'cold_ms')
                    row.append(own / v[0] if own and v else None)
                else:
                    row.append(base[0] / v[0] if base and v else None)
            rows_.append(row)
        strip_cells(c, x0, top, es, rows_, cw, rh, lambda ri, ci, r, v=variant: f'Q{qs[ri]} {slug(qs[ri])} ({v}): {SYSTEMS[es[ci]][0]} is {factor_text(r)} PostgreSQL')
    for ri, q in enumerate(qs):
        c.text(22, top + ri * rh + rh / 2 + 5, f'Q{q} {slug(q)}', 14)
    y = top + rh * len(qs) + 26
    for i, r in enumerate([0.01, 0.1, 0.5, 1, 2, 10, 100]):
        c.rect(20 + i * 56, y - 11, 54, 16, strip_color(c, r), 2)
        c.text(20 + i * 56 + 27, y + 26, factor_text(r), 11, 'middle', c.t['muted'])
    end = c.foot(y + 52, foot_lines(S, tier, D, 'pg_duckdb: the faster of its heap and Parquet modes against the PostgreSQL of its own run.'))
    return c.render(end + 8, 'Speed-up over PostgreSQL per aggregate question', 'Factor over PostgreSQL for each system and question, warm and cold.')


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
        c.text(px - 40, py - 14, f'Q{q} {slug(q)}', 14, weight='bold')
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
                c.marker(x, yy, e, f'Q{q} {slug(q)}: {SYSTEMS[e][0]} {ms_text(v)} ms at {rows_label(n)} orders', r=3.5)
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


# ---- the cases --------------------------------------------------------------------------------------------------------------------------

FAMILY = [
    ('Top-N', lambda i: i.startswith('topn_') and i not in ('topn_join_filter',)),
    ('Pre-aggregates, cubes and calendar cells', lambda i: i in ('key_presence_distinct', 'cube_minmax_groupby', 'calendar_month', 'count_distinct_hist', 'rollup_dims', 'superunit_total', 'group_numeric_key',
                                                                'cluster_visibility_map', 'count_distinct_wide_keys', 'count_distinct_spill', 'numeric_sum')),
    ('Reports, windows and aggregates of aggregates', lambda i: i in ('grouping_sets_report', 'ranked_top_per_group', 'window_json_report', 'two_level_spend', 'group_by_id_top', 'count_distinct_unique', 'text_key_filtered')),
    ('Dictionary strings and lookups by key', lambda i: i.startswith('dict_') or i.startswith('point_') or i in ('keyset_page',)),
    ('Joins, semi joins, key sets and lookups', lambda i: i.startswith('keys_') or i in ('semi_exists_self', 'anti_not_in_self', 'topn_join_filter')),
    ('JSON arrays, laterals, CTEs and series', lambda i: i in ('cte_series_arms', 'cte_two_readers', 'lateral_first_row', 'lateral_dim_test', 'list_elements', 'spine_array_key', 'jsonb_each_lookup',
                                                              'filter_few_rows', 'dirty_share_few', 'dirty_share_half')),
    ('Partitions and writes', lambda i: i.startswith('partitions_') or i == 'bulk_copy_wal'),
]


def family(case_id):
    return next((name for name, f in FAMILY if f(case_id)), 'Other')


def case_ratios(results):
    """[(case id, label, ratio native / sift or None, kept)] of bench/results/cases.tsv, the first metric (ms) of each case"""
    meta, rows = extras.read(os.path.join(results, 'cases.tsv'))
    figs = [f for f in cases_render.collect(rows, cases_mod.read_registry()) if (f['metric'] == 'ms' or f['metric'] == f['case'].metrics[0]) and f['native'] and f['sift']]
    seen, out = set(), []
    for f in figs:
        if f['case'].id in seen:
            continue
        seen.add(f['case'].id)
        out.append((f['case'].id, f['native'] / f['sift'] if f['native'] and f['sift'] else None, f['kept'], f['native'], f['sift'], f['metric']))
    return meta, out


def cases_speedup(results, theme):
    meta, cs = case_ratios(results)
    c = Wide(theme)
    c.head(f'{len(cs)} cases of query shapes: pg_sift against PostgreSQL on the same rows',
           f'PostgreSQL\'s median time over pg_sift\'s, {int(meta["orders"]):,} orders, one backend, logarithmic axis. Blue: faster, orange: slower. Gray: the planner kept PostgreSQL\'s plan (the bar shows the cost of deciding, if any).')
    groups = {}
    for case in cs:
        groups.setdefault(family(case[0]), []).append(case)
    order = [name for name, _ in FAMILY if name in groups]
    left, right, rowh, head, gap = 250, 110, 21, 30, 12
    pw = W - left - right
    lo, hi = 0.4, 400
    X = lambda v: left + (math.log10(min(max(v, lo), hi)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * pw
    top = 132
    total = sum(head + len(groups[g]) * rowh + gap for g in order)
    for g in (0.5, 1, 2, 5, 10, 20, 50, 100, 200):
        c.line(X(g), top - 10, X(g), top + total - gap, c.t['muted'] if g == 1 else None, 1.5 if g == 1 else 1)
        c.text(X(g), top - 16, f'{g:g}×', 11, 'middle', c.t['muted'])
    y = top
    for name in order:
        c.text_box(22, y + 16, name, 14, weight='bold')
        rs = sorted(r for _, r, kept, *_ in groups[name] if r and not kept)
        if rs:   # the spread of the family: the middle and the ends of its cases that run on the scan
            c.text_box(W - 16, y + 16, f'{len(groups[name])} cases, median {factor_text(statistics.median(rs))}, from {factor_text(rs[0])} to {factor_text(rs[-1])}', 12, c.t['muted'], anchor='end')
        y += head
        for cid, r, kept, nat, sf, metric in sorted(groups[name], key=lambda t: -(t[1] or 0)):
            cy = y + rowh / 2
            c.text(34, cy + 4, cid, 12)
            if r is not None:
                fill = c.t['neutral'] if kept or 0.95 < r < 1.05 else c.t['up'] if r >= 1 else c.t['down']
                x0, x1 = X(1), X(r)
                c.rect(min(x0, x1), y + 3, abs(x1 - x0) or 2, rowh - 6, fill, 2, f'{cid}: PostgreSQL {ms_text(nat)} ms, pg_sift {ms_text(sf)} ms ({factor_text(r)})' + (' (same plan)' if kept else ''))
                label = (factor_text(r) + (' less WAL' if metric == 'wal_bytes' else '')) if r >= 1 else f'{factor_text(1 / r)} slower'
                if kept:
                    label = 'same plan' + ('' if abs(math.log10(r)) < math.log10(1.05) else f', {label}')
                if r >= 1:
                    c.text(x1 + 6, cy + 4, label, 12, fill=c.t['ink'], weight=None if kept else 'bold')
                else:
                    c.text(x1 - 6, cy + 4, label, 12, 'end', c.t['ink'] if kept else c.t['down'], None if kept else 'bold')
            y += rowh
        y += gap
    end = c.foot(y + 14, [f'Data: bench/results/cases.tsv, bench/cases.tsv; measured {meta.get("date", "?")} on {meta.get("cpu", "?").split(",")[0]}; PostgreSQL {meta.get("postgresql", "?")}, pg_sift {meta.get("pg_sift", "?")}. The milliseconds are in the table of docs/BENCHMARKS.md.'])
    return c.render(end + 8, 'Speed-up of every benchmark case', 'Bars: PostgreSQL time over pg_sift time for each case, grouped by family.')


def ecdf_points(vals):
    vals = sorted(vals)
    n = len(vals)
    return [(v, (n - i) / n) for i, v in enumerate(vals)]   # the share of cases that are at least this fast


def shape_ratios(results):
    import aggregates_page
    found = aggregates_page.shape_cases(results)
    if not found:
        return []
    return [(q, v, t['heap'] / t['col']) for q, v, t in found[1] if t['heap'] and t['col']]


def shapes_tier(results):
    """'1.2M': the size of the corpus of bench/results/analytics.json"""
    return json.load(open(os.path.join(results, 'analytics.json')))['meta'].get('tier', '?')


def cases_ecdf(results, theme):
    meta, cs = case_ratios(results)
    cvals = [r for _, r, kept, _, _, metric in cs if r and metric == 'ms']
    svals = [r for _, _, r in shape_ratios(results)]
    c = Wide(theme)
    c.head('How many cases get how much faster',
           'The share of cases whose speed-up over PostgreSQL is at least x, on a logarithmic axis. A curve that stays high to the right wins by a lot on most cases; the part left of 1× are the cases where pg_sift is slower.')
    left, right, top, ph = 90, 40, 124, 460
    pw = W - left - right
    lo, hi = 0.4, 400
    X = lambda v: left + (math.log10(min(max(v, lo), hi)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * pw
    Y = lambda f: top + ph - f * ph
    for f in (0, 0.25, 0.5, 0.75, 1):
        c.line(left, Y(f), left + pw, Y(f))
        c.text(left - 8, Y(f) + 4, f'{f * 100:.0f}%', 12, 'end', c.t['muted'])
    for g in (0.5, 1, 2, 5, 10, 20, 50, 100, 200):
        c.line(X(g), top, X(g), top + ph, c.t['muted'] if g == 1 else None, 1.5 if g == 1 else 1)
        c.text(X(g), top + ph + 20, f'{g:g}×', 12, 'middle', c.t['muted'])
    c.text(left + pw / 2, top + ph + 44, 'speed-up over PostgreSQL (time of PostgreSQL / time of pg_sift)', 12, 'middle', c.t['muted'])
    series = [(f'{len(cvals)} benchmark cases, {int(meta["orders"]):,} orders', cvals, 'sift'), (f'{len(svals)} real query shapes, {shapes_tier(results)} orders', svals, 'sift_table')]
    for k, (name, vals, sid) in enumerate(series):
        if not vals:
            continue
        pts = ecdf_points(vals)
        path = [(X(pts[0][0]), Y(1.0))]
        for v, f in pts:
            path.append((X(v), path[-1][1]))
            path.append((X(v), Y(f)))
        path.append((X(pts[-1][0]), Y(0)))
        c.polyline(path, c.color(sid), 2.5)
        median = statistics.median(vals)
        slower = sum(1 for v in vals if v < 0.95)
        c.marker(X(median), Y(0.5), sid, f'{name}: median {factor_text(median)}', r=5.5)
        ly = top + 24 + 46 * k
        c.marker(left + pw - 330, ly - 4, sid)
        c.text(left + pw - 316, ly, name, 13, weight='bold')
        c.text(left + pw - 316, ly + 18, f'median {factor_text(median)}, slower on {slower} ({100 * slower / len(vals):.0f}%)', 12, fill=c.t['muted'])
    end = c.foot(top + ph + 68, ['Data: bench/results/cases.tsv (one backend, no parallel workers, PostgreSQL = the same table with sift.enable_agg = off) and bench/results/analytics.json (shapes G1-G8 at one filter and S1-S7 at three filter sizes, wall time of one backend). The two sets are different workloads.'])
    return c.render(end + 8, 'Distribution of the speed-up', 'Share of benchmark cases and of real query shapes at least as fast as x times PostgreSQL.')


def shapes_speedup(results, theme):
    rows_ = shape_ratios(results)
    c = Wide(theme)
    c.head('Real query shapes: the gain grows with the size of the filter',
           f'Speed-up of pg_sift over the PostgreSQL heap on {shapes_tier(results)} orders, one backend; the S shapes (reports of a set of tenants) are measured at 10, 400 and 4,000 tenants. Blue dot: faster, orange triangle: slower.')
    shapes = list(dict.fromkeys(q for q, _, _ in rows_))
    left, right, rowh = 120, 60, 36
    pw = W - left - right
    lo, hi = 0.4, 400
    top = 128
    X = lambda v: left + (math.log10(min(max(v, lo), hi)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * pw
    for g in (0.5, 1, 2, 5, 10, 20, 50, 100, 200):
        c.line(X(g), top - 10, X(g), top + rowh * len(shapes), c.t['muted'] if g == 1 else None, 1.5 if g == 1 else 1)
        c.text(X(g), top - 16, f'{g:g}×', 11, 'middle', c.t['muted'])
    marks = {'-': ('circle', c.t['up']), 'K10': ('square', None), 'K400': ('diamond', None), 'K4000': ('circle', None)}
    shade = {'K10': 0.35, 'K400': 0.65, 'K4000': 1.0}
    for i, q in enumerate(shapes):
        cy = top + rowh * i + rowh / 2
        if i % 2 == 0:
            c.rect(14, cy - rowh / 2, W - 28, rowh, c.t['neutral'], 4)
        c.text(22, cy + 5, q, 14, weight='bold')
        pts = [(v, r) for qq, v, r in rows_ if qq == q]
        xs = [X(r) for _, r in pts]
        if len(pts) > 1:
            c.line(min(xs), cy, max(xs), cy, c.t['muted'], 2, opacity=0.5)
        close = max(xs) - min(xs) < 36   # the sizes of the filter do not move it: one label for the three dots
        for v, r in pts:
            dot(c, X(r), cy, r >= 0.95, f'{q} {"" if v == "-" else v}: {factor_text(r)} the PostgreSQL heap')
            if v != '-' and not close:
                c.text(X(r), cy - 11, v.replace('K', ''), 10, 'middle', c.t['muted'])
        if len(pts) == 1 or close:
            c.text(max(xs) + 14, cy + 5, factor_text(statistics.median(r for _, r in pts)) + ('' if len(pts) == 1 else ' at 10, 400 and 4,000 tenants'), 12, fill=c.t['ink'], weight='bold')
    end = c.foot(top + rowh * len(shapes) + 24, ['Data: bench/results/analytics.json; the number over a dot is the tenants in the filter (10, 400, 4,000). G1-G8 read the whole table. Orange triangles: pg_sift slower than the heap.'])
    return c.render(end + 8, 'Speed-up on the real query shapes', 'Speed-up over the PostgreSQL heap for 8 whole-table shapes and 7 store-filtered shapes at three filter sizes.')


def shapes_latency(results, theme):
    """the application shapes in milliseconds on PostgreSQL, pg_sift, PostgreSQL flat tables and ClickHouse: the whole-table shapes and the store-filtered ones at the smallest and the largest filter"""
    import aggregates_page
    found = aggregates_page.shape_cases(results)
    if not found:
        return None
    meta, cases = found
    cases = [(q, v, t) for q, v, t in cases if v in ('-', 'K10', 'K4000') and t['col']]
    es = [e for e in ('heap', 'col', 'flat', 'ch') if any(t[e] for _, _, t in cases)]
    sid = {'heap': 'pg', 'col': 'sift', 'flat': 'flat', 'ch': 'ch'}
    c = Wide(theme)
    c.head(f'Real query shapes on {meta.get("tier", "?")} orders: time per shape',
           'Median wall milliseconds of one backend on a logarithmic axis, left is faster. G1-G8 read the whole table; S1-S7 are reports of a set of tenants, shown with the smallest (10) and the largest (4,000) filter. '
           'On the right pg_sift against the fastest other system.')
    y = c.legend(20, 96 + 18 * (c.sub_lines - 2), [sid[e] for e in es], {'pg': 'PostgreSQL (heap)', 'sift': 'pg_sift sift_col'})
    left, right, rowh = 150, 250, 27
    top = y + 40
    pw = W - left - right
    lo, hi = 0.5, 3000
    X = lambda v: left + (math.log10(min(max(v, lo), hi)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * pw
    for g in (1, 10, 100, 1000):
        c.line(X(g), top - 10, X(g), top + rowh * len(cases))
        c.text(X(g), top - 16, f'{g:,g} ms', 12, 'middle', c.t['muted'])
    c.text(W - 20, top - 16, 'pg_sift against the fastest other', 11, 'end', c.t['muted'])
    off = {e: (i - (len(es) - 1) / 2) * 4.5 for i, e in enumerate(es)}
    for i, (q, v, t) in enumerate(cases):
        cy = top + rowh * i + rowh / 2
        if i % 2 == 0:
            c.rect(14, cy - rowh / 2, W - 28, rowh, c.t['neutral'], 4)
        label = q if v == '-' else f'{q}, {v.replace("K", "")} tenants'
        c.text(22, cy + 4, label, 13, weight='bold' if v == '-' else None)
        for e in es:
            if t[e]:
                c.marker(X(t[e]), cy + off[e], sid[e], f'{label}: {SYSTEMS[sid[e]][0]} {ms_text(t[e])} ms', r=3.8)
        rivals = {e: t[e] for e in es if e != 'col' and t[e]}
        best = min(rivals, key=rivals.get)
        ratio = rivals[best] / t['col']
        name = {'heap': 'PostgreSQL', 'flat': 'flat tables', 'ch': 'ClickHouse'}[best]
        word = f'{factor_text(ratio)} faster than {name}' if ratio >= 1.05 else f'tie with {name}' if ratio > 0.95 else f'{factor_text(1 / ratio)} slower than {name}'
        c.text(W - 20, cy + 4, word, 12, 'end', c.t['ink'] if ratio >= 0.95 else c.t['down'], 'bold' if ratio < 0.95 else None)
    end = c.foot(top + rowh * len(cases) + 22, [f'Data: bench/results/analytics.json, {meta.get("date", "?")[:10]}, median of {meta.get("runs", "?")} runs, one backend with no parallel workers, ClickHouse on one thread; the filter of 400 tenants is in docs/perf/AGGREGATES.md. '
                                                'Flat tables are the best conventional layout (denormalized copies) of the same data. S6 has no ClickHouse query.'])
    return c.render(end + 8, 'Time per application shape', 'Median time of each application shape on PostgreSQL, pg_sift, PostgreSQL flat tables and ClickHouse.')


# ---- scale of the cases, work_mem, cost ------------------------------------------------------------------------------------------------

def scale_series(results):
    """{case id: {'native': [(rows, ms)], 'sift': [(rows, ms)]}} of bench/results/cases-scale.tsv"""
    path = os.path.join(results, 'cases-scale.tsv')
    if not os.path.exists(path):
        return {}, {}
    meta, rows = extras.read(path)
    out = {}
    for r in rows:
        cid, _, n = r['case'].partition('@')
        if r['metric'] == 'ms' and r['arm'] in ('native', 'sift') and r['value'] not in ('', 'NA', 'error') and n:
            out.setdefault(cid, {}).setdefault(r['arm'], []).append((int(n), float(r['value'])))
    for v in out.values():
        for a in v.values():
            a.sort()
    return meta, {k: v for k, v in out.items() if len(v.get('sift', [])) >= 3 and len(v.get('native', [])) >= 3}


def cases_scale(results, theme):
    meta, ser = scale_series(results)
    if not ser:
        return None
    c = Wide(theme)
    c.head(f'Scaling of {len(ser)} query shapes: time against rows',
           'Median ms, both axes logarithmic. The number at each line is its slope: 1.0 is time proportional to the rows, lower is better.')
    ids = sorted(ser, key=lambda i: -ser[i]['native'][-1][1] / ser[i]['sift'][-1][1])
    ncols, pw, ph, gy = 3, 230, 120, 70
    top = 100
    cell = (W - 40) / ncols
    c.marker(24, top - 24, 'pg')
    c.text(36, top - 20, 'PostgreSQL', 13)
    c.marker(140, top - 24, 'sift')
    c.text(152, top - 20, 'pg_sift', 13)
    top += 14
    sizes = sorted({n for s in ser.values() for a in s.values() for n, _ in a})
    for i, cid in enumerate(ids):
        px, py = 20 + (i % ncols) * cell + 48, top + (i // ncols) * (ph + gy)
        vals = [v for a in ser[cid].values() for _, v in a]
        lo, hi = 10 ** math.floor(math.log10(min(vals))), 10 ** math.ceil(math.log10(max(vals)))
        if hi == lo:
            hi *= 10
        Y = lambda v: py + ph - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * ph
        X = lambda n: px + (math.log10(n) - math.log10(sizes[0])) / (math.log10(sizes[-1]) - math.log10(sizes[0])) * pw
        c.text(px - 40, py - 12, cid, 13, weight='bold')
        g = lo
        while g <= hi * 1.001:
            c.line(px, Y(g), px + pw, Y(g))
            c.text(px - 6, Y(g) + 4, ms_text(g), 10, 'end', c.t['muted'])
            g *= 10
        for n in (sizes[0], sizes[-1]):
            c.text(X(n), py + ph + 15, rows_label(n), 10, 'middle', c.t['muted'])
        ends = []
        for arm, sid in (('native', 'pg'), ('sift', 'sift')):
            p = ser[cid][arm]
            c.polyline([(X(n), Y(v)) for n, v in p], c.color(sid), 2)
            for n, v in p:
                c.marker(X(n), Y(v), sid, f'{cid}: {SYSTEMS[sid][0]} {ms_text(v)} ms at {rows_label(n)} rows', r=3)
            ends.append((Y(p[-1][1]), sid, slope([n for n, _ in p], [v for _, v in p])))
        ends.sort()
        prev = -99
        for yy, sid, sl in ends:
            yy = max(yy, prev + 13)
            c.text(px + pw + 6, yy + 4, f'{sl:.2f}', 11, fill=c.color(sid) if False else c.t['ink'], weight='bold' if sid == 'sift' else None)
            prev = yy
    n_rows = (len(ids) - 1) // ncols + 1
    end = c.foot(top + n_rows * (ph + gy) - 22, [f'Data: bench/results/cases-scale.tsv, {meta.get("date", "?")}, {meta.get("cpu", "?").split(",")[0]}, one backend, medians of interleaved runs; sizes {", ".join(rows_label(n) for n in sizes)}. The slope is the least-squares slope of log time on log rows over all sizes.'])
    return c.render(end + 8, 'Scaling of the case families', 'Time against the number of rows for PostgreSQL and pg_sift on several query shapes.')


def cases_workmem(results, theme):
    p1, p2 = os.path.join(results, 'cases.tsv'), os.path.join(results, 'cases-wm4g.tsv')
    if not os.path.exists(p2):
        return None
    m1, r1 = extras.read(p1)
    m2, r2 = extras.read(p2)
    reg = cases_mod.read_registry()
    f1 = {f['case'].id: f for f in cases_render.collect(r1, reg) if f['metric'] == 'ms' and f['native'] and f['sift']}
    f2 = {f['case'].id: f for f in cases_render.collect(r2, reg) if f['metric'] == 'ms' and f['native'] and f['sift']}
    ids = [i for i in f2 if i in f1]
    ids.sort(key=lambda i: -f2[i]['native'] / f2[i]['sift'])
    c = Wide(theme)
    c.head('The same cases at work_mem = 4GB, a large setting',
           f'Speed-up of pg_sift over PostgreSQL at the {m1.get("work_mem", "64MB")} of the benchmark cluster (gray square) and at {m2.get("work_mem", "4GB")} (blue dot). Logarithmic axis; the plans of both engines can change with work_mem.')
    left, right, rowh, top = 250, 60, 30, 112
    pw = W - left - right
    lo, hi = 0.4, 400
    X = lambda v: left + (math.log10(min(max(v, lo), hi)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * pw
    for g in (0.5, 1, 2, 5, 10, 20, 50, 100, 200):
        c.line(X(g), top - 10, X(g), top + rowh * len(ids), c.t['muted'] if g == 1 else None, 1.5 if g == 1 else 1)
        c.text(X(g), top - 16, f'{g:g}×', 11, 'middle', c.t['muted'])
    for i, cid in enumerate(ids):
        cy = top + rowh * i + rowh / 2
        if i % 2 == 0:
            c.rect(14, cy - rowh / 2, W - 28, rowh, c.t['neutral'], 4)
        c.text(22, cy + 5, cid, 13)
        a, b = f1[cid]['native'] / f1[cid]['sift'], f2[cid]['native'] / f2[cid]['sift']
        c.line(X(a), cy, X(b), cy, c.t['muted'], 2, opacity=0.5)
        c.marker(X(a), cy, 'pg', f'{cid} at {m1.get("work_mem", "64MB")}: {factor_text(a)}', r=5)
        c.marker(X(b), cy, 'sift', f'{cid} at {m2.get("work_mem", "4GB")}: {factor_text(b)} (PostgreSQL {ms_text(f2[cid]["native"])} ms, pg_sift {ms_text(f2[cid]["sift"])} ms)', r=5.5)
        c.text(X(max(a, b)) + 12, cy + 5, f'{factor_text(b)}', 12, fill=c.t['ink'], weight='bold')
    y = top + rowh * len(ids) + 20
    c.marker(24, y, 'pg')
    c.text(36, y + 4, f'work_mem {m1.get("work_mem", "64MB")}', 12)
    c.marker(180, y, 'sift')
    c.text(192, y + 4, f'work_mem {m2.get("work_mem", "4GB")}', 12)
    end = c.foot(y + 30, [f'Data: bench/results/cases.tsv and cases-wm4g.tsv, {m2.get("date", "?")}, {m2.get("cpu", "?").split(",")[0]}; {int(m2["orders"]):,} orders, one backend.'])
    return c.render(end + 8, 'Speed-up at the default and at a large work_mem', 'Speed-up of pg_sift over PostgreSQL for the analytics-shaped cases at two work_mem settings.')


def cost_per_row(d, theme):
    S = d['S']
    tier = S.tier_of(1_000_000, 3)
    n = S.tiers[tier]['rows']
    es = [e for e in ('pg', 'sift', 'sift_table', 'ch', 'citus', 'es') if e in S.engines(tier)]
    c = Wide(theme)
    c.head('What a row costs: index bytes, load time and update rate',
           'Per system on 1,000,000 orders. Left: bytes per row of what the system keeps beside the table (PostgreSQL: its btrees and GIN; the column tenants: all their data). Middle: load time of the CSV per row. Right: rows per second when 10% of the rows are updated.')
    panels = [('bytes per row on disk', lambda e: (sc.sizes(S, tier, e)[1] or 0) * 1048576 / n, '{:.1f}', 'index or columnar data'),
              ('load microseconds per row', lambda e: (S.v(tier, e, metric='load_s') or 0) * 1e6 / n, '{:.1f}', 'data, indexes, merge'),
              ('rows per second, 10% updated', lambda e: S.v(tier, e, '-', 'update 10%', 'phase_rows_s') or 0, '{:,.0f}', '')]
    pw, top, rowh = 250, 140, 38
    for pi, (title, fn, fmt, sub) in enumerate(panels):
        px = 20 + pi * 330 + (112 if pi == 0 else 0)
        pw_ = 150 if pi == 0 else 220
        vals = {e: fn(e) for e in es}
        vmax = max(vals.values()) or 1
        c.text(20 + pi * 330, top - 28, title, 14, weight='bold')
        for i, e in enumerate(es):
            cy = top + i * rowh
            if pi == 0:
                c.text(20, cy + 17, HEADER[e] if e != 'es' else 'Elasticsearch', 13)
            if vals[e]:
                c.rect(px, cy + 4, max(1, vals[e] / vmax * pw_), 22, c.color(e), 3, f'{SYSTEMS[e][0]}: {fmt.format(vals[e])} {title}')
                c.text(px + vals[e] / vmax * pw_ + 6, cy + 20, fmt.format(vals[e]), 12, fill=c.t['ink'], weight='bold')
            else:
                c.text(px + 4, cy + 20, 'n/a', 12, fill=c.t['muted'])
    end = c.foot(top + rowh * len(es) + 14, [f'Data: bench/results/store.json, {S.tiers[tier]["parts"]["warm"]["date"][:10]}, {S.host()}, 3 CPUs per server. Citus columnar tables are append-only, so it is not in the update step (n/a).',
                                             'Bytes per row include the primary key only for the systems that hold the table themselves (sift_table, Citus columnar).'])
    return c.render(end + 8, 'Cost per row', 'Index size per row, load time per row and update rate for each system on 1,000,000 orders.')


# ---- the set ----------------------------------------------------------------------------------------------------------------------------

def make(results):
    out = {}
    if not os.path.exists(os.path.join(results, 'store.json')):
        return out
    d = load(results)
    have_cases = os.path.exists(os.path.join(results, 'cases.tsv'))
    have_shapes = os.path.exists(os.path.join(results, 'analytics.json'))
    for theme in THEMES:
        add = lambda name, svg: svg and out.__setitem__(f'{name}-{theme}.svg', svg)
        add('agg-latency', agg_latency(d, theme))
        add('agg-speedup', agg_speedup(d, theme))
        add('cost-per-row', cost_per_row(d, theme))
        if d['SS']:
            add('scale-growth', scale_growth(d, theme))
            add('scale-questions', scale_questions(d, theme))
        if have_cases:
            add('cases-speedup', cases_speedup(results, theme))
            add('cases-ecdf', cases_ecdf(results, theme))
            add('cases-scale', cases_scale(results, theme))
            add('cases-workmem', cases_workmem(results, theme))
        if have_shapes:
            add('shapes-speedup', shapes_speedup(results, theme))
            add('shapes-latency', shapes_latency(results, theme))
    return out
