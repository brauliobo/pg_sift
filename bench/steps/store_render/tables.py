# The per-tier tables: the query rows, the three-figure cells, the size split and the load rows.
# Built from a Results; the README and BENCHMARKS blocks put them together.
import triple
from store_lib import LANES, load_queries
from .results import best_bold, join_and, ms, seconds, size


def bench_table(R, tier, engines, labels, variant='warm', search=False):
    head = ['query'] + [labels[e] for e in engines]
    body = []
    for qid in R.queries(tier, search):
        rs = [R.get(tier, e, qid, variant, 'latency_ms') for e in engines]
        vals = [r['value'] if r else None for r in rs]
        body.append([QUERY_LABELS[qid]] + best_bold(vals, [ms(v) for v in vals]))
    return head, body


READ = {'warm': 'MB read (the data a request touched: PostgreSQL, pg_sift and Citus the buffers it hit and read, ClickHouse the bytes its query log counts as read)',
        'cold': 'MB read (past the engine\'s own caches: PostgreSQL, pg_sift and Citus the shared reads, ClickHouse the bytes read from files, Elasticsearch the disk reads of its container; the operating system\'s page cache stays, so these are not device reads)'}


def triple_table(R, tier, engines, labels, search=False, variant='warm'):
    """the questions of a result set as cells of time, CPU and IO (steps/triple.py), then what each slug stands for"""
    qs = R.queries(tier, search)
    fig = lambda e, q: triple.fig(lambda m: R.get(tier, e, q, variant, m))  # noqa: E731
    mine = lambda q: triple.best(fig(e, q) for e in triple.MINE if e in engines)  # noqa: E731
    return triple.table({labels[e]: e for e in engines}, [(q, q) for q in qs], fig, lambda e, q: mine(q), read=READ[variant]) + ['', 'The questions: ' + '; '.join(f'`{q}` = {QUERY_TITLES[q]}' for q in qs) + '.']


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
