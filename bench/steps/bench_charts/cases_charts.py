# The charts of the cases and of the query shapes: speedup by family, the ECDF and the latency.
# The families group the cases of bench/cases.tsv.

import json
import math
import os
import statistics

import cases as cases_mod
import cases_render
import extras
from store_charts import SYSTEMS, factor_text, ms_text

from .base import SYSTEMS, W, Wide, dot


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
        g = cases_render.gap(f) if f['kept'] else ''   # a case that runs PostgreSQL's plan has no ratio of its own: the bar is the lane with the scan forced
        scan = f['sift'] if not g else f['forced'] if g in ('cost', 'declined') else None
        out.append((f['case'].id, f['native'] / scan if scan else None, g, f['native'], scan, f['metric']))
    return meta, out


def cases_speedup(results, theme):
    meta, cs = case_ratios(results)
    c = Wide(theme)
    c.head(f'{len(cs)} cases of query shapes: pg_sift against PostgreSQL on the same rows',
           f'PostgreSQL\'s median time over pg_sift\'s, {int(meta["orders"]):,} orders, one backend, logarithmic axis. Blue: faster, orange: slower. Gray: the planner kept PostgreSQL\'s plan, the bar is the scan forced (`sift.agg_force`); no bar: the scan cannot run the case.')
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
            if r is None and kept:
                c.text(X(1) + 6, cy + 4, 'the scan cannot run it' if kept == 'feature' else 'forced scan not measured', 12, fill=c.t['ink'])
            if r is not None:
                fill = c.t['neutral'] if kept or 0.95 < r < 1.05 else c.t['up'] if r >= 1 else c.t['down']
                x0, x1 = X(1), X(r)
                c.rect(min(x0, x1), y + 3, abs(x1 - x0) or 2, rowh - 6, fill, 2, f'{cid}: PostgreSQL {ms_text(nat)} ms, pg_sift {ms_text(sf)} ms ({factor_text(r)})' + (' (scan forced; the planner runs PostgreSQL\'s plan)' if kept else ''))
                label = (factor_text(r) + (' less WAL' if metric == 'wal_bytes' else '')) if r >= 1 else f'{factor_text(1 / r)} slower'
                if kept:
                    label = f'forced scan {label}'
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
    cvals = [r for _, r, kept, _, _, metric in cs if r and not kept and metric == 'ms']
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
    end = c.foot(top + ph + 68, ['Data: bench/results/cases.tsv (one backend, no parallel workers, PostgreSQL = the same table with sift.enable_agg = off) and bench/results/analytics.json (the whole-table shapes at one filter and the set reports at three filter sizes, wall time of one backend). The two sets are different workloads.'])
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
    end = c.foot(top + rowh * len(shapes) + 24, ['Data: bench/results/analytics.json; the number over a dot is the tenants in the filter (10, 400, 4,000). The whole-table shapes read the whole table. Orange triangles: pg_sift slower than the heap.'])
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
           'Median wall milliseconds of one backend on a logarithmic axis, left is faster. The whole-table shapes read the whole table; the set reports are reports of a set of tenants, shown with the smallest (10) and the largest (4,000) filter. '
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
                                                'Flat tables are the best conventional layout (denormalized copies) of the same data. notes-path-exists has no ClickHouse query.'])
    return c.render(end + 8, 'Time per application shape', 'Median time of each application shape on PostgreSQL, pg_sift, PostgreSQL flat tables and ClickHouse.')
