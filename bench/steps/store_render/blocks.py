# The README block and the docs/BENCHMARKS.md block of the orders benchmark, with the thread and Citus tables.
# They join the tables and the notes of one run.
from store_charts import picture
from store_lib import LANES
from .results import md_table
from .tables import SHOWN, load_rows, triple_table
from .notes import caveat_lines, citus_options, cpu_name, dagger, takeaway, thread_line


def readme_block(R):
    """the aggregate questions of the README: the two charts, the takeaway, the few caveats that change a decision; the machine and the method are the footnote [^store]"""
    tier = R.tiers[-1]
    t = R.meta['tiers'][tier]
    s = t['servers']
    labels = {e: LANES[e][0] for e in LANES}
    engines = R.engines(tier, SHOWN)
    host = R.meta.get('host', {})
    qs = R.queries(tier)
    out = [f'{len(qs)} aggregate questions (a point lookup, counts and sums by group, distinct counts, a filtered average, a range count) on {t["rows"]:,} orders, the same rows in every system, one client, warm medians[^store]. '
           'The full-text questions of the same set are in [Search](#search).', '',
           picture('agg-latency', f'Median time per question and system on a logarithmic axis, {t["rows"]:,} orders'), '', takeaway(R, tier, engines), '',
           picture('agg-speedup', 'How many times faster than PostgreSQL each system is on each question, warm and cold'), '']
    out += ['- ' + c for c in caveat_lines(R, tier, engines, labels, brief=True)]
    out += ['', 'Every table, the cold runs, sizes and the configuration: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#aggregates-an-orders-dataset); raw results [bench/results/store.json](bench/results/store.json); reproduce with `bench/bench store all`.', '',
            f'[^store]: Each system alone in a docker container with {cpu_name(R.cpus(tier))} (a quota, no pinning) and {host.get("memory", "?")} of memory and the same thread budget ([method](docs/BENCHMARKS.md#configuration)), '
            f'a local connection; median of {t["runs"]} warm runs; the first run after the server\'s memory was emptied is "cold". Measured {t["parts"]["warm"]["date"][:10]} on {host.get("cpu", "?")} ({host.get("threads", "?")} threads), '
            f'PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]}' + (f', Citus {s["citus_columnar"]} columnar' if 'citus' in engines else '') + f', Elasticsearch {s["es"]}, ClickHouse {s["ch"]}; host at the start of the warm runs: {host_noise(t["parts"]["warm"])}. '
            'pg_duckdb is its own run (the better of its heap and Parquet modes, a different PostgreSQL build next to it, [docs/BENCHMARKS.md](docs/BENCHMARKS.md#pg_duckdb)): its factor is against the PostgreSQL of that run.']
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
    out = [f'{t["rows"]:,} orders and {len(R.queries(tier))} aggregate questions (the {len(R.queries(tier, search=True))} full-text ones are in the search section), {cpu_name(R.cpus(tier))} per container ({thread_line(R, tier)}). Median of {t["runs"]} warm runs, three measures in each cell (the fastest and slowest run of every time are in bench/results/store.json); the fastest system of a question and anything within 5% of its time in bold, the factors are against the faster pg_sift layout. '
           f'Measured {t["parts"]["warm"]["date"][:10]} on {host.get("cpu", "?")}, PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]}, Elasticsearch {s["es"]}, ClickHouse {s["ch"]}'
           + (f', Citus {s["citus_columnar"]}' if 'citus' in engines else '') + f'; seeded data sha256 `{t["data_sha256"][:16]}`; {host_noise(t["parts"]["warm"])} at the start of the runs (the host is shared).', '']
    out += triple_table(R, tier, engines, dagger(R, tier, labels)) + [''] + md_table(['measure'] + [dagger(R, tier, labels)[e] for e in engines], load_rows(R, tier, engines)) + ['', takeaway(R, tier, engines), '',
                                                                   picture('agg-speedup', 'How many times faster than PostgreSQL each system is on each question, warm and cold', 'img/'), '', '**Caveats**', '']
    out += ['- ' + c for c in caveat_lines(R, tier, engines, labels)]
    out += ['', '#### Search with filters', '', 'The three full-text questions of the same set, a text search with filters on the same rows (the search benchmarks proper are in the [README](../README.md#search)); they are not part of the aggregates above.', '']
    out += triple_table(R, tier, engines, dagger(R, tier, labels), search=True)
    if any(R.get(tier, e, q, 'cold', 'latency_ms') for e in engines for q in R.queries(tier)):
        out += ['', '#### Cold', '', 'The first run of every question after the server\'s memory was emptied ([method](#configuration)), the same three measures; the aggregate questions, then the searches.', ''] + triple_table(
            R, tier, engines, dagger(R, tier, labels), variant='cold') + [''] + triple_table(R, tier, engines, dagger(R, tier, labels), search=True, variant='cold')
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
    ch_threads = "`max_threads`, `max_insert_threads`, `max_final_threads` and `background_pool_size` set to N" if th else f"`max_threads` {s['ch_threads']} (what the container limit gives it)"
    out += [f'- PostgreSQL {s["pg"]} (the same image for both layouts, pg_sift {s["sift_sift"]} (the build under test)' + (f', linked with `{s["sift_libs"]}`, `compression` unset on the index = `auto`, `sift.table_compression` {s["sift_table_compression"]}' if 'sift_libs' in s else '') + f'): {s["pg_settings"]}; {pg_text}.',
            *citus_config(R, last),
            f'- Elasticsearch {s["es"]} (Lucene {s["es_lucene"]}): one node, {R.cpus(last)} shards (one per CPU of the budget of the load: a shard is searched by one thread, so with one shard a search uses one core whatever `node.processors` says; the shard count is a choice of this harness), no replica, JVM heap `{s["es_heap"]}`, {"`node.processors` and `-XX:ActiveProcessorCount` set to N, " if th else ""}security and machine learning off; `scaled_float` (factor 100) for the amount, `keyword` for country, category and status, `date` for the time, `text` for the title; '
            'each shard merged to one segment after the load; aggregations of the search API with `size: 0`; the shard request cache off (`request_cache=false`) except in the table that says otherwise; distinct counts are `cardinality` (HyperLogLog++, `precision_threshold` 40000), the only approximate result.',
            f'- ClickHouse {s["ch"]}: `MergeTree`, `PARTITION BY` month, `ORDER BY (country, created_at, id)` (`status` and `amount` stay out of the key so that they can be updated), `LowCardinality` strings, `Decimal(10, 2)` for the amount, a text index on the title, a Bloom filter on the id, the settings lightweight updates need; {ch_threads}; optimized to one part per partition after the load.',
            '- pg_sift: `sift_col` over the columns the reports read plus the month of the order, a `sift` BM25 index on the title; `sift_table` is the table itself in column format with a `sift` index on the title and the primary key. PostgreSQL: primary key, btree on the date and on country and status, GIN on `to_tsvector(\'simple\', title)`.',
            f'- Method: every engine has {cpu_name(quota)} for its server, the budget of the search tables; one client, one request at a time, wall time from this process to the end of the response over a persistent local connection; the engines are timed in turn within each run so a drift of the host load hits all of them; one untimed run per engine first (its result is checked against PostgreSQL\'s), then the timed runs. The numbers of one table are comparable with each other, not with another host.',
            '- CPU and IO: the CPU of a request is the CPU time the container\'s cgroup (`cpu.stat`) used between its first byte sent and the end of the response, every process of the server included (parallel workers, the JVM\'s background threads, ClickHouse\'s merges); median of the timed runs. '
            'MB read, warm, is what the engine says the request touched: PostgreSQL, pg_sift and Citus the buffers hit and read of one more `EXPLAIN (ANALYZE, BUFFERS)` run (a page read twice counts twice), ClickHouse `read_bytes` of its query log (decompressed); Elasticsearch has no per-request counter (`n/a`). '
            'MB read, cold, is read past the engine\'s own caches in the first run after they were emptied: the shared reads (PostgreSQL family), the bytes ClickHouse read from files, and for Elasticsearch (restarted, not emptied) the container\'s disk reads (`io.stat`). '
            'The operating system\'s page cache stays in every cold run (dropping it needs root), so a cold figure is not a device read; it tells how much each engine must fetch from below its own cache.']
    return out
