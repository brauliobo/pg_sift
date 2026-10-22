"""The store benchmark's result files and their Markdown: the tables of README.md and docs/BENCHMARKS.md, table, gap, compare (steps/store.py render)."""
import benchlib
import json
import os
import re
from store_charts import picture
from store_lib import CHUNK, LANES, LOAD_QUERIES, LOAD_QUERY, PG_ENGINES, UPDATE_ENGINES, load_queries


class Results:
    def __init__(self, path):
        d = json.load(open(path))
        self.meta, self.rows = d['meta'], d['rows']
        self.scaling = os.path.exists(os.path.join(os.path.dirname(path), 'store-scaling.json'))   # the sizes other than 1,000,000 orders are measured in their own file
        tm = self.meta['tiers']
        self.all_tiers = sorted(tm, key=lambda t: (tm[t]['rows'], self.cpus(t)))   # a result set per size and thread budget (`1M-c3`; older results: one per size)
        self.tiers = [t for t in self.all_tiers if 'load' in tm[t]['parts']]       # one per size: the set that holds the load, the plans and the updates
        self.sets = {t: [t] for t in self.tiers}   # published: the set of the load (3 CPUs, the budget of the search tables); the other budgets stay in the results file (`all_tiers`)

    def cpus(self, tier):
        old = str(self.meta.get('host', {}).get('cpus', ''))   # results made before the thread budget: the one quota of the host record
        return self.meta['tiers'][tier].get('cpus') or (int(old) if old.isdigit() else 0)

    def get(self, tier, engine, cls='-', variant='-', metric=None):
        for r in self.rows:
            if (r['tier'], r['engine'], r['class'], r['variant'], r['metric']) == (tier, engine, cls, variant, metric):
                return r
        return None

    def num(self, *a, **k):
        r = self.get(*a, **k)
        return r['value'] if r else None

    def engines(self, tier, among):
        return [e for e in among if any(r['tier'] == tier and r['engine'] == e and r['metric'] in ('latency_ms', 'load_s') for r in self.rows)]

    def queries(self, tier):
        out = {}
        for r in self.rows:
            if r['tier'] == tier and r['metric'] == 'latency_ms':
                out[r['class']] = None
        return sorted(out)


def ms(v):
    if v is None:
        return 'n/a'
    if v >= 1000:
        return f'{v:,.0f}'
    return f'{v:.3g}' if v < 100 else f'{v:.0f}'


def spread(r):
    return f'{ms(r["value"])} ({ms(r["min"])}-{ms(r["max"])})' if r else 'n/a'


def md_table(head, body, align=None):
    align = align or ['---'] + ['---:'] * (len(head) - 1)
    return ['| ' + ' | '.join(head) + ' |', '|' + '|'.join(align) + '|'] + ['| ' + ' | '.join(r) + ' |' for r in body]


def seconds(v):
    return 'n/a' if v is None else f'{v:.0f} s' if v >= 10 else f'{v:.1f} s'


def size(v, free=None):
    s = 'n/a' if v is None else f'{v / 1024:.1f} GiB' if v >= 10240 else f'{v:,.0f} MiB'
    return s + (f' ({free:,.0f} free)' if free else '')


def best_bold(values, cells):
    """bold the cells whose value is within 5% of the best (lowest) one"""
    present = [v for v in values if v is not None]
    best = min(present) if present else None
    return [f'**{c}**' if v is not None and v <= best * 1.05 else c for v, c in zip(values, cells)]


def bench_table(R, tier, engines, labels, detail=False, variant='warm'):
    head = ['question' if detail else 'query'] + [labels[e] for e in engines]
    body = []
    for qid in R.queries(tier):
        title = (QUERY_TITLES if detail else QUERY_LABELS)[qid]
        rs = [R.get(tier, e, qid, variant, 'latency_ms') for e in engines]
        vals = [r['value'] if r else None for r in rs]
        cells = [spread(r) if detail else ms(r['value'] if r else None) for r in rs]
        body.append([title] + best_bold(vals, cells))
    return head, body


HEAP = ('pg', 'sift')   # the systems whose table is a heap; the others hold the rows in their own format


def split_parts(e, total, table, free, idx):
    """MiB of a system: (heap and primary key, [the parts of the index or columnar data]); None for the first where there is no such part (the system holds everything itself).
    total, table (pg_table_size), free blocks inside it and the size of each index, as the load and the updates measure them"""
    if table is None:
        return None, [total]
    pk = idx.pop('orders_pkey')
    return (table + pk, list(idx.values())) if e in HEAP else (pk, [table - free, *idx.values()])


def size_split(R, tier, e):
    idx = {r['variant'][6:]: r['value'] for r in R.rows if (r['tier'], r['engine'], r['metric']) == (tier, e, 'size_mib') and r['variant'].startswith('index ')}
    return split_parts(e, R.num(tier, e, metric='size_mib'), R.num(tier, e, variant='table', metric='size_mib'), R.num(tier, e, variant='free', metric='size_mib') or 0, idx)


def upd_split(R, tier, e, v):
    """size_split at one point of the updates (before, after churn, after maintenance)"""
    idx = {r['class']: r['value'] for r in R.rows if (r['tier'], r['engine'], r['variant'], r['metric']) == (tier, e, v, 'index_mib')}
    return split_parts(e, R.num(tier, e, '-', v, 'size_mib'), R.num(tier, e, '-', v, 'table_mib'), R.num(tier, e, '-', v, 'free_mib') or 0, idx)


def parts_cell(parts):
    return f'{sum(parts):.1f} MiB' + (' (' + ' + '.join(f'{p:.1f}' for p in parts) + ')') * (len(parts) > 1)


def index_cell(e, heap, parts, pg_heap):
    """the index or columnar data of a system and, beside it, what it sits next to: the heap with its primary key (the same heap as PostgreSQL's where it is), or the primary key alone"""
    note = ''
    if heap:
        note = (f' (+ heap {heap:.1f}, same as PostgreSQL)' if e != 'pg' and pg_heap and abs(heap - pg_heap) < 0.05 else
                f' (+ heap {heap:.1f})' if e in HEAP else f' (+ primary key {heap:.1f})')
    return f'{sum(parts):.1f} MiB' + note


def size_cells(labels, split, disk, total, parts=None):
    """the three size rows: heap and primary key, index or columnar data, total on disk (the cells of the total, and of the parts when not `parts_cell`, as the caller writes them)"""
    return [[labels[0]] + [f'{h:.1f} MiB' if h else 'n/a' for h, _ in split],
            [labels[1]] + best_bold([sum(p) for _, p in split], parts or [parts_cell(p) for _, p in split]),
            [labels[2]] + best_bold(disk, total)]


def col_cell(split, engines):
    """the `sift_col` cell of the index row: its own size, the heap it shares with PostgreSQL's table noted beside it (the heap of both is the same table)"""
    heap, parts = split[engines.index('sift')]
    pg = split[engines.index('pg')][0]
    return f'{sum(parts):.1f} MiB (+ heap {heap:.1f}, ' + ('same as PostgreSQL' if abs(pg - heap) < 0.05 else f'PostgreSQL {pg:.1f}') + ')'


def size_rows(R, tier, engines):
    """the two size rows: index or columnar data (with the heap or primary key it sits beside) and the total on disk (the free blocks inside the file and what is live)"""
    split = {e: size_split(R, tier, e) for e in engines}
    pg_heap = split['pg'][0] if 'pg' in split else None
    disk = [R.num(tier, e, metric='size_mib') for e in engines]
    free = [R.num(tier, e, variant='free', metric='size_mib') for e in engines]
    total = [size(v) + (f' ({f:,.0f} free, {v - f:,.0f} live)' if f else '') for v, f in zip(disk, free)]
    return [['size: index or columnar data'] + best_bold([sum(split[e][1]) for e in engines], [index_cell(e, *split[e], pg_heap) for e in engines]),
            ['size on disk, total'] + best_bold(disk, total)]


def size_note(R, tier, engines):
    """what the size rows mean (the parts of each system are in the index tables of docs/BENCHMARKS.md); page: where the link to the per-column table starts from"""
    own = [LANES[e][0] for e in ('sift_table', 'es', 'ch', 'citus') if e in engines]
    return ('- **The size rows.** *Index or columnar data* is what a system keeps beside the table: PostgreSQL its secondary indexes (the btrees on the date and on country and status, the GIN on the title), `sift_col` the column index and the BM25 title index; '
            f'the heap in parentheses is the table with its primary key btree, the same heap for PostgreSQL and `sift_col`, which indexes the ordinary table. {join_and(own)} store the data themselves, so there that row is the data, not an index in the PostgreSQL sense '
            '(`sift_table` and Citus add a primary key btree beside it). The total is the file sizes, the one number that compares across systems'
            + '; the free blocks of `sift_table` left inside its file after the load are shown in parentheses' * any(R.num(tier, e, variant='free', metric='size_mib') for e in engines) + '.')


def load_rows(R, tier, engines):
    load = [R.num(tier, e, metric='load_s') for e in engines]
    return [['load time (data, indexes, merge)'] + best_bold(load, [seconds(v) for v in load]), *size_rows(R, tier, engines)]


QUERY_TITLES = {q.id: q.title for q in load_queries()}


QUERY_LABELS = {q.id: q.label for q in load_queries()}


SHOWN = ['pg', 'sift', 'sift_table', 'es', 'ch', 'citus']   # the columns of the README table, in order; a system without rows in the results has no column


def losses(R, tier, engines, labels, variant='warm'):
    """where pg_sift (the faster of its two layouts) is not the fastest system of the question, from the rows"""
    out = []
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    others = [e for e in engines if e not in ('sift', 'sift_table', 'es_cached')]
    for qid in R.queries(tier):
        m = {e: R.num(tier, e, qid, variant, 'latency_ms') for e in mine}
        m = {e: v for e, v in m.items() if v is not None}
        o = {e: R.num(tier, e, qid, variant, 'latency_ms') for e in others}
        o = {e: v for e, v in o.items() if v is not None}
        if not m or not o:
            continue
        me, ov = min(m, key=m.get), min(o, key=o.get)
        if o[ov] < m[me] * 0.95:
            out.append((qid, QUERY_TITLES[qid], labels[me], m[me], labels[ov], o[ov]))
    return out


def ratio(a, b):
    r = a / b
    return f'{r:.0f}x' if r >= 10 else f'{r:.1f}x'


def load_notes(R, tier):
    """the loads that failed as one COPY (a result) and were done in transactions of CHUNK rows"""
    out = []
    for e in PG_ENGINES:
        r = R.get(tier, e, '-', 'rows', 'load_error')
        if r:
            msg = ' '.join(re.findall(r'(?:ERROR|DETAIL):\s+([^\n]*?)(?= DETAIL:| HINT:| CONTEXT:|$)', r['value'].replace('\n', ' ')))
            out.append(f'Loading the {R.meta["tiers"][tier]["rows"]:,} rows into {LANES[e][0]} with one `COPY` failed (`{msg[:220]}`); they were loaded in transactions of '
                       f'{int(R.num(tier, e, "-", "rows", "chunk_rows") or CHUNK):,} rows with a `VACUUM` after each instead, and only that load is timed.')
    return out + error_notes(R, tier)


def error_notes(R, tier):
    """the questions a system answered with an error (their cells are n/a): the message, once per system"""
    out = []
    for e in LANES:
        bad = [(q, r['value'][7:]) for q in R.queries(tier) if (r := R.get(tier, e, q, '-', 'result')) and r['value'].startswith('error: ')]
        if bad:
            ids = ', '.join(str(int(q)) for q, _ in bad)
            out.append(f'{LANES[e][0]} failed on question{"s" * (len(bad) > 1)} {ids} at {R.meta["tiers"][tier]["rows"]:,} orders (`{bad[0][1][:160]}`); the cells are n/a.')
    return out


def cpu_name(n):
    return f'{n} CPU' + 's' * (n != 1)


def join_and(names):
    return ', '.join(names[:-1]) + (' and ' if len(names) > 1 else '') + names[-1]


def thread_line(R, tier):
    """what the servers of a result set reported for its thread budget (steps/store.py effective)"""
    th = R.meta['tiers'][tier].get('threads', {})
    pg = th.get('pg') or th.get('sift')
    who = [name for name, k in (('PostgreSQL', 'pg'), ('pg_sift', 'sift'), ('Citus', 'citus')) if k in th]
    out = [f'{join_and(who)}: a leader and {pg["max_parallel_workers_per_gather"]} parallel worker{"s" * (pg["max_parallel_workers_per_gather"] != "1")}'] * bool(pg)
    out += [f'Elasticsearch: `node.processors` {th["es"]["allocated_processors"]}'] * ('es' in th)
    out += [f'ClickHouse: `max_threads` {th["ch"]["max_threads"]}'] * ('ch' in th)
    return '; '.join(out)


def set_heading(R, tier):
    """the line above a table of one thread budget"""
    return f'{cpu_name(R.cpus(tier))} per container' + (f' ({thread_line(R, tier)})' if R.meta['tiers'][tier].get('threads') else '')


def dagger(R, tier, labels):
    """the labels of the systems whose cells in this result set were read from the cache get a dagger"""
    cached = R.meta['tiers'][tier].get('cached', {})
    return {e: lb + (' †' if benchlib.group_of(e) in cached else '') for e, lb in labels.items()}


def cache_note(R, tier):
    """one sentence for the systems whose cells were read from the cache: when and under which key each was measured, on which host, and when the rest was"""
    cached = R.meta['tiers'][tier].get('cached', {})
    if not cached:
        return []
    host_ = R.meta.get('host', {})
    when = ', '.join(f'{LANES[g][0]} {v["date"][:10]} (key `{v["key"][:8]}`)' for g, v in cached.items())
    done = R.meta['tiers'][tier]['parts'].get('warm', {}).get('date', '?')[:10]
    return [f'† Read from the cache of earlier passes, not measured in this one: {when}. Same host ({host_.get("cpu", "?")}, kernel {host_.get("kernel", "?")}, fingerprint `{host_.get("id", "?")}`), same data, '
            f'images, settings, statements and runs (the key); the other columns were measured {done}.']


def citus_options(R, tier):
    r = R.get(tier, 'citus', '-', 'orders', 'columnar_options')
    return r['value'] if r else '?'


def citus_notes(R, tier):
    """what the Citus column is, from the rows of the tier: columnar only, one node, the settings it ran with"""
    if not R.engines(tier, ['citus']):
        return []
    s = R.meta['tiers'][tier]['servers']
    return [f'Citus {s["citus_columnar"]} (PostgreSQL {s["citus"].split()[0]}) is measured as one node with the table in its `columnar` access method and its default settings ({citus_options(R, tier)}), a primary key and no other index '
            '(columnar tables take btree and hash indexes only, so the three searches scan the title column). Columnar tables are append-only, so Citus is not in the update table.']


def ahead(R, tier, engines, variant):
    """(questions where the faster layout of pg_sift is the fastest or within 5% of it, questions)"""
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    others = [e for e in engines if e not in ('sift', 'sift_table', 'es_cached')]
    qs = [q for q in R.queries(tier) if any(R.num(tier, e, q, variant, 'latency_ms') for e in mine)]
    n = sum(1 for q in qs if min(R.num(tier, e, q, variant, 'latency_ms') or 1e99 for e in mine) <= min((R.num(tier, e, q, variant, 'latency_ms') or 1e99) for e in others) * 1.05)
    return n, len(qs)


def takeaway(R, tier, engines):
    """one line: where pg_sift stands, warm and cold, and against PostgreSQL"""
    w, c = ahead(R, tier, engines, 'warm'), ahead(R, tier, engines, 'cold')
    pg = [R.num(tier, 'pg', q, 'warm', 'latency_ms') / min(R.num(tier, e, q, 'warm', 'latency_ms') for e in ('sift', 'sift_table') if R.num(tier, e, q, 'warm', 'latency_ms')) for q in R.queries(tier) if R.num(tier, 'pg', q, 'warm', 'latency_ms')]
    return (f'**Takeaway:** the faster layout of pg_sift is the fastest system, or within 5% of it, on {w[0]} of {w[1]} questions warm' + (f' and {c[0]} of {c[1]} cold' if c[1] else '')
            + f'; against PostgreSQL it is {ratio(min(pg), 1)} to {ratio(max(pg), 1)} faster.')


def caveat_lines(R, tier, engines, labels, size_rows_shown=True):
    """the caveats of the store results, short bullets: where pg_sift is behind, what the settings change, what the numbers are not"""
    out = []
    lo, cold = losses(R, tier, engines, labels), losses(R, tier, engines, labels, 'cold')
    if lo:
        out.append('**Where pg_sift is not the fastest, warm:** ' + '; '.join(f'{QUERY_LABELS[qid]}: {lb2} {ratio(v1, v2)} faster ({ms(v2)} ms against {ms(v1)} ms)' for qid, _, lb, v1, lb2, v2 in lo) + '.')
    if cold:
        out.append('**Cold (the first run after the server\'s memory was emptied), not the fastest on** ' + ', '.join(f'Q{qid} ({lb2} {ratio(v1, v2)} faster)' for qid, _, lb, v1, lb2, v2 in cold) + '.')
    cached = [(q, R.num(tier, 'es_cached', q, 'warm', 'latency_ms'), min(R.num(tier, e, q, 'warm', 'latency_ms') for e in ('sift', 'sift_table') if R.num(tier, e, q, 'warm', 'latency_ms'))) for q in R.queries(tier) if R.num(tier, 'es_cached', q, 'warm', 'latency_ms')]
    if cached:
        behind = [f'Q{q} ({ratio(m, c)})' for q, c, m in cached if c < m * 0.95]
        out.append('Elasticsearch runs with its shard request cache off (a repeated aggregation would otherwise be answered from memory); with it on' + (f' it is faster than pg_sift on {", ".join(behind)}.' if behind else ' it is not faster than pg_sift on any question.'))
    rd = R.get(tier, 'es', '06', '-', 'rel_diff')
    out.append('Every result is compared with PostgreSQL\'s before it is timed and the run stops on a difference; Elasticsearch counts distinct customers approximately (HyperLogLog++' + (f', off by at most {rd["value"]:.2%} here' if rd else '') + '), every other result matched exactly or within rounding.')
    if size_rows_shown:
        out.append(size_note(R, tier, engines)[len('- '):])
    plain = [q for q in R.queries(tier) if 'Seq Scan' in (R.get(tier, 'sift_table', q, '-', 'plan') or {}).get('value', '') and R.num(tier, 'sift_table', q, 'warm', 'latency_ms') and R.num(tier, 'pg', q, 'warm', 'latency_ms')]
    if plain and 'sift_table' in engines:
        ratios = [R.num(tier, 'sift_table', q, 'warm', 'latency_ms') / R.num(tier, 'pg', q, 'warm', 'latency_ms') for q in plain]
        out.append(f'`sift_table` ran {len(plain)} of the {len(R.queries(tier))} questions with a plain `Seq Scan` plan (no aggregate pushdown: [docs/USAGE.md](docs/USAGE.md#limits) lists what `SiftAgg` does not read) and took '
                   f'{" to ".join(dict.fromkeys([ratio(min(ratios), 1), ratio(max(ratios), 1)]))} the time of PostgreSQL\'s heap on those.')
    out += [n for n in cache_note(R, tier) + citus_notes(R, tier) + load_notes(R, tier)]
    noisy = R.meta['tiers'][tier]['parts'].get('warm', {}).get('noisy')
    if noisy:
        out.append('Cells the harness flagged NOISY after measuring them again (other work on the shared host): ' + '; '.join(noisy) + '.')
    return out


def readme_block(R):
    tier = R.tiers[-1]
    t = R.meta['tiers'][tier]
    s = t['servers']
    labels = {e: LANES[e][0] for e in LANES}
    engines = R.engines(tier, SHOWN)
    host = R.meta.get('host', {})
    out = [f'{t["rows"]:,} orders (country, category, status, amount, date, a product title) and ten questions, the same seeded rows in every system, each alone in a docker container with {cpu_name(R.cpus(tier))} (a quota, no pinning) and {host.get("memory", "?")} of memory '
           f'and the same thread budget ([method](docs/BENCHMARKS.md#configuration)), one client over a local connection. Median of {t["runs"]} warm runs in milliseconds, lower is better, the fastest of a row and anything within 5% of it in bold. '
           f'Measured {t["parts"]["warm"]["date"][:10]} on {host.get("cpu", "?")} ({host.get("threads", "?")} threads), PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]}' + (f', Citus {s["citus_columnar"]} columnar' if 'citus' in engines else '') + f', Elasticsearch {s["es"]}, ClickHouse {s["ch"]}.', '',
           picture('store-latency', 'Median time per question and system on a logarithmic axis, 1,000,000 orders'), '']
    head, body = bench_table(R, tier, engines, dagger(R, tier, labels))
    out += md_table(head, body + load_rows(R, tier, engines)) + ['', takeaway(R, tier, engines), '',
                                                                  picture('store-speedup', 'How many times faster than PostgreSQL each system is on each question, warm and cold'), '', '**Caveats**', '']
    out += ['- ' + c for c in caveat_lines(R, tier, engines, labels)]
    out += ['', 'Raw results: [bench/results/store.json](bench/results/store.json); every table, the cold runs and the configuration: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#aggregates-an-online-store); reproduce with `bench/bench store all`.']
    return out


def host_noise(part):
    """how busy the host was at the start of a part: its pressure (scripts/quiet.sh), the load average for results made before; the NOISY batches"""
    out = part['pressure'] if part.get('pressure') else 'load average ' + ' '.join(part['loadavg'])
    return out + (f' (NOISY: {", ".join(part["noisy"])})' if part.get('noisy') else '')


def thread_table(R, tiers):
    """the settings each server reported for each result set (steps/store.py effective), one row per set"""
    pg_names = 'PostgreSQL, pg_sift' + ', Citus' * any('citus' in R.meta['tiers'][x].get('threads', {}) for x in tiers)
    head = ['result set', 'container `--cpus`', f'{pg_names}: workers per gather (maintenance)', 'ClickHouse: `max_threads` (background pool)', 'Elasticsearch: `node.processors`, JVM processors (search / write threads)']
    body = []
    for x in tiers:
        th = R.meta['tiers'][x].get('threads')
        if not th:
            continue
        pg, ch, es = th.get('pg') or th.get('sift'), th.get('ch'), th.get('es')
        quota = next(iter(th.values()))['quota_cpus']
        body.append([f'{R.meta["tiers"][x]["rows"]:,} orders, {cpu_name(R.cpus(x))}', f'{quota:g}',
                     f'{pg["max_parallel_workers_per_gather"]} ({pg["max_parallel_maintenance_workers"]})' if pg else 'n/a',
                     f'{ch["max_threads"]} ({ch["background_pool_size"]})' if ch else 'n/a',
                     f'{es["allocated_processors"]}, {es["jvm_processor_count"]} ({es["thread_pools"]["search"]} / {es["thread_pools"]["write"]})' if es else 'n/a'])
    return md_table(head, body, ['---'] + ['---:'] * 4) if body else []


def citus_config(R, tier):
    """the Configuration bullet of the Citus column (nothing when the results have none): the image, the version, the table's settings as the server reports them"""
    if not R.engines(tier, ['citus']):
        return []
    s = R.meta['tiers'][tier]['servers']
    w = (R.meta['tiers'][tier].get('threads') or {}).get('citus', {}).get('count_workers_planned')
    return [f'- Citus {s["citus_columnar"]} (image `{s["citus_image"]}`, PostgreSQL {s["citus"].split()[0]}): one node, no workers added, `citus` preloaded as the image ships it and only the `columnar` access method of `citus_columnar` used; '
            f'the table `USING columnar` with the default settings ({citus_options(R, tier)}), a primary key and no other index (columnar tables take btree and hash indexes only: no GIN for the search, no `UPDATE` or `DELETE`, so it is not in the update tables). '
            f'The same server flags as the other PostgreSQL containers: {s["citus_settings"]}; `max_parallel_workers_per_gather` N - 1' + (f', and the planner planned {w} workers for `count(*)` on the table' if w is not None else '') + '.']


def benchmarks_block(R):
    labels = {e: LANES[e][0] for e in LANES}
    host = R.meta.get('host', {})
    tier = R.tiers[-1]
    t = R.meta['tiers'][tier]
    s = t['servers']
    engines = R.engines(tier, ['pg', 'sift', 'sift_table', 'es', 'ch', 'citus'])
    out = [f'{t["rows"]:,} orders and ten questions, {cpu_name(R.cpus(tier))} per container ({thread_line(R, tier)}). Median of {t["runs"]} warm runs in milliseconds with the fastest and slowest run in parentheses; the fastest of a row and anything within 5% of it in bold. '
           f'Measured {t["parts"]["warm"]["date"][:10]} on {host.get("cpu", "?")}, PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]}, Elasticsearch {s["es"]}, ClickHouse {s["ch"]}'
           + (f', Citus {s["citus_columnar"]}' if 'citus' in engines else '') + f'; seeded data sha256 `{t["data_sha256"][:16]}`; {host_noise(t["parts"]["warm"])} at the start of the runs (the host is shared).', '']
    head, body = bench_table(R, tier, engines, dagger(R, tier, labels), detail=True)
    out += md_table(head, body + load_rows(R, tier, engines)) + ['', takeaway(R, tier, engines), '',
                                                                   picture('store-speedup', 'How many times faster than PostgreSQL each system is on each question, warm and cold', 'img/'), '', '**Caveats**', '']
    out += ['- ' + c for c in caveat_lines(R, tier, engines, labels)]
    out += ['', 'Raw results: [results/store.json](results/store.json); the charts and the scale benchmarks: [perf/AGGREGATES.md](perf/AGGREGATES.md).', '']
    last = R.tiers[-1]
    t = R.meta['tiers'][last]
    s = t['servers']
    quota = R.cpus(last)
    th =t.get('threads')
    pg = (th.get('pg') or th.get('sift')) if th else None
    pg_text = (f'`max_parallel_workers_per_gather` N - 1 (a leader and N - 1 workers are N processes; `max_parallel_maintenance_workers` the same), `max_worker_processes` {pg["max_worker_processes"]} and `max_parallel_workers` '
               f'{pg["max_parallel_workers"]} (far above the budget), `parallel_setup_cost` {pg["parallel_setup_cost"]}, `parallel_tuple_cost` {pg["parallel_tuple_cost"]}, `min_parallel_table_scan_size` {pg["min_parallel_table_scan_size"]}, '
               'the same flags for both containers') if pg else '`max_parallel_workers_per_gather` and the other settings are the same for both containers'
    out += ['### Configuration', '',
            f'- Host: {host.get("cpu", "?")}, {host.get("threads", "?")} hardware threads, {host.get("ram", "?")} of memory, {host.get("kernel", "?")}; docker {host.get("docker", "?")}. '
            f'Each server runs alone in a container with `--cpus N --memory {host.get("memory", "?")}` (N = {quota}: a CPU quota, the CPUs are not pinned), no swap; the servers not under test sit idle.',
            ]
    if th:
        out += ['- Thread budget: every engine is told the same N as the quota, because they start different numbers of threads on their own. What each server reported after it started (checked against N before anything is timed):', '']
        out += thread_table(R, [x for tier in R.tiers for x in R.sets[tier]]) + ['']
    out += [f'- PostgreSQL {s["pg"]} (the same image for both layouts, pg_sift {s["sift_sift"]} (the build under test)' + (f', linked with `{s["sift_libs"]}`, `compression` unset on the index = `auto`, `sift.table_compression` {s["sift_table_compression"]}' if 'sift_libs' in s else '') + f'): {s["pg_settings"]}; {pg_text}.',
            *citus_config(R, last),
            f'- Elasticsearch {s["es"]} (Lucene {s["es_lucene"]}): one node, {R.cpus(last)} shards (one per CPU of the budget of the load: a shard is searched by one thread, so with one shard a search uses one core whatever `node.processors` says; the shard count is a choice of this harness), no replica, JVM heap `{s["es_heap"]}`, {"`node.processors` and `-XX:ActiveProcessorCount` set to N, " if th else ""}security and machine learning off; `scaled_float` (factor 100) for the amount, `keyword` for country, category and status, `date` for the time, `text` for the title; '
            'each shard merged to one segment after the load; aggregations of the search API with `size: 0`; the shard request cache off (`request_cache=false`) except in the table that says otherwise; distinct counts are `cardinality` (HyperLogLog++, `precision_threshold` 40000), the only approximate result.',
            f'- ClickHouse {s["ch"]}: `MergeTree`, `PARTITION BY` month, `ORDER BY (country, created_at, id)` (`status` and `amount` stay out of the key so that they can be updated), `LowCardinality` strings, `Decimal(10, 2)` for the amount, a text index on the title, a Bloom filter on the id, the settings lightweight updates need; {"`max_threads`, `max_insert_threads`, `max_final_threads` and `background_pool_size` set to N" if th else f"`max_threads` {s['ch_threads']} (what the container limit gives it)"}; optimized to one part per partition after the load.',
            '- pg_sift: `sift_col` over the columns the reports read plus the month of the order, a `sift` BM25 index on the title; `sift_table` is the table itself in column format with a `sift` index on the title and the primary key. PostgreSQL: primary key, btree on the date and on country and status, GIN on `to_tsvector(\'simple\', title)`.',
            f'- Method: every engine has {cpu_name(quota)} for its server, the budget of the search tables; one client, one request at a time, wall time from this process to the end of the response over a persistent local connection; the engines are timed in turn within each run so a drift of the host load hits all of them; one untimed run per engine first (its result is checked against PostgreSQL\'s), then the timed runs. The numbers of one table are comparable with each other, not with another host.']
    return out


UPD_LABELS = {'pg': 'PostgreSQL', 'sift': 'pg_sift `sift_col`', 'sift_table': 'pg_sift `sift_table`', 'es': 'Elasticsearch', 'ch': 'ClickHouse, lightweight UPDATE',
              'ch_mut': 'ClickHouse, ALTER UPDATE', 'ch_rmt': 'ClickHouse, ReplacingMergeTree + FINAL'}


UPD_PATH = {'pg': '`UPDATE` / `DELETE` ... `WHERE id IN (...)`, 1,000 ids per statement', 'sift': 'the same statements; the heap is updated, the `sift_col` index follows',
            'sift_table': 'the same statements on the column-format table', 'es': '`_bulk` `update` (script) and `delete` actions, 1,000 per request, default refresh',
            'ch': '`UPDATE` / `DELETE FROM` ... `WHERE id IN (...)` (patch parts), 1,000 ids per statement',
            'ch_mut': 'one `ALTER TABLE ... UPDATE` / `DELETE` with the predicate of the rows, `mutations_sync = 1`',
            'ch_rmt': '`INSERT ... SELECT` of a new version of the rows, 1,000 ids per statement; reads use `FINAL`'}


UPD_DOCS = [
    ('PostgreSQL', [('Routine vacuuming', 'https://www.postgresql.org/docs/18/routine-vacuuming.html'), ('UPDATE', 'https://www.postgresql.org/docs/18/sql-update.html')]),
    ('pg_sift', [('Column indexes, aggregates and sift_table', 'USAGE.md#column-indexes-aggregates-and-sift_table')]),
    ('Elasticsearch', [('Update API', 'https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-update'),
                       ('Bulk API', 'https://www.elastic.co/guide/en/elasticsearch/reference/8.19/docs-bulk.html'),
                       ('The refresh parameter', 'https://www.elastic.co/docs/reference/elasticsearch/rest-apis/refresh-parameter'),
                       ('Force a merge', 'https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-indices-forcemerge')]),
    ('ClickHouse', [('Updates in ClickHouse (which method for which job)', 'https://clickhouse.com/docs/concepts/features/operations/update/overview'),
                    ('UPDATE (lightweight updates)', 'https://clickhouse.com/docs/reference/statements/update'),
                    ('ALTER TABLE ... UPDATE (mutations)', 'https://clickhouse.com/docs/reference/statements/alter/update'),
                    ('ReplacingMergeTree', 'https://clickhouse.com/docs/reference/engines/table-engines/mergetree-family/replacingmergetree')]),
]


def upd_tier(R):
    tiers = [t for t in R.tiers if any(r['tier'] == t and r['metric'] in ('phase_rows_s', 'ack_ms') for r in R.rows)]
    return tiers[-1] if tiers else None


def upd_engines(R, tier):
    return [e for e in UPDATE_ENGINES if any(r['tier'] == tier and r['engine'] == e and r['metric'] in ('phase_rows_s', 'ack_ms', 'error') for r in R.rows)]


def rate(v):
    return 'n/a' if v is None else f'{v:,.0f}'


def best_high(values, cells):
    present = [v for v in values if v is not None]
    best = max(present) if present else None
    return [f'**{c}**' if v is not None and v >= best / 1.05 else c for v, c in zip(values, cells)]


def upd_rows(R, tier, engines, detail):
    """(label, cells) rows of the update table; the fastest cell of a row in bold"""
    g = lambda e, v, m, c='-': R.num(tier, e, c, v, m)
    out = []
    for v, label in (('update 1%', 'update 1% of the rows (rows/s, higher is better)'), ('update 10%', 'update 10% of the rows (rows/s)'), ('delete 5%', 'delete 5% of the rows (rows/s)')):
        vals = [g(e, v, 'phase_rows_s') for e in engines]
        out.append([label] + best_high(vals, [rate(x) for x in vals]))
    modes = {'pg': 'commit', 'sift': 'commit', 'sift_table': 'commit', 'es': 'default', 'ch': 'lightweight', 'ch_mut': 'mutation', 'ch_rmt': 'insert'}
    ack = [g(e, modes[e], 'ack_ms') for e in engines]
    vis = [g(e, modes[e], 'visible_ms') for e in engines]
    out.append(['single-row change: acknowledged after (ms)'] + best_bold(ack, [ms(x) for x in ack]))
    out.append(['single-row change: seen by the next query (ms after the acknowledgement)'] + best_bold(vis, [ms(x) for x in vis]))
    if 'es' in engines:
        w = [f'{ms(g(e, "wait_for", "ack_ms"))} (then {ms(g(e, "wait_for", "visible_ms"))})' if e == 'es' else '' for e in engines]
        out.append(['same with `refresh=wait_for`: acknowledged after (ms)'] + [x or '' for x in w])
    for r in (0, 1000, 10000):
        v = f'load {r}/s'
        p50 = [g(e, v, 'latency_ms', LOAD_QUERY) for e in engines]
        p95 = [g(e, v, 'latency_p95_ms', LOAD_QUERY) for e in engines]
        cells = [(f'{ms(a)}' + (f' / {ms(b)}' if detail else '')) if a is not None else ('error' if R.get(tier, e, LOAD_QUERY, v, 'read_error') else 'n/a') for e, a, b in zip(engines, p50, p95)]
        out.append([f'revenue per month while {r:,} rows/s are updated: p50' + (' / p95' if detail else '') + ' (ms)'] + best_bold(p50, cells))
        if r:
            ach = [g(e, v, 'updates_rows_s') for e in engines]
            out.append([f'rows/s the update load actually reached (asked {r:,})'] + [rate(x) if x is not None else 'n/a' for x in ach])
    for v, label in (('before', 'size before'), ('after churn', 'size after the changes'), ('after maintenance', 'size after VACUUM / merge / OPTIMIZE')):
        vals = [g(e, v, 'size_mib') for e in engines]
        split = [upd_split(R, tier, e, v) for e in engines]
        parts = [col_cell(split, engines) if e == 'sift' and s[0] else parts_cell(s[1]) for e, s in zip(engines, split)]
        out += size_cells([f'{label}: heap and primary key', f'{label}: index or columnar data', f'{label}, total'], split, vals,
                          [size(x, g(e, v, 'free_mib')) for e, x in zip(engines, vals)], parts)
    return out


def upd_read_rows(R, tier, engines):
    out = []
    for qid in LOAD_QUERIES:
        for v in ('baseline', 'after churn', 'after maintenance'):
            vals = [R.num(tier, e, qid, v, 'latency_ms') for e in engines]
            out.append([f'{QUERY_TITLES[qid]}, {v}'] + best_bold(vals, [ms(x) for x in vals]))
    vals = [R.num(tier, e, '-', 'maintenance', 'maintain_s') for e in engines]
    out.append(['time of the maintenance step'] + [seconds(x) for x in vals])
    return out


def upd_losses(R, tier, engines):
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    others = [e for e in engines if e not in ('sift', 'sift_table')]
    out = []
    for v, label in (('update 1%', 'updating 1% of the rows'), ('update 10%', 'updating 10% of the rows'), ('delete 5%', 'deleting 5% of the rows')):
        m = {e: R.num(tier, e, '-', v, 'phase_rows_s') for e in mine}
        o = {e: R.num(tier, e, '-', v, 'phase_rows_s') for e in others}
        m, o = {e: x for e, x in m.items() if x}, {e: x for e, x in o.items() if x}
        if m and o and max(o.values()) > max(m.values()) * 1.05:
            a, b = max(m, key=m.get), max(o, key=o.get)
            out.append(f'{label}: {UPD_LABELS[b]} {ratio(o[b], m[a])} faster than {UPD_LABELS[a]} ({rate(o[b])} against {rate(m[a])} rows/s)')
    for e in mine:
        for r in (1000, 10000):
            v = f'load {r}/s'
            a, b = R.num(tier, e, LOAD_QUERY, v, 'latency_ms'), R.num(tier, e, LOAD_QUERY, 'load 0/s', 'latency_ms')
            ach = R.num(tier, e, '-', v, 'updates_rows_s')
            if R.get(tier, e, LOAD_QUERY, v, 'read_error'):
                out.append(f'{UPD_LABELS[e]} under {r:,} rows/s of updates: the query failed ({R.get(tier, e, LOAD_QUERY, v, "read_error")["value"][:80]})')
            elif a and b and a > b * 1.5:
                out.append(f'{UPD_LABELS[e]} under {r:,} rows/s of updates: the query is {ratio(a, b)} slower than with no updates ({ms(b)} to {ms(a)} ms)')
            if ach and ach < r * 0.9:
                out.append(f'{UPD_LABELS[e]} reached {rate(ach)} of the {r:,} rows/s asked for')
    return out


def upd_sentences(R, tier, engines):
    """the claims of the README, each computed from the rows of the tier"""
    g = lambda e, v, m, c='-': R.num(tier, e, c, v, m)
    out = []
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    ranked = sorted(((g(e, 'update 10%', 'phase_rows_s'), e) for e in engines if g(e, 'update 10%', 'phase_rows_s')), reverse=True)
    if ranked:
        out.append('Updating 10% of the rows, in rows per second: ' + ', '.join(f'{UPD_LABELS[e]} {rate(v)}' for v, e in ranked) + '.')
    vis = {e: g(e, {'es': 'default'}.get(e, {'pg': 'commit', 'sift': 'commit', 'sift_table': 'commit', 'ch': 'lightweight', 'ch_mut': 'mutation', 'ch_rmt': 'insert'}.get(e)), 'visible_ms') for e in engines}
    es_v, pg_v, mut_v = vis.get('es'), vis.get('pg'), vis.get('ch_mut')
    parts = []
    if es_v is not None:
        parts.append(f'Elasticsearch showed a change to a query {ms(es_v)} ms after the acknowledgement with the default refresh')
    if mut_v is not None:
        parts.append(f'a ClickHouse `ALTER UPDATE` {ms(mut_v)} ms after it returned')
    if pg_v is not None:
        parts.append(f'PostgreSQL and pg_sift {ms(pg_v)}-{ms(max(vis[e] for e in mine if vis.get(e) is not None))} ms' if mine else f'PostgreSQL {ms(pg_v)} ms')
    if parts:
        out.append('Read-after-write (median of the single-row changes): ' + '; '.join(parts) + '.')
    sz = lambda e, v: g(e, v, 'size_mib')
    grow = [(e, sz(e, 'after churn') / sz(e, 'before')) for e in engines if sz(e, 'before') and sz(e, 'after churn')]
    if grow:
        out.append('Size after the changes against before: ' + ', '.join(f'{UPD_LABELS[e]} {x:.2f}x' for e, x in grow) + ' (after the maintenance step: ' + ', '.join(
            f'{UPD_LABELS[e]} {sz(e, "after maintenance") / sz(e, "before"):.2f}x' for e, _ in grow if sz(e, 'after maintenance')) + ').')
    return out


README_UPDATE_ROWS = ('update 10%', 'delete 5%', 'single-row change: seen by', 'revenue per month while 0 rows/s', 'revenue per month while 10,000 rows/s')   # the rows of the README table; the others are in docs/BENCHMARKS.md


def growth_row(R, tier, engines, v, label):
    """the size after a step over the size before: a ratio, so the sizes themselves (the total on disk of each point) stay in docs/BENCHMARKS.md"""
    vals = [R.num(tier, e, '-', v, 'size_mib') / R.num(tier, e, '-', 'before', 'size_mib') if R.num(tier, e, '-', v, 'size_mib') and R.num(tier, e, '-', 'before', 'size_mib') else None for e in engines]
    return [label] + best_bold(vals, [f'{x:.2f}x' if x else 'n/a' for x in vals])


def updates_readme(R):
    tier = upd_tier(R)
    if not tier:
        return ['No update numbers yet: `bench/bench store all` measures them.']
    engines = upd_engines(R, tier)
    t = R.meta['tiers'][tier]
    rows = [r for r in upd_rows(R, tier, engines, False) if r[0].startswith(README_UPDATE_ROWS)]
    rows += [growth_row(R, tier, engines, 'after churn', 'size on disk after the changes, against before'), growth_row(R, tier, engines, 'after maintenance', 'size after VACUUM / merge / OPTIMIZE, against before')]
    out = [f'On the {t["rows"]:,}-order table, after the read tests, each system changes its rows the way its documentation recommends for the job (below); the state after each step is compared with PostgreSQL\'s. '
           f'Query: revenue per month, timed while a second connection updates rows at the rate shown. Medians; the best of a row in bold.', '']
    out += md_table(['{:,} orders'.format(t['rows'])] + [dagger(R, tier, UPD_LABELS)[e] for e in engines], rows) + ['']
    sentences = upd_sentences(R, tier, engines)
    out += ['**Takeaway:** ' + sentences[0], '', picture('store-updates', 'Size after the changes and after the maintenance step, against before, per system'), '', '**Caveats**', '']
    out += ['- ' + x for x in sentences[1:2] + cache_note(R, tier)]
    loss = upd_losses(R, tier, engines)
    if loss:
        out += ['- **Where pg_sift loses:** ' + '; '.join(loss) + '.']
    out += ['', 'Raw results: [bench/results/store.json](bench/results/store.json); every row, the update path of each system and the official pages: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#updates-and-deletes).']
    return out


def updates_benchmarks(R):
    tier = upd_tier(R)
    if not tier:
        return []
    engines = upd_engines(R, tier)
    t = R.meta['tiers'][tier]
    part = t['parts']['updates']
    host = R.meta.get('host', {})
    out = [f'{t["rows"]:,} orders, after the read tests, {part["date"][:10]} on {host.get("cpu", "?")}, {cpu_name(R.cpus(tier))} per container, {host_noise(part)} at the start. Each system changes rows through the path its documentation recommends; '
           'after each step the state (rows and sum of the amount per status) is compared with PostgreSQL\'s and the run stops on a difference. Medians; the best of a row in bold. '
           f'Steps: update 1% (`id % 100 = 0`), update 10% (`id % 10 = 1`), delete 5% (`id % 20 = 3`), {t.get("probes", "?")} single-row changes timed until a query sees them, the revenue-per-month query for {t.get("load_seconds", "?")} s '
           'while a second connection updates 0, 1,000 and 10,000 rows per second, then one maintenance step per system.', '']
    out += md_table(['{:,} orders'.format(t['rows'])] + [dagger(R, tier, UPD_LABELS)[e] for e in engines], upd_rows(R, tier, engines, True)) + ['']
    sentences = upd_sentences(R, tier, engines)
    out += ['**Takeaway:** ' + sentences[0], '', picture('store-updates', 'Size after the changes and after the maintenance step, against before, per system', 'img/'), '', '**Caveats**', '']
    loss = upd_losses(R, tier, engines)
    if loss:
        out += ['- **Where pg_sift loses:** ' + '; '.join(loss) + '.']
    if part.get('sizes'):   # a later run of the same steps that also read the table and each index
        out += [f'- The size rows (split as in the load table; the table and each index are read in that run only) are from a second run of the same steps, {part["sizes"]["date"]}, {host_noise(part["sizes"])} at the start; every other row is from the run above.']
    out += ['- **Paths:** ' + '; '.join(f'{UPD_LABELS[e]}: {UPD_PATH[e]}' for e in engines) + '.']
    errs = [(e, R.get(tier, e, '-', 'run', 'error')) for e in engines]
    errs = [(e, r['value']) for e, r in errs if r]
    errs += [(e, R.get(tier, e, LOAD_QUERY, f'load {r}/s', 'read_error')['value']) for e in engines for r in (1000, 10000) if R.get(tier, e, LOAD_QUERY, f'load {r}/s', 'read_error')]
    errs += [(e, R.get(tier, e, '-', f'load {r}/s', 'update_error')['value']) for e in engines for r in (1000, 10000) if R.get(tier, e, '-', f'load {r}/s', 'update_error')]
    out += [f'- Error the system raised during the run, kept as a result: {UPD_LABELS[e]}: {m}' for e, m in errs]
    out += ['- ' + c for c in cache_note(R, tier)]
    out += ['- The rows say what those paths did on this table, not what other sizes, hardware or concurrency would do: ClickHouse recommends lightweight `UPDATE` for frequent small changes and mutations for rare large ones, `ReplacingMergeTree` needs `FINAL` at read time; Elasticsearch reindexes the whole document and shows it at the next refresh.']
    out += ['- Official pages: ' + '; '.join(f'{name}: ' + ', '.join(f'[{label}]({url})' for label, url in links) for name, links in UPD_DOCS) + '.']
    out += ['', 'Raw results: [results/store.json](results/store.json); the first-run and latency spread of every step: `bench/bench store table`.']
    return out


CZ_LANES = ['sift_none', 'sift_enc', 'sift_auto', 'sift', 'sift_lz4', 'sift_zstd']


CZ_NAMES = {'sift_none': '`none`', 'sift_enc': 'encodings only', 'sift_auto': '`auto` (explicit)', 'sift': 'unset (the default)', 'sift_lz4': '`lz4`', 'sift_zstd': '`zstd:3`'}


def compression_block(R):
    tier = R.tiers[-1]
    t = R.meta['tiers'][tier]
    n = t['rows']
    s = t['servers']
    host = R.meta.get('host', {})
    lanes = [e for e in CZ_LANES if R.engines(tier, [e])]
    sz = lambda e, v='index orders_col': R.num(tier, e, variant=v, metric='size_mib')
    leaves = {e: {l['leaf']: l for l in json.loads(R.get(tier, e, '-', 'orders_col', 'col_leaves')['value'])} for e in lanes}
    out = [f'The `sift_col` index of the same {n:,} orders under each `compression` setting, {t["parts"]["load"]["date"][:10]} on {host.get("cpu", "?")}, PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]} linked with `{s["sift_libs"]}`; '
           'size and build time of the index alone, every setting read back from the server that ran it.', '']
    head = ['`compression` of the `sift_col` index', '`reloptions`', 'index size', 'build time', 'vs `none`']
    body = [[CZ_NAMES[e], f'`{R.get(tier, e, "-", "orders_col", "reloptions")["value"]}`', f'{sz(e):,.1f} MiB', f'{R.num(tier, e, variant="build orders_col", metric="build_s"):.1f} s',
             f'{sz(e) / sz("sift_none"):.2f}x'] for e in lanes]
    out += md_table(head, body, ['---', '---', '---:', '---:', '---:']) + ['']
    out += [f'**Takeaway:** the default (`compression` unset = `auto`) is {sz("sift"):,.1f} MiB, {sz("sift") / sz("sift_none"):.2f}x of `none` ({sz("sift_none"):,.1f} MiB), and `zstd:3` is {sz("sift_zstd"):,.1f} MiB; '
            f'the build time stays within {min(R.num(tier, e, variant="build orders_col", metric="build_s") for e in lanes):.1f}-{max(R.num(tier, e, variant="build orders_col", metric="build_s") for e in lanes):.1f} s.', '', '**Caveats**', '']
    if 'sift_auto' in lanes and 'sift' in lanes:
        same = all(leaves['sift'][k]['bytes'] == leaves['sift_auto'][k]['bytes'] for k in leaves['sift'])
        enc = 'sift_enc' in lanes and all(leaves['sift'][k]['bytes'] == leaves['sift_enc'][k]['bytes'] for k in leaves['sift'])
        lz = 'sift_lz4' in lanes and all(leaves['sift'][k]['bytes'] == leaves['sift_lz4'][k]['bytes'] for k in leaves['sift'])
        out += [f'- The chunk bytes of every column ({"identical" if same else "NOT identical"} to an explicit `auto`, {"identical" if enc else "different"} to the encodings alone, {"identical" if lz else "different"} to `lz4`; `sift.col_leaves`) '
                'show that `auto` layered no LZ4 or zstd over the encodings on this data (the cost rule of [docs/perf/COMPRESSION.md](perf/COMPRESSION.md) accepts a layer only when its decode time is worth the bytes); no SQL function reports the layer chunk by chunk, so it is read from the byte counts.']
    qs = R.queries(tier)
    spread = [max(v) / min(v) for q in qs if len(v := [R.num(tier, e, q, 'warm', 'latency_ms') for e in lanes]) and all(v)]
    out += [f'- Warm time of a question across the {len(lanes)} settings: the slowest against the fastest {min(spread):.2f}x to {max(spread):.2f}x, in no fixed order, so the differences are run-to-run noise (the medians are in `store-compression.json`).']
    tbl = [(p, R.num(tier, 'sift_table', variant='codec ' + p, metric='size_mib'), R.num(tier, 'sift_table', variant='codec ' + p + ' free', metric='size_mib')) for p in ('none', 'auto', 'lz4', 'zstd:3')]
    others = [f'pg_sift `sift_table` with `sift.table_compression` = {p}: {size(v, f)}' for p, v, f in tbl if v is not None]
    others += [f'ClickHouse, {k}: {size(R.num(tier, "ch", variant=f"codec {k}", metric="size_mib"))}' for k in ('default (LZ4)', 'ZSTD(3)') if R.num(tier, 'ch', variant=f'codec {k}', metric='size_mib') is not None]
    others += [f'Elasticsearch, {k}: {size(R.num(tier, "es", variant=f"codec {k}", metric="size_mib") if k != "default (LZ4)" else R.num(tier, "es", metric="size_mib"))}' for k in ('default (LZ4)', 'best_compression') if R.engines(tier, ['es'])]
    others += [f'PostgreSQL (heap, btree, GIN): {size(R.num(tier, "pg", metric="size_mib"))}'] if R.engines(tier, ['pg']) else []
    others += [f'Citus columnar ({citus_options(R, tier)}), primary key: {size(R.num(tier, "citus", metric="size_mib"))}'] if R.engines(tier, ['citus']) else []
    out += ['- Table and indexes on disk of the other layouts and systems with their own compression settings: ' + '; '.join(others) + '.']
    out += ['', 'Raw results: [results/store-compression.json](results/store-compression.json); reproduce with `bench/bench store all --sizes 1000000 --compression --docs`.']
    return out


def replace_blocks(args):
    import readme  # steps/readme.py: replace_block
    R = Results(args.results)
    if args.readme:
        readme.replace_block(args.readme, 'store', readme_block(R))
        readme.replace_block(args.readme, 'store-updates', updates_readme(R))
    if args.benchmarks:
        readme.replace_block(args.benchmarks, 'store', benchmarks_block(R))
        readme.replace_block(args.benchmarks, 'store-updates', updates_benchmarks(R))
        if os.path.exists(args.compression):
            readme.replace_block(args.benchmarks, 'store-compression', compression_block(Results(args.compression)))


def cmd_render(args):
    if args.stdout:
        R = Results(args.results)
        print('\n'.join(readme_block(R) + [''] + updates_readme(R) + [''] + benchmarks_block(R) + [''] + updates_benchmarks(R)))
    else:
        replace_blocks(args)
    return 0


def all_lanes(R, tier):
    return R.engines(tier, list(LANES))


def set_title(R, tier):
    return f'{R.meta["tiers"][tier]["rows"]:,} orders' + (f', {cpu_name(R.cpus(tier))}' if R.cpus(tier) else '')


def cmd_table(args):
    """the warm medians of every result set (size and thread budget), as a Markdown table (stdout)"""
    R = Results(args.file)
    for tier in R.all_tiers:
        engines = all_lanes(R, tier)
        head, body = bench_table(R, tier, engines, {e: LANES[e][0] for e in LANES})
        print(f'\n{set_title(R, tier)}, warm median ms\n')
        print('\n'.join(md_table(head, body)))
    return 0


def cmd_compare(args):
    """two result files side by side: the warm median of every question and system in each result set both have, the ratio B / A (below 1: B is faster) with the 95% interval of the
    ratio of the medians of the runs, and * where the interval excludes 1 and a Mann-Whitney test of the runs rejects equality (benchlib.changed)"""
    A, B = Results(args.a), Results(args.b)
    body, flagged = [], 0
    for tier in [t for t in A.all_tiers if t in B.all_tiers]:
        for q in A.queries(tier):
            for e in all_lanes(A, tier):
                ra, rb = (X.get(tier, e, q, 'warm', 'latency_ms') for X in (A, B))
                if not (ra and rb and ra.get('values') and rb.get('values')):
                    continue
                ratio, lo, hi, p, diff = benchlib.changed(ra['values'], rb['values'])
                flagged += diff
                body.append([tier, q, QUERY_TITLES[q][:44], LANES[e][0], ms(ra['value']), ms(rb['value']), f'{ratio:.2f} [{lo:.2f}, {hi:.2f}]' + (' *' if diff else ''), f'{p:.3f}'])
    print('\n'.join(md_table(['set', 'q', 'question', 'system', 'A ms', 'B ms', 'B / A [95%]', 'p'], body, ['---'] * 4 + ['---:'] * 4)))
    print(f'\n* {flagged} of {len(body)} cells changed (interval of the ratio excludes 1 and Mann-Whitney p < 0.05)')
    return 0


def gap_rows(R, tier):
    """one row per question: the warm medians of sift_col, sift_table, PostgreSQL, ClickHouse and Elasticsearch (request cache off, on), and sift_col / ClickHouse, sift_col / Elasticsearch"""
    out = []
    for q in R.queries(tier):
        v = {e: R.num(tier, e, q, 'warm', 'latency_ms') for e in ('sift', 'sift_table', 'pg', 'ch', 'es', 'es_cached')}
        rel = lambda e: f'**{v["sift"] / v[e]:.2f}**' if v['sift'] and v[e] and v['sift'] > v[e] else f'{v["sift"] / v[e]:.2f}' if v['sift'] and v[e] else 'n/a'
        out.append([q, QUERY_TITLES[q], *(ms(v[e]) for e in v), rel('ch'), rel('es'), rel('es_cached')])
    return out


def gap_table(R):
    head = ['q', 'question', 'sift_col', 'sift_table', 'PostgreSQL', 'ClickHouse', 'Elasticsearch', 'ES, cache on', 'sift_col / ClickHouse', 'sift_col / ES', 'sift_col / ES cache on']
    out = []
    for tier in R.all_tiers:
        out += [f'#### {set_title(R, tier)} (warm median, ms; a ratio above 1, in bold, means pg_sift is slower)', '']
        out += md_table(head, gap_rows(R, tier), ['---:', '---'] + ['---:'] * 9) + ['']
    return out


def cmd_gap(args):
    print('\n'.join(gap_table(Results(args.file))))
    return 0
