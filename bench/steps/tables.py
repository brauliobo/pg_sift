"""Turns the result directories of steps/ab.sh and steps/places.sh into chart input and Markdown tables.

    python3 -I steps/tables.py OUTDIR [--engines a,b] [--per-dataset DIR] DATASET:TIER:RESULT_DIR ...

DATASET is articles (a steps/ab.sh directory: cells.tsv, builds.tsv) or places (a steps/places.sh
directory), TIER quick, ab or full, or <tier>-cold for the directory of a cold-cache run. --engines keeps only those engine ids. Writes to OUTDIR:

  results.tsv   one row per measurement, tab separated, first line a `#` comment naming the columns:
                dataset  tier  engine  class  variant  metric  value  min  max  unit
                articles: class = and|or|phrase|mixed|count|filter|join, variant = -, metric = qps (median
                over the interleaved rounds, min and max over the rounds).
                places: class = p1..p4|w1|a2|a2s|a3|x2, variant = top|boost|filter|count|scan, metric = p50_ms
                or p95_ms (single client, median over the rounds).
                both: class = variant = - and metric = index_mib | build_s | setup_s | heap_growth_mib (one
                build per engine; build_s is CREATE INDEX, setup_s the work before it, heap_growth_mib the
                table growth of that setup) and load_1m (host load average of the cells; engine -). The extra
                columns and indexes of an emulated workload (places: rum boost, pg_textsearch ranked prefixes)
                are the same metrics with variant = emulation.
                cold-cache runs (tier full-cold): metric = cold_p50_ms | cold_max_ms of 12 queries per class
                (articles: class = and|or|..., variant = -; places: class = p1.., variant = the workload).
                A value of NA means the engine cannot run the class, error that its statement failed.
  builds.tsv    dataset tier engine setup_s build_s index_bytes heap_growth_bytes (unrounded)
  tables.md     the same as Markdown tables, index size and build time next to the throughput / latency

--per-dataset DIR also writes DIR/<dataset>.tsv with the rows of each dataset (docs/results/); the rows of the
dataset/tier pairs given here replace those of the same pair in the file, the other tiers stay.
Engine ids, order and labels: bench/engines.tsv.
"""
import collections, os, statistics as st, sys

with open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'engines.tsv')) as f:   # the engines, in table order
    NAME = {r[0]: r[2] for r in (l.rstrip('\n').split('\t') for l in f if l.strip() and not l.startswith('#'))}
ORDER = list(NAME)
MIB = 1048576
COLUMNS = '# columns: dataset tier engine class variant metric value min max unit (tab separated; NA = the engine cannot run the class, error = the statement failed; bench/steps/tables.py)'
HEADER = 'dataset\ttier\tengine\tclass\tvariant\tmetric\tvalue\tmin\tmax\tunit'


def rows_of(path, skip_header=True):
    with open(path) as f:
        lines = [l.rstrip('\n').split('\t') for l in f if l.strip()]
    return lines[1:] if skip_header else lines


def num(x):
    try:
        return float(x)
    except ValueError:
        return None


def fmt(v, digits=0):
    return v if isinstance(v, str) else ('%%.%df' % digits) % v


def stats(values):
    """(median, min, max) of the numbers in values; NA when it holds None, error when it holds NaN"""
    if any(v is None for v in values):
        return 'NA', 'NA', 'NA'
    if any(v != v for v in values):
        return 'error', 'error', 'error'
    return st.median(values), min(values), max(values)


def cold_rows(dataset, tier, cells):
    """cells: (variant, class, engine, p50, max) -> rows of the cold-cache metrics"""
    return [(dataset, tier, e, c, v, m, x, x, x, 'ms') for v, c, e, p50, mx in cells for m, x in (('cold_p50_ms', p50), ('cold_max_ms', mx))]


def articles(tier, path):
    if tier.endswith('-cold'):   # class engine p50_ms max_ms
        return cold_rows('articles', tier, [('-', *r) for r in rows_of(path + '/summary.tsv')]), [], []
    out, builds = [], []
    cells = rows_of(path + '/cells.tsv')
    engines = list(dict.fromkeys(c[3] for c in cells))
    classes = list(dict.fromkeys(c[2] for c in cells))
    for e in engines:
        for c in classes:
            v = [num(x[4]) or float('nan') for x in cells if x[2] == c and x[3] == e and x[4] != 'NA']   # 0: a client failed
            out.append(('articles', tier, e, c, '-', 'qps', *(stats(v) if v else ('NA',) * 3), 'queries/s'))
    for b in rows_of(path + '/builds.tsv', skip_header=False):   # tier engine docs setup_s build_s index_bytes heap_growth
        builds.append(('articles', tier, b[1], b[3], b[4], b[5], b[6], '-'))
    return out, builds, [float(c[1]) for c in cells]


def places(tier, path):
    if tier.endswith('-cold'):   # workload class engine p50_ms max_ms
        return cold_rows('places', tier, [(r[0], r[1], r[2], r[3], r[4]) for r in rows_of(path + '/cold.tsv')]), [], []
    out, builds = [], []
    cells = rows_of(path + '/cells.tsv')
    for wl, c, e in dict.fromkeys((c[2], c[3], c[4]) for c in cells):
        sel = [x for x in cells if (x[2], x[3], x[4]) == (wl, c, e)]
        for metric, col in (('p50_ms', 5), ('p95_ms', 6)):
            out.append(('places', tier, e, c, wl, metric, *stats([None if x[col] == 'NA' else num(x[col]) for x in sel]), 'ms'))
    for b in rows_of(path + '/builds.tsv', skip_header=False):   # engine build_s index_bytes heap_growth setup_s variant
        builds.append(('places', tier, b[0], b[4], b[1], b[2], b[3], b[5]))
    return out, builds, [float(c[1]) for c in cells]


def build_rows(builds):
    out = []
    for d, t, e, setup, build, size, heap, variant in builds:
        mib = lambda x: '%.1f' % (max(float(x), 0) / MIB)   # VACUUM can shrink the table below its size before the setup
        out += [(d, t, e, '-', variant, 'index_mib', mib(size), mib(size), mib(size), 'MiB'), (d, t, e, '-', variant, 'build_s', build, build, build, 's'),
                (d, t, e, '-', variant, 'setup_s', setup, setup, setup, 's'), (d, t, e, '-', variant, 'heap_growth_mib', mib(heap), mib(heap), mib(heap), 'MiB')]
    return out


def get(rows, dataset, tier, engine, cls='-', variant='-', metric=None):
    return next((r for r in rows if r[:6] == (dataset, tier, engine, cls, variant, metric)), None)


def value(rows, *key, **kw):
    r = get(rows, *key, **kw)
    return r[6] if r else 'NA'


def ratio(a, b):
    try:
        return '%.2fx' % (float(a) / float(b))
    except (ValueError, ZeroDivisionError):
        return 'n/a'


def md_table(head, body, left=1):
    return ['| ' + ' | '.join(head) + ' |', '|' + '---|' * left + '---:|' * (len(head) - left)] + ['| ' + ' | '.join(r) + ' |' for r in body]


def build_cols(rows, dataset, tier, e):
    return [value(rows, dataset, tier, e, metric=m) for m in ('index_mib', 'build_s')]


def md_articles(rows, tier):
    engines = [e for e in ORDER if get(rows, 'articles', tier, e, 'and', '-', 'qps')]
    classes = list(dict.fromkeys(r[3] for r in rows if r[:2] == ('articles', tier) and r[5] == 'qps'))
    ref = 'pg_search' if 'pg_search' in engines else engines[-1]
    growth = any(float(value(rows, 'articles', tier, e, metric='heap_growth_mib') or 0) > 0.5 for e in engines)
    qps = lambda e, c: get(rows, 'articles', tier, e, c, '-', 'qps')
    shown = lambda r: fmt(float(r[6])) + ' (%s-%s)' % (fmt(float(r[7])), fmt(float(r[8]))) if r and r[6] not in ('NA', 'error') else (r[6] if r else 'NA')
    head = ['engine', 'index (MiB)', 'build (s)'] + (['table growth (MiB)'] if growth else []) + classes
    body = [[NAME[e]] + build_cols(rows, 'articles', tier, e) + ([value(rows, 'articles', tier, e, metric='heap_growth_mib')] if growth else [])
            + [shown(qps(e, c)) for c in classes] for e in engines]
    out = ['Queries per second, median (minimum-maximum over the rounds); index size and CREATE INDEX time of the one build per engine:', ''] + md_table(head, body)
    rel = [[NAME[e] + ' / ' + NAME[ref]] + [ratio(value(rows, 'articles', tier, e, c, '-', 'qps'), value(rows, 'articles', tier, ref, c, '-', 'qps')) for c in classes]
           for e in engines if e != ref]
    return out + ['', 'Ratio of queries per second to %s:' % NAME[ref], ''] + md_table(['engine'] + classes, rel)


def md_places(rows, tier):
    engines = [e for e in ORDER if any(r[:3] == ('places', tier, e) and r[5] == 'p50_ms' for r in rows)]
    sel = [r for r in rows if r[:2] == ('places', tier) and r[5] == 'p50_ms']
    classes = list(dict.fromkeys(r[3] for r in sel))
    first = sel[0][4]
    p50 = lambda e, c, wl: next((r[6] for r in sel if (r[2], r[3], r[4]) == (e, c, wl)), 'NA')
    show = lambda v, d=2: fmt(float(v), d) if v not in ('NA', 'error') else v
    head = ['engine', 'index (MiB)', 'build (s)'] + ['%s' % c for c in classes]
    body = [[NAME[e]] + build_cols(rows, 'places', tier, e) + [show(p50(e, c, first)) for c in classes] for e in engines]
    out = ['Index size, CREATE INDEX time and p50 latency in ms of the `%s` workload per query class (single client):' % first, ''] + md_table(head, body)
    emu = [e for e in engines if get(rows, 'places', tier, e, '-', 'emulation', 'index_mib')]
    if emu:
        metrics = ('index_mib', 'build_s', 'setup_s', 'heap_growth_mib')
        out += ['', 'Extra columns and indexes of the emulated workloads (rum boost; pg_textsearch top-20, boost, filtered):', '']
        out += md_table(['engine', 'index (MiB)', 'build (s)', 'column fill (s)', 'table growth (MiB)'],
                        [[NAME[e]] + [value(rows, 'places', tier, e, '-', 'emulation', m) for m in metrics] for e in emu])
    for wl in dict.fromkeys(r[4] for r in sel):
        body = []
        for c in classes:
            v = {e: p50(e, c, wl) for e in engines}
            body.append([c] + [show(v[e]) + ('' if e == 'sift' or v[e] in ('NA', 'error') else ' (%s)' % ratio(v[e], v['sift'])) for e in engines])
        out += ['', 'Workload `%s`, p50 latency in ms (ratio to sift):' % wl, ''] + md_table(['class'] + [NAME[e] for e in engines], body)
    return out


def md_cold(rows, dataset, tier):
    sel = {(r[2], r[4], r[3], r[5]): fmt(float(r[6]), 2) for r in rows if r[:2] == (dataset, tier)}   # (engine, variant, class, metric)
    engines = [e for e in ORDER if any(k[0] == e for k in sel)]
    keys = list(dict.fromkeys((k[1], k[2]) for k in sel))
    cell = lambda e, k: sel[(e,) + k + ('cold_p50_ms',)] + ' / ' + sel[(e,) + k + ('cold_max_ms',)] if (e,) + k + ('cold_p50_ms',) in sel else 'NA'
    places = dataset == 'places'
    body = [([k[0]] if places else []) + [k[1]] + [cell(e, k) for e in engines] for k in keys]
    return (['Cold cache (index, heap and TOAST evicted before every query), p50 / maximum execution time in ms of 12 queries per cell:', '']
            + md_table((['workload'] if places else []) + ['class'] + [NAME[e] for e in engines], body, 2 if places else 1))


def main(argv):
    outdir, per, keep, specs = argv[0], None, None, []
    args = iter(argv[1:])
    for a in args:
        if a == '--per-dataset':
            per = next(args)
        elif a == '--engines':
            keep = [e for e in next(args).split(',')]
        else:
            specs.append(a)
    rows, builds, loads, tiers = [], [], collections.defaultdict(list), []
    for spec in specs:
        dataset, tier, path = spec.split(':', 2)
        r, b, load = (articles if dataset == 'articles' else places)(tier, path)
        rows += [x for x in r if keep is None or x[2] in keep]
        builds += [x for x in b if keep is None or x[2] in keep]
        loads[dataset, tier] += load
        tiers.append((dataset, tier))
    rows += build_rows(builds)
    for (dataset, tier), v in loads.items():
        if v:
            rows.append((dataset, tier, '-', '-', '-', 'load_1m', '%.1f' % st.median(v), '%.1f' % min(v), '%.1f' % max(v), 'load average'))
    os.makedirs(outdir, exist_ok=True)

    def write(path, selected):
        with open(path, 'w') as f:
            f.write(COLUMNS + '\n' + HEADER + '\n')
            for r in selected:
                f.write('\t'.join(('%.2f' % x).rstrip('0').rstrip('.') if isinstance(x, float) else str(x) for x in r) + '\n')
    write(outdir + '/results.tsv', rows)
    if per:
        os.makedirs(per, exist_ok=True)
        for d in dict.fromkeys(r[0] for r in rows):
            path = '%s/%s.tsv' % (per, d)
            kept = [tuple(r) for r in rows_of(path, skip_header=False)[2:] if (r[0], r[1]) not in tiers] if os.path.exists(path) else []
            write(path, [r for r in rows if r[0] == d] + [r for r in kept if r[0] == d])
    with open(outdir + '/builds.tsv', 'w') as f:
        f.write('dataset\ttier\tengine\tsetup_s\tbuild_s\tindex_bytes\theap_growth_bytes\tvariant\n')
        for b in builds:
            f.write('\t'.join(b) + '\n')
    md = []
    for dataset, tier in tiers:
        md += ['### %s, %s tier' % (dataset, tier.replace('-cold', ' cold cache')), '']
        if tier.endswith('-cold'):
            md += md_cold(rows, dataset, tier) + ['']
            continue
        load = get(rows, dataset, tier, '-', '-', '-', 'load_1m')
        md += ['Host load average (1 min) during the cells: %s-%s (median %s).' % (load[7], load[8], load[6]), '']
        md += (md_articles if dataset == 'articles' else md_places)(rows, tier) + ['']
    open(outdir + '/tables.md', 'w').write('\n'.join(md))
    print('\n'.join(md))


if __name__ == '__main__':
    main(sys.argv[1:])
