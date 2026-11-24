"""readme-analytics: the speed × disk Pareto frontier of the orders aggregates (top) and the per-query warm / cold strip (bottom)."""
from .data import LABEL, QUERY_ORDER_ANALYTICS, QUERY_SLUG
from .theme import LEFT, RIGHT, Canvas, fmt_ms, fmt_mib, geomean, role, scale, short

NAME, HEIGHT = 'analytics', 420
SIZE_KEY = {'sift': 'pg_sift (sift_col + heap)', 'sift_table': 'pg_sift (sift_table)', 'ch': 'ClickHouse 26.9', 'es': 'Elasticsearch 9.5', 'duckdb': 'DuckDB (Parquet)', 'pg': 'PostgreSQL 18 (stock)'}
LABEL_AT = {'duckdb': (9, -9, 'start'), 'ch': (9, -9, 'start'), 'sift_table': (9, -9, 'start'), 'es': (-9, -9, 'end'), 'sift': (-9, -10, 'end'), 'pg': (-9, 4, 'end')}
STRIP_ARMS = ('sift', 'ch', 'ch_proj', 'es', 'duckdb', 'pg')
COLUMNS = ('02 Count', '03 Revenue', '04 Top 10', '05 Avg ship', '06 Distinct', '07 Recent')


def points(data):
    sizes = {label: mib for label, mib, _, _ in data['storage_orders']}
    speed = {e: geomean([vals[e] for _, _, vals in data['analytics']]) for e in LABEL if all(e in vals for _, _, vals in data['analytics'])}
    return {e: (sizes[SIZE_KEY[e]], speed[e]) for e in speed if SIZE_KEY.get(e) in sizes}


def frontier(pts):
    best, out = float('inf'), []
    for e, (x, y) in sorted(pts.items(), key=lambda kv: kv[1]):
        if y < best:
            best, out = y, out + [e]
    return out


def leaders(data):
    """{query label: fastest engine} over the warm medians of the headline tier"""
    return {label: min(vals, key=vals.get) for label, _, vals in data['analytics']}


def title(data):
    lead = leaders(data)
    losses = [e for e in lead.values() if e not in ('sift', 'sift_table')]
    wins = len(lead) - len(losses)
    rest = f'; {LABEL[max(set(losses), key=losses.count)]} leads {len(losses)} selective scans' if losses else ''
    return f'pg_sift is fastest on {wins} of {len(lead)} aggregates{rest}'


def plot(c, data):
    pts = points(data)
    front = frontier(pts)
    X, Y = scale(250, 2000, 84, 836, log=True), scale(2.5, 600, 222, 74, log=True)
    for t in (5, 10, 50, 100, 500):
        c.line(84, Y(t), 836, Y(t))
        c.text(76, Y(t) + 4, f'{t}', 'sub', anchor='end')
    for t in (300, 500, 1000, 2000):
        c.line(X(t), 74, X(t), 222)
        c.text(X(t), 240, f'{t:,}', 'sub', anchor='middle')
    c.text(LEFT, 66, '↑ geomean of the warm medians 02–07 (ms, log)', 'sub')
    c.text(RIGHT, 66, 'total on-disk size (MiB, log) →', 'sub', anchor='end')
    c.text(92, 214, '↙ better', 'sub')
    c.poly([(X(pts[e][0]), Y(pts[e][1])) for e in front], 'soft', 1.5, '5 4')
    for e, (size, ms) in sorted(pts.items(), key=lambda kv: -kv[1][1]):
        won = e in front and role(e) == 'rival'
        c.circle(X(size), Y(ms), 6.5, 'amber' if won else role(e))
        dx, dy, anchor = LABEL_AT[e]
        c.text(X(size) + dx, Y(ms) + dy, f'{LABEL[e]} · {fmt_ms(ms)}', 'text' if role(e) != 'rival' else 'sub', anchor=anchor, bold=role(e) != 'rival')


def cell(row, arm, q):
    got = row.get(QUERY_SLUG[q])
    return got.get(arm) if got else None


def strip(c, data, y0):
    cold = data['analytics_cold']
    c.text(LEFT, y0, 'Per query on 1M orders, warm / cold ms · amber: a rival is faster than pg_sift sift_col', bold=True)
    xs = [190 + i * 106 + 53 for i in range(6)]
    for x, label in zip(xs, COLUMNS):
        c.text(x, y0 + 22, label, 'sub', anchor='middle')
    shown = [a for a in STRIP_ARMS if any(cell(cold, a, q) for q, _, _ in QUERY_ORDER_ANALYTICS)]
    for r, arm in enumerate(shown):
        y = y0 + 44 + r * 20
        c.line(LEFT, y - 15, RIGHT, y - 15)
        c.text(LEFT, y, LABEL[arm], 'text' if role(arm) != 'rival' else 'sub', bold=role(arm) != 'rival')
        for x, (q, _, _) in zip(xs, QUERY_ORDER_ANALYTICS):
            v, base = cell(cold, arm, q), cell(cold, 'sift', q)
            if v is None:
                continue
            c.text(x, y, '/', 'sub', anchor='middle')
            for i, anchor in enumerate(('end', 'start')):
                faster = arm != 'sift' and base is not None and v[i] < base[i]
                c.text(x + (-5 if i == 0 else 5), y, short(v[i]), 'amber' if faster else 'col' if arm == 'sift' else 'text', anchor=anchor, bold=faster or arm == 'sift')
    missing = ', '.join(LABEL[a] for a in STRIP_ARMS if a not in shown)
    return missing, y0 + 44 + len(shown) * 20


def render(data, mode):
    c = Canvas(mode, HEIGHT)
    c.header(title(data), f'{data["orders_tier"]} orders · 3 CPUs, 8 GiB · warm geomean against disk · amber: a rival beats pg_sift on one axis')
    plot(c, data)
    missing, y = strip(c, data, 268)
    if missing:
        c.text(LEFT, y - 2, f'not measured yet: {missing}', 'sub')
    c.footer('Data: store-scaling.json, store.json, duckdb-10000000.tsv · Ryzen 7 9700X · 2026-10-27')
    desc = '; '.join(f'{LABEL[e]} {fmt_ms(ms)} geomean, {fmt_mib(s)}' for e, (s, ms) in points(data).items())
    return c.svg(title(data), desc)
