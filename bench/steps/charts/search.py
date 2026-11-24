"""readme-search: throughput × index size of exact BM25 on 50K Wikipedia articles (top), the 10M-order hybrid searches and the cold p50 (bottom)."""
from .theme import LEFT, RIGHT, Canvas, fmt_mib, geomean, role, scale, short

NAME, HEIGHT = 'search', 420
ENGINES = {'sift': 'pg_sift', 'es': 'Elasticsearch 9.5', 'paradedb': 'ParadeDB', 'textsearch': 'pg_textsearch', 'rum': 'PG rum'}
SIZE_LABEL = {'sift': 'pg_sift (default)', 'es': 'Elasticsearch 9.5', 'paradedb': 'ParadeDB (pg_search)', 'textsearch': 'pg_textsearch 1.5', 'rum': 'PG rum (tsvector)'}
LABEL_AT = {'sift': (-10, -9, 'end'), 'es': (-10, 4, 'end'), 'paradedb': (10, 4, 'start'), 'textsearch': (10, 4, 'start'), 'rum': (10, 4, 'start')}
HYBRID = (('08', 'Search count(*)'), ('09', '+ filter, GROUP BY'), ('10', '+ ORDER BY ts LIMIT 10'))
COLD = (('count', 'Exact count(*)'), ('phrase', 'Phrase "a b"'), ('and', 'Boolean a AND b'), ('filter', 'Filtered top-k'), ('or', 'Boolean a OR b'), ('join', 'Joined top-k'))


def points(data):
    """{engine: (geometric mean QPS of the seven classes, index MiB)}"""
    sizes = {label: mib for label, mib, _, _ in data['storage_bm25']}
    qps = {e: geomean([vals[e] for _, _, vals in data['search']]) for e in ENGINES if all(e in vals for _, _, vals in data['search'])}
    return {e: (qps[e], sizes[SIZE_LABEL[e]]) for e in qps}


def title(pts):
    (q_s, s_s), (q_e, s_e) = pts['sift'], pts['es']
    return f"pg_sift: {q_s / q_e:.2f}× Elasticsearch's throughput in a {s_e / s_s:.1f}× smaller index"


def plot(c, pts):
    X, Y = scale(150, 30000, 84, 836, log=True), scale(0, 100, 74, 222)
    for t in (0, 25, 50, 75, 100):
        c.line(84, Y(t), 836, Y(t))
        c.text(76, Y(t) + 4, f'{t}', 'sub', anchor='end')
    for t in (200, 1000, 5000, 20000):
        c.line(X(t), 74, X(t), 222)
        c.text(X(t), 240, f'{t:,}', 'sub', anchor='middle')
    c.text(LEFT, 66, '↓ index size (MiB, smaller is better)', 'sub')
    c.text(RIGHT, 66, 'geomean warm QPS of 7 query classes, 8 clients (log) →', 'sub', anchor='end')
    c.text(RIGHT, 88, 'better ↗', 'sub', anchor='end')
    for e, (qps, mib) in pts.items():
        beats = e != 'sift' and (qps > pts['sift'][0] or mib < pts['sift'][1])
        c.circle(X(qps), Y(mib), 6.5, 'amber' if beats else role(e))
        dx, dy, anchor = LABEL_AT[e]
        c.text(X(qps) + dx, Y(mib) + dy, f'{ENGINES[e]} · {fmt_mib(mib)}', 'text' if e == 'sift' else 'sub', anchor=anchor, bold=e == 'sift')


def block(c, x, y0, title_text, cols, rows, value):
    """a small table: cols (key, header, center x), rows (key, label); amber where a rival is faster than the first column"""
    c.text(x, y0, title_text, bold=True)
    for key, header, cx in cols:
        c.text(cx, y0 + 20, header, 'sub', anchor='middle')
    for i, (rk, label) in enumerate(rows):
        y = y0 + 40 + i * 16
        c.text(x, y, label, 'sub')
        vals = {key: value(rk, key) for key, _, _ in cols}
        for key, _, cx in cols:
            v = vals[key]
            if v is not None:
                slow = key != cols[0][0] and vals[cols[0][0]] is not None and v < vals[cols[0][0]]
                c.text(cx, y, short(v), 'amber' if slow else 'col' if key == cols[0][0] else 'text', anchor='middle', bold=slow or key == cols[0][0])


def render(data, mode):
    c, pts, tier = Canvas(mode, HEIGHT), points(data), data['orders_tier']
    c.header(title(pts), '50,000 Simple English Wikipedia articles · exact BM25, md5-equal rank order · amber: a rival beats pg_sift on one axis')
    plot(c, pts)
    cold, hybrid = data['search_cold'], data['search_10m']
    block(c, LEFT, 268, f'{tier} orders, hybrid search, warm ms', [('sift', 'pg_sift', 250), ('es', 'ES 9.5', 322), ('pg', 'PG GIN', 394)], HYBRID,
          lambda q, e: hybrid[q].get(e))
    c.text(LEFT, 356, '5M orders: not measured yet', 'sub')
    block(c, 450, 268, '50K Wikipedia, cold p50 ms', [('sift', 'pg_sift', 620), ('es', 'ES 9.5', 700), ('paradedb', 'ParadeDB', 784)], COLD,
          lambda k, e: cold[k].get(e))
    c.footer('Data: articles.tsv (slim, slim-cold), store-scaling.json · Ryzen 7 9700X · 2026-10-27 · pg_sift positions=off: 12.0 MiB')
    return c.svg(title(pts), '; '.join(f'{ENGINES[e]} {q:,.0f} QPS, {fmt_mib(m)}' for e, (q, m) in pts.items()))
