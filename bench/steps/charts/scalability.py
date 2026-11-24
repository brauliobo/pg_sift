"""readme-scalability: Tufte small multiples of the executor's scaling: rows, workers, cold I/O (top), partitions, writers and the opt-in GPU (bottom)."""
from .theme import LEFT, Canvas, fmt_qps, scale

NAME, HEIGHT = 'scalability', 400
PANEL_W, PLOT_W = 280, 120
WARM = (('A1_scan_filter', 'Scan'), ('A2_lowcard_groupby', 'Low-card'), ('A3_highcard_groupby', 'High-card'), ('A4_count_distinct', 'Distinct'),
        ('A5_parallel_build', 'Build'), ('A6_parallel_vacuum', 'Vacuum'))
COLD = (('B1_cold_wide_scan', 'Wide'), ('B2_cold_narrow_scan', 'Narrow'), ('B3_cold_table_bitmap', 'Bitmap'), ('B4_cold_bm25', 'BM25'))
IO_METHODS = (('io_uring', 'col'), ('worker', 'soft'), ('sync', 'rival'))


def origin(i):
    return LEFT + (i % 3) * PANEL_W, 82 if i < 3 else 252


def frame(c, x, y, title, plot_h, ends):
    """the title of a panel and its two end labels of the x axis; the top of the plot"""
    c.text(x, y, title, bold=True)
    c.text(x, y + 28 + plot_h + 16, ends[0], 'sub')
    c.text(x + PLOT_W, y + 28 + plot_h + 16, ends[1], 'sub', anchor='end')
    return y + 28


def empty(c, x, top, h, note='not measured yet'):
    c.line(x, top + h, x + PLOT_W, top, 'grid', 1.5, '4 4')
    c.line(x, top + h, x + PLOT_W, top + h, 'axis')
    c.text(x + PLOT_W + 12, top + h / 2 + 4, note, 'sub')


def rows_panel(c, i):
    x, y = origin(i)
    h = 70
    top = frame(c, x, y, 'Rows 100k → 5M', h, ('100k', '5M'))
    empty(c, x, top, h)


def workers_panel(c, i, scaling):
    x, y = origin(i)
    h = 70
    top = frame(c, x, y, 'Workers 1 → 8: warm speedup', h, ('1', '8 workers'))
    warm = {p: scaling['warm'][p] for p, _ in WARM if scaling and p in scaling['warm']}
    if not warm:
        return empty(c, x, top, h)
    X, Y = scale(1, 8, x, x + PLOT_W, log=True), scale(1, 8, top + h, top)
    c.line(X(1), Y(1), X(8), Y(8), 'grid', 1.5, '4 4')
    ends = []
    for probe, name in WARM:
        pts = sorted(warm.get(probe, {}).items())
        if pts:
            c.poly([(X(w), Y(v)) for w, v in pts], 'col', 1.8)
            ends.append((Y(pts[-1][1]), f'{pts[-1][1]:.1f}× {name}'))
    last = -99
    for yy, label in sorted(ends):
        last = max(yy, last + 13)
        c.text(x + PLOT_W + 8, last + 4, label, 'text')


def cold_panel(c, i, scaling):
    x, y = origin(i)
    h = 70
    top = frame(c, x, y, 'Cold I/O: io_uring · worker · sync', h, ('0', 'MB/s'))
    cold = {p: scaling['cold'][p] for p, _ in COLD if scaling and p in scaling['cold']}
    if not cold:
        return empty(c, x, top, h)
    X = scale(0, max(v for at in cold.values() for v in at.values()), x + 44, x + PLOT_W + 60)
    for r, (probe, name) in enumerate(p for p in COLD if p[0] in cold):
        yy = top + r * 18
        c.text(x, yy + 9, name, 'sub')
        base = cold[probe].get('io_uring', 0)
        for k, (method, color) in enumerate(IO_METHODS):
            if method in cold[probe]:
                v = cold[probe][method]
                c.rect(x + 44, yy + k * 5, X(v) - X(0), 4, 'amber' if v > base else color, 1.5)
        c.text(X(base) + 6, yy + 9, f'{fmt_qps(base)}', 'text')
    c.legend(x, top + h + 36, [(color, method) for method, color in IO_METHODS])


def writers_panel(c, i, scaling):
    x, y = origin(i)
    h = 50
    top = frame(c, x, y, 'Writers 1 → 16: ops/s', h, ('1', '16 clients'))
    writers = scaling['writers'] if scaling else {}
    if not writers:
        return empty(c, x, top, h)
    X, Y = scale(1, 16, x, x + PLOT_W, log=True), scale(0, max(writers.values()), top + h, top)
    pts = [(X(k), Y(v)) for k, v in sorted(writers.items())]
    c.poly(pts, 'col', 1.8)
    for px, py in pts:
        c.circle(px, py, 3, 'col')
    c.text(pts[-1][0] + 8, pts[-1][1] + 4, f'{fmt_qps(max(writers.values()))} ops/s', 'text')


def static_panel(c, i, title, ends):
    x, y = origin(i)
    empty(c, x, frame(c, x, y, title, 50, ends), 50)


def title(scaling):
    if not scaling or not scaling['warm']:
        return 'Scalability and parallelism: not measured yet'
    best, speed = max(((p, v[max(v)]) for p, v in scaling['warm'].items()), key=lambda kv: kv[1])
    return f'Warm scans scale {speed:.1f}× on {max(scaling["warm"][best])} workers; cold reads follow io_uring'


def render(data, mode):
    c, scaling = Canvas(mode, HEIGHT), data['scaling']
    c.header(title(scaling), 'each panel is its own scale: pg_sift against its own 1-worker, sync and 1-client runs · dashed: ideal linear · gray: not measured yet')
    rows_panel(c, 0)
    workers_panel(c, 1, scaling)
    cold_panel(c, 2, scaling)
    static_panel(c, 3, 'Partitions 1 → 4,096', ('1', '4,096'))
    writers_panel(c, 4, scaling)
    static_panel(c, 5, 'Opt-in Vulkan GPU vs CPU', ('CPU', 'GPU'))
    meta = scaling['meta'] if scaling else {}
    c.footer(f'Data: v6-scaling.tsv · {meta["cpu"]} · {meta["date"]}' if scaling else 'Data: `bench/bench scaling-io --docs` writes docs/results/v6-scaling.tsv')
    desc = ', '.join(f'{n} {max(scaling["warm"][p].values()):.1f}x' for p, n in WARM if scaling and p in scaling['warm']) or 'scaling is not measured yet'
    return c.svg(title(scaling), 'Warm speedup at the most workers: ' + desc)
