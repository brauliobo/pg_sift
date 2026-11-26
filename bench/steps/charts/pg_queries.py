"""readme-pg-queries: the speedup of every headline PostgreSQL-specific case over stock PG 18 by query family (top) and one flagship case per family (bottom)."""
from .data import PG_FAMILIES
from .theme import LEFT, RIGHT, Canvas, fmt_ms, fmt_x, scale

NAME, HEIGHT = 'pg-queries', 440
FLAGSHIP = ('softdel_live_groupby', 'lateral_first_row', 'cte_series_arms', 'jsonb_each_member', 'grouping_sets_report', 'keyset_page')
TICKS = ((0.3, '0.3×'), (1, '1×'), (10, '10×'), (100, '100×'), (1000, '1,000×'))
X = scale(0.25, 1200, 240, 680, log=True)
COLUMNS = ((LEFT, 'Case', 'start'), (360, 'Against', 'start'), (560, 'pg_sift', 'end'), (720, 'PG 18', 'end'), (RIGHT, 'Speedup', 'end'))


def wins(rows):
    return [r for r in rows if r['ratio'] > 1]


def title(data):
    rows = data['pg_cases']['rows']
    won = [r['ratio'] for r in wins(rows)]
    return f'pg_sift wins {len(won)} of {len(rows)} PostgreSQL-specific queries ({fmt_x(min(won))}–{fmt_x(max(won))}); PG leads {len(rows) - len(won)}'


def tone(ratio):
    return 'col' if ratio > 1 else 'amber'


def family_row(c, y, family, rows):
    won = len(wins(rows))
    c.text(LEFT, y + 2, family, bold=True, size=11)
    c.text(LEFT, y + 14, f'{won}/{len(rows)} wins' if won else f'0/{len(rows)} · PG wins', 'col' if won else 'amber', size=10)
    lo, hi = min(rows, key=lambda r: r['ratio']), max(rows, key=lambda r: r['ratio'])
    c.line(X(lo['ratio']), y, X(hi['ratio']), y, 'soft', 2)
    for r in rows:
        c.circle(X(r['ratio']), y, 5, tone(r['ratio']))
    c.text(X(hi['ratio']) + 9, y + 4, f'{hi["case"]} {fmt_x(hi["ratio"])}', size=11)
    if lo is not hi:
        c.text(X(lo['ratio']) - 4, y + 17, f'{lo["case"]} {fmt_x(lo["ratio"])}', 'sub', size=10)


def plot(c, data):
    rows = data['pg_cases']['rows']
    c.text(LEFT, 66, 'Query family · cases pg_sift wins', 'sub')
    c.text(RIGHT, 66, 'speedup = PG 18 ms ÷ pg_sift ms (log) →', 'sub', anchor='end')
    for t, label in TICKS:
        c.line(X(t), 74, X(t), 262, 'axis' if t == 1 else 'grid', dash='4 3' if t == 1 else None)
        c.text(X(t), 276, label, 'sub', anchor='middle')
    for i, family in enumerate(PG_FAMILIES):
        family_row(c, 88 + 31 * i, family, [r for r in rows if r['family'] == family])


def blocks(v):
    return '—' if v is None else f'{v:,.0f} blk'


def strip(c, data, y0):
    rows = {r['case']: r for r in data['pg_cases']['rows']}
    c.text(LEFT, y0, 'One flagship case per family · warm ms · shared-buffer blocks · amber: PG 18 is faster', bold=True)
    for x, label, anchor in COLUMNS:
        c.text(x, y0 + 20, label, 'sub', anchor=anchor)
    for i, case in enumerate(FLAGSHIP):
        r, y = rows[case], y0 + 38 + 15 * i
        won = r['ratio'] > 1
        cells = (case, r['against'], f'{fmt_ms(r["sift"]["ms"])} · {blocks(r["sift"]["buffers"])}', f'{fmt_ms(r["pg"]["ms"])} · {blocks(r["pg"]["buffers"])}', fmt_x(r['ratio']))
        fills = ('text', 'sub', 'col' if won else 'text', 'text' if won else 'amber', tone(r['ratio']))
        bolds = (False, False, won, not won, True)
        for (x, _, anchor), s, fill, bold in zip(COLUMNS, cells, fills, bolds):
            c.text(x, y, s, fill, size=11, anchor=anchor, bold=bold)


def subtitle(meta):
    return f'{meta["orders"] / 1000:,.0f}k rows · PG {meta["postgresql"]} · warm median · speedup = PG 18 (faster of heap/B-tree) ÷ pg_sift · amber: PG 18 wins'


def footer(data):
    meta, n = data['pg_cases']['meta'], len(data['pg_cases']['rows'])
    if meta['source'] == 'live':
        return f'Data: docs/results/cases.tsv ({n} headline cases, {meta["orders"]:,} rows) · PG {meta["postgresql"]} · pg_sift {meta["pg_sift"]} {meta["build"]} · {meta["date"]}'
    return f'Data: bench/cases.tsv ({n} headline cases, 300k rows) · PG {meta["postgresql"]} · v{meta["pg_sift"]} baseline {meta["build"]} (live cases.tsv overlays on 6.0 run)'


def render(data, mode):
    c = Canvas(mode, HEIGHT)
    c.header(title(data), subtitle(data['pg_cases']['meta']))
    plot(c, data)
    strip(c, data, 296)
    c.footer(footer(data))
    return c.svg(title(data), '; '.join(f'{r["case"]} {fmt_x(r["ratio"])}' for r in data['pg_cases']['rows']))
