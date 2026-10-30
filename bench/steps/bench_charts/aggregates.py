# The charts of the aggregate questions: latency on a log axis and the speedup over PostgreSQL.
# Each takes the loaded results and a theme and returns an SVG.

import math

from store_charts import SYSTEMS, factor_text, ms_text, strip_cells, strip_color

from .base import HEADER, SHORT, SYSTEMS, W, Wide
from .results import engines_of, foot_lines, lat, questions


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
        c.text(22, cy - 2, f'{q}', 14)
        best_mine = min((e for e in ('sift', 'sift_table') if vals.get(e)), key=lambda e: vals[e][0], default=None)
        if best_mine and vals.get('pg'):
            c.text(22, cy + 15, f'PostgreSQL {ms_text(vals["pg"][0])} ms, {SHORT[best_mine]} {ms_text(vals[best_mine][0])} ms', 11, fill=c.t['muted'])
        for e in es:
            if not vals[e]:
                continue
            v, mn, mx = vals[e]
            ey = cy + off[e]
            c.line(X(max(mn, lo)), ey, X(min(mx, hi)), ey, c.color(e), 2, opacity=0.55)
            c.marker(X(v), ey, e, f'{q}: {SYSTEMS[e][0]} {ms_text(v)} ms (fastest {ms_text(mn)}, slowest {ms_text(mx)})', r=4.5)
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
        strip_cells(c, x0, top, es, rows_, cw, rh, lambda ri, ci, r, v=variant: f'{qs[ri]} ({v}): {SYSTEMS[es[ci]][0]} is {factor_text(r)} PostgreSQL')
    for ri, q in enumerate(qs):
        c.text(22, top + ri * rh + rh / 2 + 5, f'{q}', 14)
    y = top + rh * len(qs) + 26
    for i, r in enumerate([0.01, 0.1, 0.5, 1, 2, 10, 100]):
        c.rect(20 + i * 56, y - 11, 54, 16, strip_color(c, r), 2)
        c.text(20 + i * 56 + 27, y + 26, factor_text(r), 11, 'middle', c.t['muted'])
    end = c.foot(y + 52, foot_lines(S, tier, D, 'pg_duckdb: the faster of its heap and Parquet modes against the PostgreSQL of its own run.'))
    return c.render(end + 8, 'Speed-up over PostgreSQL per aggregate question', 'Factor over PostgreSQL for each system and question, warm and cold.')
