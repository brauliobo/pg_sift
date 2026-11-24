"""readme-storage: proportional disk blocks (top left), flat peak RSS of the scoped build, 5.1 against 6.0 (top right), and the WAL and churn strip (bottom)."""
from .theme import LEFT, RIGHT, Canvas, fmt_mib, scale

NAME, HEIGHT = 'storage', 420
BLOCK, STEP = 7, 8.5
ORDERS = {'DuckDB (Parquet)': 'DuckDB', 'ClickHouse 26.9': 'ClickHouse', 'pg_sift (sift_table)': 'sift_table', 'Citus 14.2 columnar': 'Citus columnar',
          'pg_sift (sift_col + heap)': 'sift_col + heap', 'Elasticsearch 9.5': 'Elasticsearch', 'PostgreSQL 18 (stock)': 'PG 18'}
BM25 = {'pg_sift (positions=off)': 'sift positions=off', 'pg_sift (default)': 'sift default', 'pg_textsearch 1.5': 'pg_textsearch', 'ParadeDB (pg_search)': 'ParadeDB',
        'Elasticsearch 9.5': 'Elasticsearch', 'PG rum (tsvector)': 'PG rum'}
SIFT_ROLE = {'sift_table': 'tbl', 'sift_col + heap': 'col', 'sift positions=off': 'tbl', 'sift default': 'col'}


def slope(ladder, key='rss'):
    """least squares slope of the build's peak RSS over the rows of a ladder, in bytes a row"""
    xs, ys = list(ladder), [ladder[x][key] * 1048576 for x in ladder]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)


def title(data):
    size = {label: mib for label, mib, _, _ in data['storage_orders']}
    mem = data['memory']
    ratio = size['PostgreSQL 18 (stock)'] / size['pg_sift (sift_table)']
    tail = f'6.0 grows {slope(mem["v6"]):.1f} B/row' if mem['v6'] else '6.0 gate: ≤ 0.5 B/row'
    return f'pg_sift sift_table is {ratio:.1f}× smaller than PG 18; scoped build memory: 5.1 grew {slope(mem["v51"]):.0f} B/row, {tail}'


def waffle(c, x, y0, heading, items, names, unit):
    """one row of blocks a system, a block per `unit` MiB (the last one cut to its share); rivals smaller than the smallest pg_sift system are amber"""
    c.text(x, y0, heading, bold=True)
    smallest = min(mib for label, mib, _, _ in items if label.startswith('pg_sift'))
    for i, (label, mib, _, _) in enumerate(items):
        y, name = y0 + 20 + i * 13, names[label]
        color = SIFT_ROLE.get(name) or ('amber' if mib < smallest else 'soft')
        c.text(x + 118, y, name, 'text' if name in SIFT_ROLE else 'sub', anchor='end')
        for k in range(int(mib // unit) + 1):
            part = min(1, mib / unit - k)
            if part > 0:
                c.rect(x + 126 + k * STEP, y - BLOCK, BLOCK * part, BLOCK, color, 1)
        c.text(x + 126 + (mib / unit) * STEP + 6, y, fmt_mib(mib), 'text' if name in SIFT_ROLE else 'sub')


def rss_panel(c, x, mem):
    base, now = mem['v51'], mem['v6']
    c.text(x, 70, 'Peak RSS of the scoped build, MB by rows', bold=True)
    hi = max(v['rss'] for ladder in (base, now or {}) for v in ladder.values())
    X, Y = scale(min(base), max(base), x + 28, x + 130, log=True), scale(0, hi * 1.1, 262, 96)
    for t in range(0, int(hi * 1.1) + 1, 25):
        c.line(x + 28, Y(t), x + 130, Y(t))
        c.text(x + 20, Y(t) + 4, f'{t}', 'sub', anchor='end')
    for rows in base:
        c.text(X(rows), 280, f'{rows // 1000}k', 'sub', anchor='middle')
    c.poly([(X(r), Y(v['rss'])) for r, v in sorted(base.items())], 'rival', 2)
    c.text(x + 140, Y(base[max(base)]['rss']) + 4, f'5.1: {slope(base):.0f} B/row', 'sub')
    first = base[min(base)]['rss']
    if now:
        c.poly([(X(r), Y(v['rss'])) for r, v in sorted(now.items())], 'col', 2.5)
        c.text(x + 140, Y(now[max(now)]['rss']) + 4, f'6.0: {slope(now):.1f} B/row', 'text', bold=True)
    else:
        c.line(x + 28, Y(first), x + 130, Y(first), 'col', 2.5, '6 4')
        c.text(x + 140, Y(first) - 4, '6.0 gate', 'text', bold=True)
        c.text(x + 140, Y(first) + 12, '≤ 0.5 B/row', 'text', bold=True)
        c.text(x + 140, Y(first) + 28, 'not measured yet', 'sub')


def churn_strip(c, y0, wal):
    c.text(LEFT, y0, 'OLTP churn and WAL, bytes a row (amber: PG 18 beats pg_sift 5.1)', bold=True)
    cols = (330, 450, 580, RIGHT)
    for x, head in zip(cols, ('PG 18 heap', 'pg_sift 5.1', '6.0 gate', '6.0 measured')):
        c.text(x, y0 + 18, head, 'sub', anchor='end')
    rows = [(label, f'~{pg:.0f}' if pg else '—', f'{v51:.1f}', f'≤ {gate:.0f}', pg is not None and pg < v51) for label, pg, v51, gate in wal]
    rows.append(('10% UPDATE and 5% DELETE churn', '—', '—', '—', False))
    for i, (label, pg, v51, gate, amber) in enumerate(rows):
        y = y0 + 36 + i * 16
        c.text(LEFT, y, label, 'sub')
        c.text(cols[0], y, pg, 'amber' if amber else 'text', anchor='end', bold=amber)
        c.text(cols[1], y, v51, 'text', anchor='end')
        c.text(cols[2], y, gate, 'sub' if gate == '—' else 'col', anchor='end', bold=gate != '—')
        c.text(cols[3], y, 'not yet', 'sub', anchor='end')


def render(data, mode):
    c = Canvas(mode, HEIGHT)
    c.header(title(data), 'every block is 50 MiB (orders) or 3 MiB (BM25 index) · amber: a rival is smaller than pg_sift')
    waffle(c, LEFT, 72, f'{data["orders_tier"]} orders, total on-disk size', data['storage_orders'], ORDERS, 50)
    waffle(c, LEFT, 72 + 20 + 7 * 13 + 8, '50K Wikipedia, BM25 index', data['storage_bm25'], BM25, 3)
    rss_panel(c, 560, data['memory'])
    churn_strip(c, 298, data['wal'])
    c.footer('Data: store-scaling.json, articles.tsv, v6-core.tsv · 5.1 from bench/baselines/v510.tsv')
    return c.svg(title(data), '; '.join(f'{ORDERS[l]} {fmt_mib(m)}' for l, m, _, _ in data['storage_orders']))
