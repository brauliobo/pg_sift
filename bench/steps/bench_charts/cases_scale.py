# The charts of the scale of the cases, of work_mem and of the cost per row.
# cases-scale.tsv and cases-wm4g.tsv feed them.

import math
import os

import cases as cases_mod
import cases_render
import extras
import store_charts as sc
from store_charts import SYSTEMS, factor_text, ms_text

from .base import HEADER, SYSTEMS, W, Wide
from .scale import rows_label, slope


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
