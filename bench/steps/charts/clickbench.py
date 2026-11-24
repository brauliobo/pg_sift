"""readme-clickbench: the 43-tile win map against ClickHouse + PROJECTION, a geometric-mean pill per arm and the five query families."""
import math

from .data import CB_FAMILIES
from .theme import LEFT, RIGHT, Canvas, geomean, role, scale, short

NAME, HEIGHT = 'clickbench', 400
ARMS = {'sift_table': 'pg_sift sift_table', 'sift_col': 'pg_sift sift_col', 'ch': 'ClickHouse', 'ch_proj': 'CH + PROJECTION', 'duckdb': 'DuckDB', 'pg': 'PG 18'}
QUERIES, TIE = 43, 1.05


def standings(cb):
    """(the pg_sift arm with the lowest geomean, {arm: geomean ms}) of the warm wall times; (None, {}) without a run"""
    if not cb:
        return None, {}
    geo = {a: geomean(list(cb['warm'][a].values())) for a in ARMS if cb['warm'].get(a)}
    sift = [a for a in ('sift_table', 'sift_col') if a in geo]
    return (min(sift, key=geo.get) if sift else None), geo


def ratios(cb, sift):
    """{query: CH + PROJECTION time / pg_sift time}, over the queries both arms answered"""
    if not sift or 'ch_proj' not in cb['warm']:
        return {}
    a, b = cb['warm'][sift], cb['warm']['ch_proj']
    return {q: b[q] / a[q] for q in range(QUERIES) if q in a and q in b and a[q] > 0}


def title(sift, ratio):
    if not ratio:
        return 'ClickBench Q0–Q42 against ClickHouse + PROJECTION: not measured yet'
    wins = sum(r > TIE for r in ratio.values())
    return f'{ARMS[sift]} is faster than CH + PROJECTION on {wins} of {len(ratio)} ClickBench queries'


def tiles(c, ratio):
    step = (RIGHT - LEFT) / QUERIES
    for q in range(QUERIES):
        x, r = LEFT + q * step, ratio.get(q)
        if r is None:
            c.rect(x + .5, 74, step - 2.5, 34, None, 2, stroke='grid')
            continue
        lean = math.log2(r) if abs(r - 1) > TIE - 1 else 0
        c.rect(x, 74, step - 2, 34, 'soft' if not lean else 'col' if lean > 0 else 'amber', 2, opacity=n_op(lean))
    for q in range(0, QUERIES, 6):
        c.text(LEFT + q * step + (step - 2) / 2, 126, f'Q{q}', 'sub', anchor='middle')


def n_op(lean):
    return round(.35 + .65 * min(1, abs(lean) / 3), 2) if lean else .55


def pills(c, sift, geo):
    c.text(LEFT, 156, 'geometric mean of the 43 warm wall times (ms, lower is better)', bold=True)
    order = sorted(geo, key=geo.get) if geo else list(ARMS)
    X = scale(0, max(geo.values()) if geo else 1, 190, 700)
    for i, arm in enumerate(order):
        y = 180 + i * 18
        c.text(LEFT, y, ARMS[arm], 'text' if role(arm) != 'rival' else 'sub')
        if not geo:
            c.rect(190, y - 11, 160, 13, None, 6.5, stroke='grid')
            continue
        rival_wins = role(arm) == 'rival' and sift and geo[arm] < geo[sift]
        c.rect(190, y - 11, X(geo[arm]) - 190, 13, 'amber' if rival_wins else role(arm) if role(arm) != 'rival' else 'soft', 6.5)
        c.text(X(geo[arm]) + 8, y, short(geo[arm]) + ' ms', 'text')
    if not geo:
        c.text(370, 180 + 2 * 18 + 15, '`bench/bench clickbench run` fills the tiles and the bars', 'sub', anchor='start')


def families(c, ratio):
    c.text(LEFT, 296, 'five query families: CH + PROJECTION time against pg_sift (geometric mean ratio)', bold=True)
    col = (RIGHT - LEFT) / len(CB_FAMILIES)
    for i, (name, qs) in enumerate(CB_FAMILIES):
        x, got = LEFT + i * col, [ratio[q] for q in qs if q in ratio]
        c.line(x, 308, x, 362, 'axis', 2)
        c.text(x + 10, 324, name, bold=True)
        c.text(x + 10, 342, f'{sum(r > TIE for r in got)} / {len(got)} faster' if got else f'{len(qs)} queries', 'col' if got else 'sub')
        c.text(x + 10, 358, f'{geomean(got):.2f}× geomean' if got else 'not measured yet', 'sub')


def render(data, mode):
    cb = data['clickbench']
    sift, geo = standings(cb)
    ratio = ratios(cb, sift)
    c = Canvas(mode, HEIGHT)
    meta = cb['meta'] if cb else {}
    c.header(title(sift, ratio), (f'{meta["tier"]} hits rows · ' if cb else '') + 'one tile a query: blue pg_sift faster, amber CH + PROJECTION faster, outline not measured')
    tiles(c, ratio)
    pills(c, sift, geo)
    families(c, ratio)
    c.footer('Data: docs/results/clickbench.tsv · every answer md5_equal to stock PostgreSQL 18' if cb else 'Data: docs/results/clickbench.tsv is not there yet')
    return c.svg(title(sift, ratio), '; '.join(f'{ARMS[a]} {g:,.1f} ms' for a, g in geo.items()) or 'ClickBench is not measured yet')
