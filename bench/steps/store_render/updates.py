# The update and delete lanes: their rows, losses and sentences, and the README and BENCHMARKS blocks of them.
# The labels and the documentation links of those lanes are here.
from store_charts import picture
from store_lib import LANES, LOAD_QUERIES, LOAD_QUERY, MODES, UPDATE_ENGINES
from .results import best_bold, md_table, ms, seconds, size
from .tables import QUERY_TITLES, col_cell, parts_cell, size_cells, upd_split
from .notes import cache_note, cpu_name, dagger, ratio
from .blocks import host_noise


UPD_LABELS = {'pg': 'PostgreSQL', 'sift': 'pg_sift `sift_col`', 'sift_maint': LANES['sift_maint'][0], 'sift_table': 'pg_sift `sift_table`', 'sift_table_maint': LANES['sift_table_maint'][0], 'es': 'Elasticsearch', 'ch': 'ClickHouse, lightweight UPDATE',
              'ch_mut': 'ClickHouse, ALTER UPDATE', 'ch_rmt': 'ClickHouse, ReplacingMergeTree + FINAL'}


UPD_PATH = {'pg': '`UPDATE` / `DELETE` ... `WHERE id IN (...)`, 1,000 ids per statement', 'sift': 'the same statements; the heap is updated, the `sift_col` index follows',
            'sift_maint': 'the same, in a database with the automatic vacuum and merge of the column index on', 'sift_table_maint': 'the same, in a database with `sift.table_auto_defrag = on`',
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
    modes = MODES
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
    for v, label in (('baseline', 'fresh table'), ('after churn', 'after the changes'), ('after maintenance', 'after VACUUM / merge / OPTIMIZE')):
        vals = [g(e, v, 'latency_ms', LOAD_QUERY) for e in engines]
        out.append([f'revenue per month, {label}: p50 (ms)'] + best_bold(vals, [ms(x) for x in vals]))
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
    sift = ('sift', 'sift_table', 'sift_maint', 'sift_table_maint')
    mine = [e for e in sift if e in engines]
    others = [e for e in engines if e not in sift]
    out = []
    for group in (('sift', 'sift_table'), ('sift_maint', 'sift_table_maint')):   # the defaults and the automatic maintenance are judged apart: a win of the second does not hide a loss of the first
        for v, label in (('update 1%', 'updating 1% of the rows'), ('update 10%', 'updating 10% of the rows'), ('delete 5%', 'deleting 5% of the rows')):
            m = {e: R.num(tier, e, '-', v, 'phase_rows_s') for e in group if e in engines}
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
    mine = [e for e in ('sift', 'sift_table', 'sift_maint', 'sift_table_maint') if e in engines]
    ranked = sorted(((g(e, 'update 10%', 'phase_rows_s'), e) for e in engines if g(e, 'update 10%', 'phase_rows_s')), reverse=True)
    if ranked:
        out.append('Updating 10% of the rows, in rows per second: ' + ', '.join(f'{UPD_LABELS[e]} {rate(v)}' for v, e in ranked) + '.')
    vis = {e: g(e, {'es': 'default'}.get(e, MODES.get(e)), 'visible_ms') for e in engines}
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


def updates_readme(R):
    """the README's changes of data: the cost chart (rows/s of the 10% update is in it), the size growth chart, the takeaway and the losses; the table is in docs/BENCHMARKS.md"""
    tier = upd_tier(R)
    if not tier:
        return ['No update numbers yet: `bench/bench store all` measures them.']
    engines = upd_engines(R, tier)
    t = R.meta['tiers'][tier]
    sentences = upd_sentences(R, tier, engines)
    out = [f'On the {t["rows"]:,}-order table, after the read tests, each system changes its rows the way its documentation recommends for the job; the state after each step is compared with PostgreSQL\'s.', '',
           '**Takeaway:** ' + sentences[0], '', picture('cost-per-row', 'Index bytes per row, load microseconds per row and rows per second of a 10% update, per system'), '',
           picture('store-updates', 'Size after the changes and after the maintenance step, against before, per system'), '']
    out += ['- ' + x for x in sentences[1:2] + cache_note(R, tier)]
    loss = upd_losses(R, tier, engines)
    if loss:
        out += ['- **Where pg_sift loses:** ' + '; '.join(loss) + '.']
    out += ['', 'Every row, the update path of each system and the official pages: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#updates-and-deletes); raw results [bench/results/store.json](bench/results/store.json).']
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
