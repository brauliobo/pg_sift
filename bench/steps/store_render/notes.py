# The sentences under a table: where the systems lose, the load and error notes, the thread and cache lines and the caveats.
# Each reads a Results and one tier and returns lines of Markdown.
import benchlib
import os
import re
from store_lib import CHUNK, LANES, PG_ENGINES
from .results import join_and, ms
from .tables import QUERY_LABELS, QUERY_TITLES, size_note


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


def duck_best(R, q, variant):
    """the faster pg_duckdb mode on a question (bench/results/duckdb.tsv, its own run), or None"""
    path = os.path.join(R.dir, 'duckdb.tsv')
    if not os.path.exists(path):
        return None
    from store_charts import read_tsv
    rows = read_tsv(path)[1]
    m = 'warm_ms' if variant == 'warm' else 'cold_ms'
    v = [float(r['value']) for r in rows if r['case'].split(' ')[0] == q and r['arm'] in ('duckdb_heap', 'duckdb_parquet') and r['metric'] == m and r['value'] not in ('NA', 'error', '')]
    return min(v) if v else None


def ahead(R, tier, engines, variant):
    """(questions where the faster layout of pg_sift is the fastest or within 5% of it, questions); pg_duckdb counts among the others"""
    mine = [e for e in ('sift', 'sift_table') if e in engines]
    others = [e for e in engines if e not in ('sift', 'sift_table', 'es_cached')]
    qs = [q for q in R.queries(tier) if any(R.num(tier, e, q, variant, 'latency_ms') for e in mine)]
    n = sum(1 for q in qs if min(R.num(tier, e, q, variant, 'latency_ms') or 1e99 for e in mine) <= min([(R.num(tier, e, q, variant, 'latency_ms') or 1e99) for e in others] + [duck_best(R, q, variant) or 1e99]) * 1.05)
    return n, len(qs)


def takeaway(R, tier, engines):
    """one line: where pg_sift stands, warm and cold, and against PostgreSQL"""
    w, c = ahead(R, tier, engines, 'warm'), ahead(R, tier, engines, 'cold')
    pg = [R.num(tier, 'pg', q, 'warm', 'latency_ms') / min(R.num(tier, e, q, 'warm', 'latency_ms') for e in ('sift', 'sift_table') if R.num(tier, e, q, 'warm', 'latency_ms')) for q in R.queries(tier) if R.num(tier, 'pg', q, 'warm', 'latency_ms')]
    return (f'**Takeaway:** the faster layout of pg_sift is the fastest system (pg_duckdb included), or within 5% of it, on {w[0]} of {w[1]} questions warm' + (f' and {c[0]} of {c[1]} cold' if c[1] else '')
            + f'; against PostgreSQL it is {ratio(min(pg), 1)} to {ratio(max(pg), 1)} faster.')


def caveat_lines(R, tier, engines, labels, size_rows_shown=True, brief=False):
    """the caveats of the store results, short bullets: where pg_sift is behind, what the settings change, what the numbers are not"""
    out = []
    lo, cold = losses(R, tier, engines, labels), losses(R, tier, engines, labels, 'cold')
    if lo:
        out.append('**Where pg_sift is not the fastest, warm:** ' + '; '.join(f'{QUERY_LABELS[qid]}: {lb2} {ratio(v1, v2)} faster ({ms(v2)} ms against {ms(v1)} ms)' for qid, _, lb, v1, lb2, v2 in lo) + '.')
    if cold:
        out.append('**Cold (the first run after the server\'s memory was emptied), not the fastest on** ' + ', '.join(f'{qid} ({lb2} {ratio(v1, v2)} faster)' for qid, _, lb, v1, lb2, v2 in cold) + '.')
    cached = [(q, R.num(tier, 'es_cached', q, 'warm', 'latency_ms'), min(R.num(tier, e, q, 'warm', 'latency_ms') for e in ('sift', 'sift_table') if R.num(tier, e, q, 'warm', 'latency_ms'))) for q in R.queries(tier) if R.num(tier, 'es_cached', q, 'warm', 'latency_ms')]
    if cached:
        behind = [f'{q} ({ratio(m, c)})' for q, c, m in cached if c < m * 0.95]
        out.append('Elasticsearch runs with its shard request cache off (a repeated aggregation would otherwise be answered from memory); with it on' + (f' it is faster than pg_sift on {", ".join(behind)}.' if behind else ' it is not faster than pg_sift on any question.'))
    rd = R.get(tier, 'es', 'customers-per-month', '-', 'rel_diff')
    out.append('Every result is compared with PostgreSQL\'s before it is timed and the run stops on a difference; Elasticsearch counts distinct customers approximately (HyperLogLog++' + (f', off by at most {rd["value"]:.2%} here' if rd else '') + '), every other result matched exactly or within rounding.')
    if brief:
        return out
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
