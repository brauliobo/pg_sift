"""readme-hero: the four headline numbers of the README benchmarks, a strip of 88 px that is always visible."""
from .theme import LEFT, RIGHT, Canvas, fmt_qps, fmt_x

NAME, HEIGHT = 'hero', 88


def kpis(data):
    """[(value, role, caption line 1, caption line 2)] every number computed from the data of the charts below"""
    agg = max(vals['pg'] / vals['sift'] for _, _, vals in data['analytics'])
    size = {label: mib for label, mib, _, _ in data['storage_orders']}
    roll = data['scoped_rollup']
    count = data['search'][0][2]
    return [(fmt_x(float(f'{agg:.3g}')), 'col', 'faster than PG 18 on', f'{data["orders_tier"]}-order aggregates'),
            (f'{size["PostgreSQL 18 (stock)"] / size["pg_sift (sift_table)"]:.1f}×', 'tbl', 'smaller than PG 18:', f'{data["orders_tier"]} orders, sift_table'),
            (f'{roll["faster_cells"]} / {roll["cells"]}', 'col', 'scoped cells faster than', f'tuned stock PG ({roll["version"]})'),
            (fmt_qps(count['sift']), 'col', 'exact BM25 count(*) QPS,', f'{count["sift"] / count["es"]:.2f}× Elasticsearch 9.5')]


def render(data, mode):
    c, items = Canvas(mode, HEIGHT), kpis(data)
    step = (RIGHT - LEFT) / len(items)
    for i, (value, color, line1, line2) in enumerate(items):
        x = LEFT + i * step + (0 if i == 0 else 16)
        if i:
            c.line(LEFT + i * step, 14, LEFT + i * step, 74)
        c.text(x, 38, value, color, size=28, bold=True)
        c.text(x, 58, line1, 'text')
        c.text(x, 74, line2, 'sub')
    desc = '; '.join(f'{v} {a} {b}' for v, _, a, b in items) + '; every answer md5-equal to stock PostgreSQL 18'
    return c.svg('pg_sift benchmark headline numbers', desc)
