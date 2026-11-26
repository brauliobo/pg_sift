"""readme-pg-queries: the speedup of every measured headline PostgreSQL-specific case over the fastest stock PG 18 plan by query family (top) and one flagship case per family (bottom).

A case not measured yet has no dot and no number: its family and its flagship row say it is pending the 6.0 run.
"""
from .data import PG_FAMILIES, PG_FLAGSHIP, measured
from .theme import LEFT, RIGHT, Canvas, fmt_ms, fmt_x, scale

NAME, HEIGHT = 'pg-queries', 448
PENDING_RUN = 'bench/bench cases --headline --docs'
TICKS = ((0.3, '0.3×'), (1, '1×'), (10, '10×'), (100, '100×'), (1000, '1,000×'))
X = scale(0.25, 1200, 240, 680, log=True)
COLUMNS = ((LEFT, 'Case', 'start'), (360, 'Against', 'start'), (560, 'pg_sift', 'end'), (720, 'PG 18', 'end'), (RIGHT, 'Speedup', 'end'))


def wins(rows):
    return [r for r in measured(rows) if r['ratio'] > 1]


def title(data):
    rows = data['pg_cases']['rows']
    done, won = measured(rows), [r['ratio'] for r in wins(rows)]
    pending = f'; {len(rows) - len(done)} pending' if len(done) < len(rows) else ''
    return f'pg_sift wins {len(won)} of {len(done)} measured PostgreSQL-specific queries ({fmt_x(min(won))}–{fmt_x(max(won))}); PG leads {len(done) - len(won)}{pending}'


def tone(ratio):
    return 'col' if ratio > 1 else 'amber'


def family_label(c, y, family, rows):
    done, won = measured(rows), len(wins(rows))
    pending = len(rows) - len(done)
    c.text(LEFT, y + 2, family, bold=True, size=11)
    tally = f'{won}/{len(done)} wins' if won else f'0/{len(done)} · PG wins'
    label = f'{pending} pending' if not done else f'{tally} · {pending} pending' if pending else tally
    c.text(LEFT, y + 14, label, 'sub' if not done else 'col' if won else 'amber', size=10)


def family_row(c, y, family, rows):
    family_label(c, y, family, rows)
    done = measured(rows)
    if not done:
        c.text(X(0.25), y + 4, f'{len(rows)} new 6.0 cases · pending {PENDING_RUN}', 'sub', size=10)
        return
    lo, hi = min(done, key=lambda r: r['ratio']), max(done, key=lambda r: r['ratio'])
    c.line(X(lo['ratio']), y, X(hi['ratio']), y, 'soft', 2)
    for r in done:
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


def flagship_cells(r):
    """(texts, fills, bolds) of a flagship row: its figures, or `pending 6.0 run` for a case not measured yet"""
    if r['ratio'] is None:
        return (r['case'], r['against'], 'pending 6.0 run', '—', '—'), ('text', 'sub', 'sub', 'sub', 'sub'), (False,) * 5
    won = r['ratio'] > 1
    cells = (r['case'], r['against'], f'{fmt_ms(r["sift"]["ms"])} · {blocks(r["sift"]["buffers"])}', f'{fmt_ms(r["pg"]["ms"])} · {blocks(r["pg"]["buffers"])}', fmt_x(r['ratio']))
    return cells, ('text', 'sub', 'col' if won else 'text', 'text' if won else 'amber', tone(r['ratio'])), (False, False, won, not won, True)


def strip(c, data, y0):
    rows = {r['case']: r for r in data['pg_cases']['rows']}
    c.text(LEFT, y0, 'One flagship case per family · warm ms · shared-buffer blocks · amber: PG 18 is faster', bold=True)
    for x, label, anchor in COLUMNS:
        c.text(x, y0 + 20, label, 'sub', anchor=anchor)
    for i, case in enumerate(PG_FLAGSHIP):
        for (x, _, anchor), s, fill, bold in zip(COLUMNS, *flagship_cells(rows[case])):
            c.text(x, y0 + 38 + 16 * i, s, fill, size=11, anchor=anchor, bold=bold)


def subtitle(meta):
    return (f'{meta["orders"] / 1000:,.0f}k rows · PG {meta["postgresql"]} · warm median · speedup = fastest PG 18 plan (heap, B-tree, expr B-tree, GIN) ÷ pg_sift'
            ' · amber: PG 18 wins')


def footer(data):
    meta, rows = data['pg_cases']['meta'], data['pg_cases']['rows']
    n, done = len(rows), len(measured(rows))
    if meta['source'] == 'live':
        return f'Data: docs/results/cases.tsv ({n} headline cases, {meta["orders"]:,} rows) · PG {meta["postgresql"]} · pg_sift {meta["pg_sift"]} {meta["build"]} · {meta["date"]}'
    if meta['source'] == 'mixed':
        return (f'Data: docs/results/cases.tsv ({meta["live"]} of {n} cases, {meta["orders"]:,} rows, pg_sift {meta["pg_sift"]} {meta["build"]}) · '
                f'{done - meta["live"]} v4.0 baseline · {n - done} pending')
    return f'Data: bench/cases.tsv ({n} headline cases: {done} v{meta["pg_sift"]} baseline at 300k rows, {n - done} pending) · PG {meta["postgresql"]} · build {meta["build"]}'


def render(data, mode):
    c = Canvas(mode, HEIGHT)
    c.header(title(data), subtitle(data['pg_cases']['meta']))
    plot(c, data)
    strip(c, data, 296)
    c.footer(footer(data))
    rows = data['pg_cases']['rows']
    return c.svg(title(data), '; '.join(f'{r["case"]} {"pending" if r["ratio"] is None else fmt_x(r["ratio"])}' for r in rows))
