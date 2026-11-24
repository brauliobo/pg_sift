"""readme-scopes: read latency × write-side maintenance of a tenant scope (top), the 72-cell tally and the Entity-Once 6.0 build cost (bottom)."""
from .data import SCOPE_STRUCTURE
from .theme import LEFT, RIGHT, Canvas, fmt_ms, role, scale

NAME, HEIGHT = 'scopes', 420
LABEL = {'sift_scope': 'sift_scope', 'stock_default': 'PG stock', 'semijoin': 'semijoin', 'denorm_flat': 'denorm_flat', 'denorm_part': 'denorm_part', 'rollup': 'rollup',
         'pg_ivm': 'pg_ivm', 'ch_join': 'ClickHouse JOIN', 'ch_proj': 'CH + PROJECTION', 'ch_mv': 'ClickHouse matview'}
BANDS = (('no copy', 100), ('row copy', 154), ('matview', 204))
LABEL_AT = {'sift_scope': (10, 4, 'start'), 'stock_default': (0, -12, 'middle'), 'semijoin': (0, 22, 'middle'), 'denorm_flat': (0, -12, 'middle'), 'pg_ivm': (10, 4, 'start'),
            'ch_join': (10, 4, 'start'), 'ch_proj': (10, 4, 'start'), 'ch_mv': (10, 4, 'start')}
LANE = {'ch_join': 106, 'ch_proj': 122, 'ch_mv': 204}


def title(data):
    tally, cells = data['scopes']['tally'], data['scopes']['cells']
    wins = tally['stock_default'][0]
    rival = max(tally, key=lambda a: tally[a][2])
    return f'sift_scope wins {wins} of {cells} cells against stock PostgreSQL; {LABEL[rival]} still wins {tally[rival][2]}'


def plot(c, data):
    reads, tally = data['scopes']['read_ms'], data['scopes']['tally']
    X = scale(1, 1000, 84, 600, log=True)
    for t in (1, 10, 100, 1000):
        c.line(X(t), 74, X(t), 222)
        c.text(X(t), 240, f'{t:,}', 'sub', anchor='middle')
    for name, y in BANDS:
        c.text(76, y + 4, name, 'sub', anchor='end')
    c.line(84, 128, 836, 128, dash='3 4')
    c.line(84, 180, 836, 180, dash='3 4')
    c.line(616, 74, 616, 222, 'axis')
    c.text(LEFT, 66, '↓ what a write also maintains', 'sub')
    c.text(RIGHT, 66, 'tenant 1 sum_by_month read, warm ms (log) →', 'sub', anchor='end')
    if any(a not in reads for a in LANE):
        c.text(626, 90, 'ClickHouse: not measured yet', 'sub')
    for arm in LABEL:
        dx, dy, anchor = LABEL_AT.get(arm, (10, 4, 'start'))
        if arm in reads:
            x, y = X(reads[arm]), BANDS[SCOPE_STRUCTURE[arm]][1]
            c.circle(x, y, 6.5, 'amber' if tally.get(arm, (0, 0, 0))[2] else role(arm))
            c.text(x + dx, y + dy, f'{LABEL[arm]} · {fmt_ms(reads[arm])}', 'text' if arm == 'sift_scope' else 'sub', anchor=anchor, bold=arm == 'sift_scope')
        elif arm in LANE:
            c.circle(634, LANE[arm], 5.5, None, 'soft', 1.5)
            c.text(646, LANE[arm] + 4, LABEL[arm], 'sub')


def tally_block(c, x, y0, data):
    tally, cells = data['scopes']['tally'], data['scopes']['cells']
    c.text(x, y0, f'{cells} cells per rival: sift_scope wins / ties / loses (warm)', bold=True)
    for i, (arm, (w, t, l)) in enumerate(tally.items()):
        y = y0 + 24 + i * 17
        c.text(x, y, LABEL[arm], 'sub')
        left = x + 108
        for width, fill in ((w, 'col'), (t, 'soft'), (l, 'amber')):
            c.rect(left, y - 10, 190 * width / cells, 11, fill)
            left += 190 * width / cells
        c.text(x + 312, y, f'{w} / {t} / {l}', 'amber' if l > cells / 4 else 'text', bold=l > cells / 4)


def build_block(c, x, y0, data):
    c.text(x, y0, 'Entity-Once build cost: 5.1 against the 6.0 gate', bold=True)
    c.text(RIGHT - 110, y0 + 20, 'v5.1', 'sub', anchor='end')
    c.text(RIGHT, y0 + 20, '6.0 gate', 'sub', anchor='end')
    for i, (label, now, gate) in enumerate(data['scope_build']):
        y = y0 + 40 + i * 17
        c.text(x, y, label, 'sub')
        c.text(RIGHT - 110, y, now, 'text', anchor='end')
        c.text(RIGHT, y, gate, 'col', anchor='end', bold=True)
    c.text(x, y0 + 40 + len(data['scope_build']) * 17 + 4, '6.0 measured: not measured yet', 'sub')


def render(data, mode):
    c = Canvas(mode, HEIGHT)
    cells = data['scopes']['cells']
    c.header(title(data), f'{data["scoped_rollup"]["rows"]:,} events · 9 query classes × 8 tenants = {cells} cells · amber: a rival wins some cells · hollow: not measured yet')
    plot(c, data)
    tally_block(c, LEFT, 268, data)
    build_block(c, 470, 268, data)
    c.footer(f'Data: scope-stock.tsv ({data["scoped_rollup"]["version"]}), v6-core.tsv, v510.tsv · every answer md5_equal to stock PostgreSQL')
    return c.svg(title(data), '; '.join(f'{LABEL[a]} {fmt_ms(ms)}' for a, ms in data['scopes']['read_ms'].items()))
