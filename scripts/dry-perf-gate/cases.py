"""Cases of the DRY performance gate. A case is one statement measured in a fresh backend: instructions:u and cycles:u of the whole process tree (perf stat on the
postmaster, so parallel workers count), elapsed ms, blocks hit / read / written / extended of the backend, WAL bytes, the md5 of the sorted result rows.

Sources, nothing parallel to the repo's own harnesses:
  corpus   a statement of test/corpus/cases.sql by slug, on the corpus lab (test/corpus/lab.sql at :rows events); pre-lines (`@pre`) run before the window;
  catalog  a statement of bench/perf/catalog.py (the focused performance gate of CI) by name, on tables of the same names (fixture-gate.sql);
  gate     a statement of this file on fixture-gate.sql: the write / build / promote / merge / vacuum paths the other two do not time.

kind     warm   one un-measured execution in the same backend first (relcache, catcache, plan caches warm), then the window (a read in steady state);
         once   no warm-up: the first statement of a backend (the maintenance cases, where a second run would change the state);
         evict  fresh backend and pg_buffercache_evict_all() before the window: the first statement after eviction (blocks read are the cold reads).
`pre` statements run before the window and `post` after it, both outside the counters; state-changing cases reset their state in `pre` so every run starts equal.
"""
from dataclasses import dataclass, field, replace

from ports import MODES as PORT_MODES


@dataclass(frozen=True)
class Case:
    id: str
    groups: tuple
    sql: str = ''
    src: str = 'gate'          # corpus | catalog | gate | sqlite | mariadb (the ports: ports.py)
    kind: str = 'warm'
    pre: tuple = ()
    post: tuple = ()
    plan: bool = True          # capture EXPLAIN (COSTS OFF) of the statement and require the same plan on both sides
    settings: str = ''         # session settings after the common ones
    par: bool = False          # workers split the work dynamically: instructions, blocks and WAL vary a little between two runs of the same code (wider tolerances)
    expect: str = ''           # a regex the plan must match (a parallel plan really launches workers): otherwise the case fails, it does not measure another plan
    clients: int = 0           # concurrent backends running the statement `loops` times each, counters summed over them (par tolerances: the sharing of the caches varies a little)
    loops: int = 1
    after: tuple = ()          # statements inside the window after the measured one (the wait for a background worker)
    cleanup: tuple = ()        # run after the window, even when the run failed (the settings of a worker case are the database's, not the session's)
    note: str = ''


def g(*names):
    return tuple(names)


# --- corpus statements (agg and col reads) ------------------------------------------------------------------------------------------------------------------
CORPUS = [
    # ungrouped aggregates and kernels
    ('agg_count_star',            g('smoke', 'agg-read', 'agg-kernel')),
    ('agg_basic_five',            g('smoke', 'agg-read', 'agg-kernel', 'first')),
    ('agg_no_group_filter',       g('agg-read', 'agg-kernel')),
    ('c3_wide_sum_all_float',     g('agg-kernel')),
    ('c3_wide_sum_all_int',       g('agg-kernel')),
    ('agg_numeric_sum',           g('agg-kernel')),
    ('s2_stddev_variance',        g('agg-kernel')),
    ('agg_text_minmax',           g('agg-kernel')),
    # grouped: few groups, many groups, FILTER / CASE bitmap forms
    ('agg_filter_clause',         g('smoke', 'agg-read', 'agg-kernel', 'first')),
    ('s2_sum_case_pivot',         g('agg-read', 'agg-kernel')),
    ('c3_groups_label_count',     g('agg-read')),
    ('agg_group_many_cols',       g('agg-read')),
    ('agg_distinct_entity',       g('agg-read', 'agg-kernel')),
    ('agg_rollup',                g('agg-read')),
    # ordered-set, arrays
    ('agg_percentile',            g('agg-kernel')),
    ('agg_mode',                  g('agg-kernel')),
    ('agg_array_agg',             g('agg-read', 'agg-kernel')),
    # top-N
    ('topn_order_limit',          g('agg-topn')),
    ('c3_groups_top_labels',      g('agg-topn')),
    ('c2_topk_tenant_amount',     g('agg-topn')),
    # HAVING and IN sub-select
    ('agg_having',                g('agg-read', 'agg-plan')),
    ('c3_having_in_list',         g('agg-plan')),
    ('join_set_in_subselect_75_group', g('agg-plan', 'agg-read')),
    ('having_count_gt',           g('agg-plan')),
    # join to a lookup table
    ('join_one_dim',              g('agg-read', 'agg-plan')),
    ('join_two_dims',             g('agg-plan')),
    # jsonb ? and containment
    ('pred_jsonb_contains',       g('agg-read')),
    ('c3_jsonb_key_filter',       g('agg-read')),
    ('c3_jsonb_key_case',         g('agg-read')),
    # range lane tests
    ('c3_range_overlap_count',    g('agg-read')),
    ('c3_range_lower_month',      g('agg-read')),
    # text predicates, CTEs, partitions, lookup joins, facets, distinct pairs
    ('agg_group_highcard_50k',    g('agg-read')),
    ('agg_distinct_multi',        g('agg-read', 'agg-kernel')),
    ('pred_like_prefix',          g('agg-read')),
    ('pred_like_contains',        g('agg-read')),
    ('pred_ilike',                g('agg-read')),
    ('pred_jsonb_path',           g('agg-read')),
    ('acc_partition_all',         g('agg-read')),
    ('c2_events_p_month_status',  g('agg-read')),
    ('c2_cte_twice_selfjoin',     g('agg-plan', 'agg-read')),
    ('c2_cte_two_levels',         g('agg-plan')),
    ('s2_cte_two_aggs_join',      g('agg-plan')),
    ('join_set_in_subselect_1000', g('agg-plan', 'agg-read')),
    ('join_set_bigdim_cte',       g('agg-plan')),
    ('arch_facets_one_query',     g('agg-plan', 'agg-read')),
    # point lookups, range scans, time series on sift_col
    ('acc_point_pk',              g('smoke', 'col-read', 'first')),
    ('acc_entity_list',           g('col-read')),
    ('s2_small_group_one_entity', g('col-read')),
    ('pred_range_amount',         g('col-read', 'agg-read')),
    ('ts_last_30_days',           g('col-read')),
    ('ts_readings_bucket',        g('col-read', 'agg-read')),
]

# --- catalog statements of bench/perf/catalog.py (bm25 and storage) -----------------------------------------------------------------------------------------
CATALOG = [
    ('bm25_top10_common',     g('smoke', 'bm25-read', 'first')),
    ('bm25_top10_rare',       g('bm25-read')),
    ('bm25_count_common',     g('smoke', 'bm25-read', 'first')),
    ('bm25_count_rare',       g('bm25-read')),
    ('bm25_filtered_top10',   g('bm25-read')),
    ('bm25_filtered_count',   g('bm25-read')),
    ('bm25_phrase_nightly',   g('bm25-read')),
    ('storage_point_lookup',  g('smoke', 'table', 'first')),
    ('agg_table_group',       g('table', 'smoke')),
    ('agg_table_scan',        g('table')),
]

RESET = 'TRUNCATE mutable', 'INSERT INTO mutable SELECT * FROM seed ORDER BY id', 'VACUUM (FREEZE, ANALYZE) mutable'
FILL_WBP = ("TRUNCATE wbp", "INSERT INTO wbp SELECT i, i % 16, i % 1000, 'common w' || i % 97 || ' t' || (i::bigint * 7919) % 2000 FROM generate_series(1, 4000) i")
FILL_MT = ('TRUNCATE mt',) + tuple(
    stmt for k in range(6) for stmt in (
        f"INSERT INTO mt SELECT i, i % 16, i % 1000 FROM generate_series({k * 3000 + 1}, {k * 3000 + 3000}) i", "SELECT sift.col_promote('mt_col')"))

FILL_JW = ('TRUNCATE jw',) + tuple(
    stmt for k in range(4) for stmt in (
        f"INSERT INTO jw SELECT id, g, doc FROM cd WHERE id > {k * 3000} AND id <= {k * 3000 + 3000}", "SELECT sift.col_promote('jw_col')"))

FILL_WBP_SEGS = ('TRUNCATE wbp',) + tuple(
    stmt for k in range(6) for stmt in (
        f"INSERT INTO wbp SELECT i, i % 16, i % 1000, 'common w' || i % 97 || ' t' || (i::bigint * 7919) % 2000 FROM generate_series({k * 1500 + 1}, {k * 1500 + 1500}) i", "SELECT sift.promote('wbp_bm25')"))
PAR = ('set max_parallel_workers_per_gather = 2; set parallel_setup_cost = 0; set parallel_tuple_cost = 0; set min_parallel_table_scan_size = 0; '
       'set min_parallel_index_scan_size = 0;')
PAR_MAINT = 'set max_parallel_maintenance_workers = 2; set min_parallel_table_scan_size = 0;'

GATE = [
    # aggregate kernels and group tables on the lab events
    Case('agg_sum_int_expr', g('agg-kernel'), 'SELECT sum(qty * 3 + score), count(*), min(qty) FROM events'),
    Case('agg_ts_hour_metric', g('agg-kernel', 'agg-read'), "SELECT date_trunc('hour', created_at) AS h, count(*), sum(amount) FROM events GROUP BY 1"),
    Case('agg_group_spill', g('agg-read'), 'SELECT label, count(*), sum(amount) FROM events GROUP BY label', settings="set work_mem = '1MB';", note='a group table over work_mem'),
    # sift_col: deleted rows no VACUUM took, the sketch, verification, rows written under change tracking
    Case('col_dead_group', g('col-read', 'agg-read'), 'SELECT g, count(*), sum(amount) FROM dd GROUP BY g ORDER BY g', note='5% deleted rows, pre-aggregate read with the dead-row correction'),
    Case('col_dead_group_many', g('col-read', 'agg-read'), 'SELECT g2, count(*), sum(amount), avg(x) FROM dd GROUP BY g2'),
    Case('col_dead_count', g('col-read'), 'SELECT count(*), max(id) FROM dd'),
    Case('col_hll_distinct', g('agg-kernel', 'col-read'), 'SELECT g, sift.approx_count_distinct(g2) FROM dd GROUP BY g ORDER BY g'),
    Case('col_verify', g('col-maint'), "SELECT sift.col_verify('docs_col')", kind='once', plan=False),
    Case('col_track_update', g('col-maint', 'col-wbuf'), 'UPDATE tr SET amount = amount + 1 WHERE id % 10 = 0', kind='once',
         pre=('TRUNCATE tr', 'INSERT INTO tr SELECT i, i % 16, i % 1000 FROM generate_series(1, 20000) i', "SELECT sift.col_promote('tr_col')"), plan=False),
    Case('col_track_delete', g('col-maint', 'col-wbuf'), 'DELETE FROM tr WHERE id % 10 = 0', kind='once',
         pre=('TRUNCATE tr', 'INSERT INTO tr SELECT i, i % 16, i % 1000 FROM generate_series(1, 20000) i', "SELECT sift.col_promote('tr_col')"), plan=False),
    # bm25: several terms, ranked OR, merge and vacuum of promoted segments
    Case('bm25_and_terms', g('bm25-read'), "SELECT count(*) FROM docs WHERE body @@> 'word7 AND t17 AND clean'"),
    Case('bm25_or_three_top20', g('bm25-read'), "SELECT id FROM docs WHERE body @@> 'word7 OR t17 OR u5' ORDER BY sift.score(ctid) DESC, id LIMIT 20"),
    Case('bm25_merge', g('bm25-maint'), "SELECT sift.merge('wbp_bm25', true)", kind='once', pre=FILL_WBP_SEGS, plan=False),
    Case('bm25_vacuum', g('bm25-maint'), 'VACUUM wbp', kind='once', pre=FILL_WBP_SEGS + ('DELETE FROM wbp WHERE id % 10 = 3',), plan=False),
    # parallel workers, opt-in group `par` (workers 2: the instructions are those of the leader and its workers together)
    Case('par_agg_group', g('par', 'agg-par'), 'SELECT label, count(*), sum(amount) FROM events GROUP BY label', settings=PAR, expect='Workers Planned', par=True),
    Case('par_agg_wide', g('par', 'agg-par'), 'SELECT status, sum(m01), sum(m02), sum(n01), count(*) FROM wide GROUP BY status', settings=PAR, expect='Workers Planned', par=True),
    Case('par_agg_ungrouped', g('par', 'agg-par'), 'SELECT count(*), sum(amount), avg(score) FROM events', settings=PAR, expect='Workers Planned', par=True),
    Case('par_col_build', g('par', 'col-build'), 'CREATE INDEX cb_col ON cb USING sift_col (id, g, g2, v, x, t)', kind='once', settings=PAR_MAINT, pre=('DROP INDEX IF EXISTS cb_col',), post=('DROP INDEX cb_col',), plan=False, par=True),
    Case('par_bm25_build', g('par', 'bm25-build'), 'CREATE INDEX cbb_bm25 ON cbb USING sift (body)', kind='once', settings=PAR_MAINT, pre=('DROP INDEX IF EXISTS cbb_bm25',), post=('DROP INDEX cbb_bm25',), plan=False, par=True),
    # float column evaluated per bucket of its values (grouping by an expression of a float8 column)
    Case('agg_group_float_floor', g('agg-kernel', 'agg-read'), 'SELECT floor(amount / 5)::int AS k, count(*), sum(qty) FROM events WHERE amount BETWEEN -1e9 AND 1e9 GROUP BY 1'),
    Case('agg_group_float_cast', g('agg-kernel'), 'SELECT (amount / 3)::int AS k, count(*), min(qty), max(qty) FROM events GROUP BY 1'),
    Case('agg_group_float_round_avg', g('agg-kernel'), 'SELECT round(amount) + 0.0 AS k, count(*), avg(score) FROM events GROUP BY 1'),
    # one statement for each aggregate kind the corpus statements above do not reach (SiftAggKind), on the lab
    Case('agg_count_col', g('agg-kernel', 'agg-read'), "SELECT status, count(note), count(*) FROM events GROUP BY status", note='count(col): SAA_COUNT; count(*) beside it'),
    Case('agg_count_distinct_unique', g('agg-kernel', 'agg-read'), "SELECT e.status, count(DISTINCT e.id) FROM events e, unnest(e.tags) t GROUP BY e.status", note='count(DISTINCT id) of a unique id per element row: SAA_COUNT_ROWS'),
    Case('agg_percentile_disc', g('agg-kernel', 'agg-read'), "SELECT status, percentile_disc(0.5) WITHIN GROUP (ORDER BY qty), count(*) FROM events GROUP BY status", note='percentile_disc: SAA_PCT_DISC'),
    Case('agg_grouping_sets', g('agg-kernel', 'agg-read'), "SELECT status, region, count(*), grouping(status) FROM events GROUP BY ROLLUP (status, region)", note='ROLLUP with GROUPING(): SAA_GROUPING'),
    Case('agg_join_col_minmax', g('agg-kernel', 'agg-read'), "SELECT e.status, count(d.type), count(DISTINCT d.region), min(d.region), max(d.region) FROM events e JOIN dim_a d ON d.id = e.dim_a_id GROUP BY e.status", note='count / count(DISTINCT) / min / max of a joined table column: SAA_M_COUNT, M_DISTINCT, M_MIN, M_MAX'),
    Case('agg_join_col_sum', g('agg-kernel', 'agg-read'), "SELECT o.status, sum(p.price), avg(p.price), count(p.category) FROM orders o JOIN order_items i ON i.order_id = o.id JOIN products p ON p.id = i.foreign_id_b GROUP BY o.status", note='sum / avg of a float8 column of a joined table: SAA_M_SUM, M_AVG'),
    Case('agg_join_col_pg', g('agg-kernel', 'agg-read'), "SELECT e.status, sum(t.id), avg(t.id) FROM events e JOIN tenants t ON t.id = e.tenant_id GROUP BY e.status", note='sum / avg of an int4 column of a joined table: SAA_M_PG'),
    Case('agg_arm_float', g('agg-kernel', 'agg-read'), "SELECT status, count(*) AS cells, avg(s) AS a, sum(s) AS t, count(DISTINCT id) AS ids FROM (SELECT e.id, e.status, t, sum(e.amount) AS s FROM events e, unnest(e.tags) t GROUP BY e.id, e.status, t) x GROUP BY status", note='outer aggregates over per-row float8 aggregates: SAA_O_COUNT_STAR, O_COUNT_ROWS, O_AVG_F64, O_SUM_F64'),
    Case('agg_arm_int', g('agg-kernel', 'agg-read'), "SELECT status, count(*) AS cells, sum(s) AS t, count(DISTINCT id) AS ids FROM (SELECT e.id, e.status, t, sum(e.qty) AS s FROM events e, unnest(e.tags) t GROUP BY e.id, e.status, t) x GROUP BY status", note='outer sum of a per-row int8 aggregate: SAA_O_SUM_I64'),
    Case('agg_arm_key_distinct', g('agg-kernel', 'agg-read'), "SELECT status, count(DISTINCT t) AS tags, sum(s) FROM (SELECT e.id, e.status, t, sum(e.amount) AS s FROM events e, unnest(e.tags) t GROUP BY e.id, e.status, t) x GROUP BY status", note='count(DISTINCT) of a column the arms group by: SAA_O_COUNT_KEY'),
    Case('agg_expr_minmax', g('agg-kernel', 'agg-read'), "SELECT status, count(note), min(amount + qty), max(amount + qty), count(amount + qty) FROM events GROUP BY status", note='min / max / count of an expression PostgreSQL evaluates: SAA_X_COUNT, X_MIN, X_MAX'),
    Case('agg_expr_numeric', g('agg-kernel', 'agg-read'), "SELECT status, avg(amount * qty::numeric), sum(amount::numeric * qty), count(DISTINCT dim_a_id) FROM events GROUP BY status", note='sum / avg of a numeric expression: SAA_X_SUM, X_AVG'),
    # bm25
    Case('bm25_phrase_rare', g('bm25-read'), "SELECT count(*) FROM docs WHERE body @@> '\"rare clean\"'"),
    Case('bm25_or_recheck', g('bm25-read', 'agg-read'), "SELECT count(*) FROM docs WHERE body @@> 'rare OR word7' AND amount > 500"),
    Case('bm25_wbuf_count', g('bm25-wbuf', 'bm25-read'), "SELECT count(*) FROM wb WHERE body @@> 'common'", note='read with an open write buffer'),
    Case('bm25_wbuf_top10', g('bm25-wbuf'), "SELECT id FROM wb WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC, id LIMIT 10"),
    Case('bm25_build', g('bm25-build'), 'CREATE INDEX cbb_bm25 ON cbb USING sift (body)', kind='once', pre=('DROP INDEX IF EXISTS cbb_bm25',), post=('DROP INDEX cbb_bm25',), plan=False),
    Case('bm25_promote', g('bm25-maint', 'bm25-wbuf'), "SELECT sift.promote('wbp_bm25') >= 0", kind='once', pre=FILL_WBP, plan=False),
    # col
    Case('col_wbuf_group', g('col-wbuf', 'agg-read'), 'SELECT g, count(*), sum(amount) FROM wb GROUP BY g ORDER BY g', note='aggregate with an open write buffer'),
    Case('col_wbuf_point', g('col-wbuf', 'col-read'), 'SELECT count(*), sum(amount) FROM wb WHERE id = 1000005'),
    Case('col_hll_distinct', g('col-read', 'agg-read'), 'SELECT sift.approx_count_distinct(k) FROM sc', note='the merge of the unit sketches of an index with hll_columns'),
    Case('col_sorted_run_topn', g('col-read'), 'SELECT id, k FROM sc WHERE v > 500 ORDER BY k LIMIT 100', note='top-N read by the sorted runs of the units (order_columns)'),
    Case('col_wbuf_insert', g('col-wbuf', 'smoke'), "INSERT INTO wbp SELECT i, i % 16, i % 1000, 'common w' || i % 97 FROM generate_series(1, 3000) i", kind='once',
         pre=('TRUNCATE wbp',), plan=False),
    Case('col_promote', g('col-maint', 'col-wbuf'), "SELECT sift.col_promote('wbp_col') >= 0", kind='once', pre=FILL_WBP, plan=False),
    Case('col_build', g('col-build'), 'CREATE INDEX cb_col ON cb USING sift_col (id, g, g2, v, x, t)', kind='once', pre=('DROP INDEX IF EXISTS cb_col',), post=('DROP INDEX cb_col',), plan=False),
    Case('col_reindex', g('col-build', 'col-maint'), 'REINDEX INDEX docs_col', kind='once', plan=False),
    Case('col_merge', g('col-maint'), "SELECT sift.col_merge('mt_col', true)", kind='once', pre=FILL_MT, plan=False),
    Case('col_vacuum', g('col-maint'), 'VACUUM mt', kind='once', pre=FILL_MT + ('DELETE FROM mt WHERE id % 10 = 3',), plan=False),
    # a nullable jsonb document with paths: the presence leaf of the document
    Case('col_doc_not_contains', g('col-read', 'agg-read'), 'SELECT count(*), sum(id) FROM cd WHERE NOT (doc @> \'{"b": "x"}\')', note='NOT of a containment over a nullable document: the presence leaf'),
    Case('col_doc_is_null', g('col-read', 'agg-read'), 'SELECT count(*) FROM cd WHERE doc IS NULL'),
    Case('col_doc_build', g('col-build'), "CREATE INDEX cd_build ON cd USING sift_col (id, g, doc) WITH (key_cut = g, paths = 'doc.a int4, doc.b astext')", kind='once',
         pre=('DROP INDEX IF EXISTS cd_build',), post=('DROP INDEX cd_build',), plan=False),
    Case('col_doc_insert', g('col-wbuf'), "INSERT INTO jw SELECT id, g, doc FROM cd WHERE id <= 3000", kind='once', pre=('TRUNCATE jw',), plan=False),
    Case('col_doc_promote', g('col-maint', 'col-wbuf'), "SELECT sift.col_promote('jw_col') >= 0", kind='once', pre=('TRUNCATE jw', 'INSERT INTO jw SELECT id, g, doc FROM cd WHERE id <= 12000'), plan=False),
    Case('col_doc_merge', g('col-maint'), "SELECT sift.col_merge('jw_col', true)", kind='once', pre=FILL_JW, plan=False),
    # table
    Case('table_insert', g('table'), 'INSERT INTO mutable SELECT * FROM seed WHERE id <= 3000', kind='once', pre=('TRUNCATE mutable',), plan=False),
    Case('table_update', g('table'), 'UPDATE mutable SET amount = amount + 1 WHERE id BETWEEN 100 AND 200', kind='once', pre=RESET, plan=False),
    Case('table_vacuum', g('table'), 'VACUUM (DISABLE_PAGE_SKIPPING) mutable', kind='once', pre=RESET + ('DELETE FROM mutable WHERE id > 2048',), plan=False),
]

# --- background maintenance workers (opt-in group `bgw`): the statement asks a worker, the window ends when the worker has finished. The counters are those of the whole process tree,
# so they hold the statement and the worker. The settings are the database's (a worker reads those, not the session's) and are reset by `cleanup`, whatever the run did.
NO_WORKERS = "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'"
ASYNC_ON = ('ALTER DATABASE gate SET sift.async_maintenance = on', 'ALTER DATABASE gate SET sift.col_auto_merge = on', 'ALTER DATABASE gate SET sift.table_async_promote = on',
            'SET sift.async_maintenance = on', 'SET sift.col_auto_merge = on', 'SET sift.table_async_promote = on')
ASYNC_OFF = ('ALTER DATABASE gate RESET sift.async_maintenance', 'ALTER DATABASE gate RESET sift.col_auto_merge', 'ALTER DATABASE gate RESET sift.table_async_promote', NO_WORKERS)


def wait_for(pred):
    """A statement that polls every 0.1 s until `pred` holds (an error after 30 s: the worker did not do what the case measures)."""
    return (f"DO $$ BEGIN FOR i IN 1..300 LOOP PERFORM pg_sleep(0.1); EXIT WHEN {pred}; END LOOP; IF NOT ({pred}) THEN RAISE EXCEPTION 'the worker did not finish'; END IF; END $$",)


BGW = [
    # VACUUM of a churned index asks the rewrite worker (7 segments -> 2)
    Case('bgw_col_rewrite', g('bgw', 'col-maint'), 'VACUUM mt', kind='once', pre=(NO_WORKERS,) + FILL_MT + ('DELETE FROM mt WHERE id % 10 < 4',) + ASYNC_ON, plan=False, par=True,
         after=wait_for("(SELECT segments FROM sift.worker_state('mt_col')) < 7 AND coalesce((SELECT NOT running FROM sift.worker_state('mt_col')), true)"), cleanup=ASYNC_OFF,
         note='the rewrite worker VACUUM asks for'),
    # an insert that seals the write buffer asks the promotion worker
    Case('bgw_bm25_promote', g('bgw', 'bm25-maint', 'bm25-wbuf'), 'INSERT INTO wbp SELECT * FROM docs', kind='once', pre=(NO_WORKERS, 'TRUNCATE wbp') + ASYNC_ON, plan=False, par=True,
         after=wait_for("coalesce((SELECT sealed = 0 AND segments = 2 FROM sift.worker_state('wbp_bm25')), false)"), cleanup=ASYNC_OFF, note='the promotion worker asked by the seal of the write buffer'),
    # sift_table: the writer seals buffers and asks the worker that settles them into columns
    Case('bgw_table_settle', g('bgw', 'table'), 'INSERT INTO mutable SELECT * FROM seed WHERE id <= 60000', kind='once', pre=(NO_WORKERS, 'TRUNCATE mutable') + ASYNC_ON, plan=False, par=True,
         after=wait_for("NOT EXISTS (SELECT FROM sift.table_segments('mutable') WHERE kind = 'sealed buffer')"), cleanup=ASYNC_OFF, note='the settle worker of a sift_table'),
]

# --- concurrent backends on the shared caches (opt-in group `conc`): the same read in 3 backends at once (loops of it each); instructions, hits and reads are summed over them
CONC_OF = {'agg_basic_five': (3, 8), 'c3_groups_label_count': (3, 8), 'acc_point_pk': (3, 200), 'bm25_top10_common': (3, 20), 'bm25_count_common': (3, 20),
           'col_wbuf_group': (3, 8), 'bm25_wbuf_count': (3, 8), 'storage_point_lookup': (3, 200)}

# --- the SQLite and MariaDB ports (groups `sqlite`, `mariadb`; ports.py): the case's `sql` is the mode of the driver
PORTS = [Case(f'{host}_{mode}', (host,), mode, src=host, kind='once', plan=False, note=f'{host} port: {mode}') for host, modes in PORT_MODES.items() for mode in modes]

# first statement after eviction: the cold-read path of each family
EVICT_OF = {'agg_basic_five': g('smoke', 'first-agg'), 'acc_point_pk': g('first-agg', 'first-col'), 'col_wbuf_group': g('first-col'), 'bm25_count_common': g('first-bm25'),
            'storage_point_lookup': g('first-table')}


def build(corpus_cases, catalog_cases):
    """The registry: list of Case with sql filled from the corpus parse and the catalog."""
    out = []
    for slug, groups in CORPUS:
        c = corpus_cases[slug]
        out.append(Case(slug, tuple(x for x in groups if x != 'first'), c['sql'], 'corpus', 'warm', tuple(c['pre']), plan=not c['pre']))
    for name, groups in CATALOG:
        c = catalog_cases[name]
        out.append(Case(name, tuple(x for x in groups if x != 'first'), c.sql, 'catalog', 'warm', plan=True))
    out += GATE + BGW + PORTS
    by = {c.id: c for c in out}
    for cid, groups in EVICT_OF.items():
        b = by[cid]
        out.append(replace(b, id=cid + '_evict', groups=groups, kind='evict', note='first statement after eviction'))
    for cid, (clients, loops) in CONC_OF.items():
        out.append(replace(by[cid], id='conc_' + cid, groups=('conc',), clients=clients, loops=loops, par=True, note=f'{clients} backends at once, {loops} executions each'))
    return out
