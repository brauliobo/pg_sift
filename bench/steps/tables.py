"""Turns the result directories of steps/articles.sh and steps/places.sh into chart input and Markdown tables.

    python3 -I steps/tables.py OUTDIR [--engines a,b] [--per-dataset DIR] DATASET:TIER:RESULT_DIR ...

DATASET is articles (a steps/articles.sh directory: cells.tsv, builds.tsv) or places (a steps/places.sh
directory), TIER slim, ab or full (quick is the old name of slim), or <tier>-cold for the directory of a cold-cache run (cold.tsv). --engines keeps only those
engine ids. Writes to OUTDIR:

  results.tsv   one row per measurement, tab separated, first line a `#` comment naming the columns:
                dataset  tier  engine  class  variant  metric  value  min  max  unit  stderr  n  values
                articles: class = and|or|phrase|mixed|count|filter|join|term|prefix|fuzzy|update, variant = -, metric = qps
                (median over the interleaved rounds, min and max over the rounds); class update also has the metrics write_ops
                (writer operations per second) and wal_mib (WAL written during the cell).
                places: class = p1..p4|w1|a2|a2s|a3|x2, variant = top|boost|filter|count|scan, metric = p50_ms
                or p95_ms (single client, median over the rounds).
                both: class = variant = - and metric = index_mib | build_s | setup_s | heap_growth_mib (one
                build per engine; build_s is CREATE INDEX, setup_s the work before it, heap_growth_mib the
                table growth of that setup) and load_1m (host load average of the cells; engine -). The extra
                columns and indexes of an emulated workload (places: rum boost, pg_textsearch ranked prefixes)
                are the same metrics with variant = emulation.
                host rows (engine -): corpus = documents of the dataset (corpus.txt of the directory), wall_s = seconds of a phase of the
                session (phases.txt, variant = the phase), session = the host and versions (machine.txt; variant = key=value words
                joined by `;`: date, cpu, threads, pg, sift, elasticsearch).
                cold-cache runs (tier full-cold): metric = cold_p50_ms | cold_max_ms of up to 12 queries per class
                (articles: class = and|or|..., variant = -; places: class = p1.., variant = the workload).
                A value of NA means the engine cannot run the class, error that its statement failed.
                The uncertainty of a cell: n is the number of interleaved rounds, values their results (comma separated, round
                order), stderr the standard error of the cell: the larger of the sample standard deviation of the rounds divided
                by sqrt(n) and the error the rounds' own standard errors leave, sqrt(sum se^2) / n (cells.tsv has the se of each
                round: batch means of the queries per second, order statistics of the p50, steps/cell.py; directories written before
                that have the first only). NA for n < 2 without a round error (one build per engine, cold-cache cells). With n = 2-3
                the first is a rough indication, not a confidence interval: the range (min, max) is the other half of the picture.
  builds.tsv    dataset tier engine setup_s build_s index_bytes heap_growth_bytes (unrounded)
  tables.md     the same as Markdown tables, index size and build time next to the throughput / latency

--per-dataset DIR also writes DIR/<dataset>.tsv with the rows of each dataset (docs/results/); the rows of the
dataset/tier pairs given here replace those of the same pair in the file, the other tiers stay.
Engine ids, order and labels: bench/engines.tsv.
"""
import collections, math, os, re, statistics as st, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stats import number as num, sem  # noqa: E402  (steps/stats.py)

with open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'engines.tsv')) as f:   # the engines, in table order
    NAME = {r[0]: r[2] for r in (l.rstrip('\n').split('\t') for l in f if l.strip() and not l.startswith('#'))}
ORDER = list(NAME)
MIB = 1048576
UNIT = {'qps': 'queries/s', 'write_ops': 'ops/s', 'wal_mib': 'MiB'}
COLUMNS = '# columns: dataset tier engine class variant metric value min max unit stderr n values (tab separated; NA = the engine cannot run the class, error = the statement failed; stderr = the larger of sd / sqrt(n) over the n interleaved rounds (NA below 2) and the errors of the rounds themselves, sqrt(sum se^2) / n; values = the rounds; bench/steps/tables.py)'
HEADER = 'dataset\ttier\tengine\tclass\tvariant\tmetric\tvalue\tmin\tmax\tunit\tstderr\tn\tvalues'
WIDTH = 13
def cpus_text(quota):
    """where the engines ran: all the CPUs of the host, or the scope of the session (`systemd-run -p CPUQuota`: the server, the clients and a Java process all inside it)"""
    return 'every CPU of the host' if not quota else 'the %s of the scope the session ran in (server, clients and Java process together; the host has more)' % quota


def method(quota=''):
    return 'Method: one engine at a time on %s (the servers of the other engines idle), the engines interleaved in rounds, the same clients for all.' % cpus_text(quota)


def session_quota(rows, dataset, tier):
    """the CPU quota word of the session row of a table ('' when the host's CPUs were all available)"""
    r = next((r for r in rows if r[0] == dataset and r[1] == tier and r[5] == 'session'), None)
    return dict(w.split('=', 1) for w in r[4].split(';')).get('quota', '') if r else ''


METHOD = method()
FLAG = 0.10   # a relative standard error above this is flagged in the tables and the charts


def rows_of(path, skip_header=True):
    with open(path) as f:
        lines = [l.rstrip('\n').split('\t') for l in f if l.strip()]
    return lines[1:] if skip_header else lines


def fmt(v, digits=0):
    return v if isinstance(v, str) else ('%%.%df' % digits) % v


def short(v):
    return 'error' if v != v else ('%.3f' % v).rstrip('0').rstrip('.')


def stats(values, errors=()):
    """(median, min, max, stderr, n, values) of the per-round numbers: NA when it holds None, error when it holds NaN;
    stderr = the larger of the sample standard deviation / sqrt(n) (a single round has none) and sqrt(sum se^2) / n over the
    standard errors of the rounds (errors, in the unit of the values; none when a round has none), NA without either"""
    n, rounds = str(len(values)), ','.join('NA' if v is None else short(v) for v in values)
    if any(v is None for v in values):
        return 'NA', 'NA', 'NA', 'NA', n, rounds
    if any(v != v for v in values):
        return 'error', 'error', 'error', 'NA', n, rounds
    spread = sem(values)
    own = math.sqrt(sum(e * e for e in errors)) / len(values) if len(errors) == len(values) and all(e is not None for e in errors) else None
    known = [e for e in (spread, own) if e is not None]
    return st.median(values), min(values), max(values), '%.4g' % max(known) if known else 'NA', n, rounds


def single(value):
    """the uncertainty columns of a measurement that is made once"""
    return 'NA', '1', str(value)


def cold_rows(dataset, tier, cells):
    """cells: (variant, class, engine, p50, max) -> rows of the cold-cache metrics"""
    return [(dataset, tier, e, c, v, m, x, x, x, 'ms', *single(x)) for v, c, e, p50, mx in cells for m, x in (('cold_p50_ms', p50), ('cold_max_ms', mx))]


def articles(tier, path):
    if tier.endswith('-cold'):   # workload class engine p50_ms max_ms
        return cold_rows('articles', tier, [tuple(r[:5]) for r in rows_of(path + '/cold.tsv')]), [], []
    out, builds = [], []
    cells = rows_of(path + '/cells.tsv')   # round load class engine metric value [se n secs]
    engines = list(dict.fromkeys(c[3] for c in cells))
    keys = list(dict.fromkeys((c[2], c[4]) for c in cells))
    for e in engines:
        for c, m in keys:
            sel = [x for x in cells if (x[2], x[3], x[4]) == (c, e, m) and x[5] != 'NA']
            v = [num(x[5]) if m != 'qps' or num(x[5]) else float('nan') for x in sel]   # a qps of 0: a client failed
            t = stats(v, [num(x[6]) if len(x) > 6 else None for x in sel]) if v else ('NA',) * 4 + ('0', 'NA')
            out.append(('articles', tier, e, c, '-', m, *t[:3], UNIT[m], *t[3:]))
    for b in rows_of(path + '/builds.tsv', skip_header=False):   # tier engine docs setup_s build_s index_bytes heap_growth [variant]
        builds.append(('articles', tier, b[1], b[3], b[4], b[5], b[6], b[7] if len(b) > 7 else '-'))
    return out, builds, [float(c[1]) for c in cells]


def places(tier, path):
    if tier.endswith('-cold'):   # workload class engine p50_ms max_ms
        return cold_rows('places', tier, [(r[0], r[1], r[2], r[3], r[4]) for r in rows_of(path + '/cold.tsv')]), [], []
    out, builds = [], []
    cells = rows_of(path + '/cells.tsv')
    for wl, c, e in dict.fromkeys((c[2], c[3], c[4]) for c in cells):
        sel = [x for x in cells if (x[2], x[3], x[4]) == (wl, c, e)]
        for metric, col in (('p50_ms', 5), ('p95_ms', 6)):
            t = stats([None if x[col] == 'NA' else num(x[col]) for x in sel], [num(x[7]) if metric == 'p50_ms' and len(x) > 7 else None for x in sel])
            out.append(('places', tier, e, c, wl, metric, *t[:3], 'ms', *t[3:]))
    for b in rows_of(path + '/builds.tsv', skip_header=False):   # engine build_s index_bytes heap_growth setup_s variant
        builds.append(('places', tier, b[0], b[4], b[1], b[2], b[3], b[5]))
    return out, builds, [float(c[1]) for c in cells]


def build_rows(builds):
    out = []
    for d, t, e, setup, build, size, heap, variant in builds:
        mib = lambda x: '%.1f' % (max(float(x), 0) / MIB)   # VACUUM can shrink the table below its size before the setup
        out += [(d, t, e, '-', variant, 'index_mib', mib(size), mib(size), mib(size), 'MiB', *single(mib(size))),
                (d, t, e, '-', variant, 'build_s', build, build, build, 's', *single(build)),
                (d, t, e, '-', variant, 'setup_s', setup, setup, setup, 's', *single(setup)),
                (d, t, e, '-', variant, 'heap_growth_mib', mib(heap), mib(heap), mib(heap), 'MiB', *single(mib(heap)))]
    return out


def host_rows(dataset, tier, path):
    """what a result directory says about its session, as rows of the engine `-`: the size of the corpus (corpus.txt, documents), the
    seconds of each phase (phases.txt, metric wall_s, variant = the phase) and the host and versions (machine.txt, metric session, variant =
    `key=value` words separated by `;`)"""
    out = []
    try:
        n = int(open(os.path.join(path, 'corpus.txt')).read())
        out.append((dataset, tier, '-', '-', '-', 'corpus', n, n, n, 'documents', *single(n)))
    except (OSError, ValueError):
        pass
    try:
        for line in open(os.path.join(path, 'phases.txt')):
            name, secs = line.rstrip('\n').split('\t')
            out.append((dataset, tier, '-', '-', name, 'wall_s', secs, secs, secs, 's', *single(secs)))
    except OSError:
        pass
    try:
        text = open(os.path.join(path, 'machine.txt')).read()
    except OSError:
        return out
    grab = lambda key, pat='(.*)': (re.search(r'^%s: %s' % (key, pat), text, re.M) or [None, ''])[1].strip().replace(';', ',')
    cpu = re.sub(r'\s+\d+-Core Processor$', '', grab('cpu'))
    quota = grab('cpu quota')
    words = [('date', grab('date')[:10]), ('cpu', cpu), ('threads', grab('cpu threads', r'(\d+)')), ('quota', '' if quota in ('', 'none') else quota), ('pg', grab('postgres')), ('sift', grab('sift commit')),
             ('elasticsearch', grab('elasticsearch', r'(\S+)'))]
    out.append((dataset, tier, '-', '-', ';'.join('%s=%s' % kv for kv in words if kv[1]), 'session', 1, 1, 1, '-', *single(1)))
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


def pm(r, digits=0):
    """a result row as `value ± stderr` (the range is added by the tables that have room), ` !` when the relative standard error
    is above FLAG; NA / error and rows without a standard error stay as they are"""
    if r is None or r[6] in ('NA', 'error'):
        return 'NA' if r is None else r[6]
    v, se = float(r[6]), r[10] if len(r) > 10 else 'NA'
    if se == 'NA':
        return fmt(v, digits)
    sd = digits if float(se) >= 10 ** -digits * 10 else digits + 1
    return fmt(v, digits) + ' ± ' + fmt(float(se), sd) + (' !' if v and float(se) / v > FLAG else '')


LEGEND = ('`±` is one standard error of the median over the n interleaved rounds (sample standard deviation / sqrt(n); with n = 2-3 a rough '
          'indication, read it with the range); `!` marks a relative standard error above %d%%.' % (FLAG * 100))


def build_cols(rows, dataset, tier, e):
    return [value(rows, dataset, tier, e, metric=m) for m in ('index_mib', 'build_s')]


def md_merge(rows, dataset, tier, engines):
    """the force-merge of an engine that merges after loading (Elasticsearch): its own row, `build` of the main row is the bulk load and refresh"""
    merge = [e for e in engines if get(rows, dataset, tier, e, '-', 'forcemerge', 'build_s')]
    if not merge:
        return []
    return ['', 'Force-merge to one segment after the bulk load and refresh (the build column is the bulk load and refresh, the index column the size after the merge):', ''] + md_table(
        ['engine', 'force-merge (s)', 'index before the merge (MiB)'],
        [[NAME[e], value(rows, dataset, tier, e, '-', 'forcemerge', 'build_s'), value(rows, dataset, tier, e, '-', 'forcemerge', 'index_mib')] for e in merge])


def md_articles(rows, tier):
    measured = [r for r in rows if r[:2] == ('articles', tier) and r[5] == 'qps']
    engines = [e for e in ORDER if any(r[2] == e for r in measured)]
    classes = list(dict.fromkeys(r[3] for r in measured))
    ref = 'pg_search' if 'pg_search' in engines else engines[-1]
    growth = any(float(value(rows, 'articles', tier, e, metric='heap_growth_mib') or 0) > 0.5 for e in engines)
    qps = lambda e, c: get(rows, 'articles', tier, e, c, '-', 'qps')
    shown = lambda r: pm(r) + ' (%s-%s)' % (fmt(float(r[7])), fmt(float(r[8]))) if r and r[6] not in ('NA', 'error') else (r[6] if r else 'NA')
    head = ['engine', 'index (MiB)', 'build (s)'] + (['table growth (MiB)'] if growth else []) + classes
    body = [[NAME[e]] + build_cols(rows, 'articles', tier, e) + ([value(rows, 'articles', tier, e, metric='heap_growth_mib')] if growth else [])
            + [shown(qps(e, c)) for c in classes] for e in engines]
    out = ['Queries per second, median ± standard error (minimum-maximum over the rounds); index size and CREATE INDEX time of the one build per engine:', ''] + md_table(head, body)
    out += md_merge(rows, 'articles', tier, engines)
    out += ['', LEGEND]
    rel = [[NAME[e] + ' / ' + NAME[ref]] + [ratio(value(rows, 'articles', tier, e, c, '-', 'qps'), value(rows, 'articles', tier, ref, c, '-', 'qps')) for c in classes]
           for e in engines if e != ref]
    out += ['', 'Ratio of queries per second to %s:' % NAME[ref], ''] + md_table(['engine'] + classes, rel)
    upd = [e for e in engines if get(rows, 'articles', tier, e, 'update', '-', 'write_ops')]
    if upd:
        out += ['', 'Continuous updates (writers at a fixed rate: 70% update, 15% insert, 15% delete) with `mixed` readers:', '']
        out += md_table(['engine', 'read queries/s', 'writer ops/s', 'WAL written (MiB)'],
                        [[NAME[e]] + [pm(get(rows, 'articles', tier, e, 'update', '-', m)) for m in ('qps', 'write_ops', 'wal_mib')] for e in upd])
    return out


def md_places(rows, tier):
    engines = [e for e in ORDER if any(r[:3] == ('places', tier, e) and r[5] == 'p50_ms' for r in rows)]
    sel = [r for r in rows if r[:2] == ('places', tier) and r[5] == 'p50_ms']
    classes = list(dict.fromkeys(r[3] for r in sel))
    first = sel[0][4]
    base = 'sift' if 'sift' in engines else engines[0]   # the engine the ratios are relative to
    p50 = lambda e, c, wl: next((r for r in sel if (r[2], r[3], r[4]) == (e, c, wl)), None)
    show = lambda r, d=2: pm(r, d)
    head = ['engine', 'index (MiB)', 'build (s)'] + ['%s' % c for c in classes]
    body = [[NAME[e]] + build_cols(rows, 'places', tier, e) + [show(p50(e, c, first)) for c in classes] for e in engines]
    out = ['Index size, CREATE INDEX time and p50 latency in ms ± standard error of the `%s` workload per query class (single client):' % first, ''] + md_table(head, body)
    emu = [e for e in engines if get(rows, 'places', tier, e, '-', 'emulation', 'index_mib')]
    if emu:
        metrics = ('index_mib', 'build_s', 'setup_s', 'heap_growth_mib')
        out += ['', 'Extra columns and indexes of the emulated workloads (rum boost; pg_textsearch top-20, boost, filtered):', '']
        out += md_table(['engine', 'index (MiB)', 'build (s)', 'column fill (s)', 'table growth (MiB)'],
                        [[NAME[e]] + [value(rows, 'places', tier, e, '-', 'emulation', m) for m in metrics] for e in emu])
    out += md_merge(rows, 'places', tier, engines)
    for wl in dict.fromkeys(r[4] for r in sel):
        body = []
        for c in classes:
            v = {e: p50(e, c, wl) for e in engines}
            val = lambda e: v[e][6] if v[e] else 'NA'
            body.append([c] + [show(v[e]) + ('' if e == base or val(e) in ('NA', 'error') else ' (%s)' % ratio(val(e), val(base))) for e in engines])
        out += ['', 'Workload `%s`, p50 latency in ms ± standard error (ratio to %s):' % (wl, NAME[base]), ''] + md_table(['class'] + [NAME[e] for e in engines], body)
    out += ['', LEGEND]
    return out


def md_cold(rows, dataset, tier):
    sel = {(r[2], r[4], r[3], r[5]): fmt(float(r[6]), 2) if r[6] not in ('NA', 'error') else r[6] for r in rows if r[:2] == (dataset, tier) and r[5] in ('cold_p50_ms', 'cold_max_ms')}   # (engine, variant, class, metric)
    engines = [e for e in ORDER if any(k[0] == e for k in sel)]
    keys = list(dict.fromkeys((k[1], k[2]) for k in sel))
    cell = lambda e, k: sel[(e,) + k + ('cold_p50_ms',)] + ' / ' + sel[(e,) + k + ('cold_max_ms',)] if (e,) + k + ('cold_p50_ms',) in sel else 'NA'
    places = dataset == 'places'
    body = [([k[0]] if places else []) + [k[1]] + [cell(e, k) for e in engines] for k in keys]
    return (['Cold cache (index, heap and TOAST evicted before every query), p50 / maximum execution time in ms of up to 12 queries per cell (at least 5: a cell that takes seconds stops early, cold.tsv has the count); ' + METHOD, '']
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
        rows += host_rows(dataset, tier, path)
        loads[dataset, tier] += load
        tiers.append((dataset, tier))
    rows += build_rows(builds)
    for (dataset, tier), v in loads.items():
        if v:
            rows.append((dataset, tier, '-', '-', '-', 'load_1m', '%.1f' % st.median(v), '%.1f' % min(v), '%.1f' % max(v), 'load average', *single('%.1f' % st.median(v))))
    os.makedirs(outdir, exist_ok=True)

    def write(path, selected):
        with open(path, 'w') as f:
            f.write(COLUMNS + '\n' + HEADER + '\n')
            for r in selected:
                f.write('\t'.join(('%.*f' % (2 if abs(x) >= 1 else 4, x)).rstrip('0').rstrip('.') if isinstance(x, float) else str(x) for x in r) + '\n')
    write(outdir + '/results.tsv', rows)
    if per:
        os.makedirs(per, exist_ok=True)
        for d in dict.fromkeys(r[0] for r in rows):
            path = '%s/%s.tsv' % (per, d)
            kept = [tuple(r) + ('NA',) * (WIDTH - len(r)) for r in rows_of(path, skip_header=False)[2:] if (r[0], r[1]) not in tiers] if os.path.exists(path) else []
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
        md += [method(session_quota(rows, dataset, tier)),
               'Host load average (1 min) during the cells: %s-%s (median %s).' % (load[7], load[8], load[6]), '']
        md += (md_articles if dataset == 'articles' else md_places)(rows, tier) + ['']
    open(outdir + '/tables.md', 'w').write('\n'.join(md))
    print('\n'.join(md))


if __name__ == '__main__':
    main(sys.argv[1:])
