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


@dataclass(frozen=True)
class Case:
    id: str
    groups: tuple
    sql: str = ''
    src: str = 'gate'          # corpus | catalog | gate
    kind: str = 'warm'
    pre: tuple = ()
    post: tuple = ()
    plan: bool = True          # capture EXPLAIN (COSTS OFF) of the statement and require the same plan on both sides
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

GATE = [
    # float column evaluated per bucket of its values (grouping by an expression of a float8 column)
    Case('agg_group_float_floor', g('agg-kernel', 'agg-read'), 'SELECT floor(amount / 5)::int AS k, count(*), sum(qty) FROM events WHERE amount BETWEEN -1e9 AND 1e9 GROUP BY 1'),
    Case('agg_group_float_cast', g('agg-kernel'), 'SELECT (amount / 3)::int AS k, count(*), min(qty), max(qty) FROM events GROUP BY 1'),
    Case('agg_group_float_round_avg', g('agg-kernel'), 'SELECT round(amount) + 0.0 AS k, count(*), avg(score) FROM events GROUP BY 1'),
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
    Case('col_wbuf_insert', g('col-wbuf', 'smoke'), "INSERT INTO wbp SELECT i, i % 16, i % 1000, 'common w' || i % 97 FROM generate_series(1, 3000) i", kind='once',
         pre=('TRUNCATE wbp',), plan=False),
    Case('col_promote', g('col-maint', 'col-wbuf'), "SELECT sift.col_promote('wbp_col') >= 0", kind='once', pre=FILL_WBP, plan=False),
    Case('col_build', g('col-build'), 'CREATE INDEX cb_col ON cb USING sift_col (id, g, g2, v, x, t)', kind='once', pre=('DROP INDEX IF EXISTS cb_col',), post=('DROP INDEX cb_col',), plan=False),
    Case('col_reindex', g('col-build', 'col-maint'), 'REINDEX INDEX docs_col', kind='once', plan=False),
    Case('col_merge', g('col-maint'), "SELECT sift.col_merge('mt_col', true)", kind='once', pre=FILL_MT, plan=False),
    Case('col_vacuum', g('col-maint'), 'VACUUM mt', kind='once', pre=FILL_MT + ('DELETE FROM mt WHERE id % 10 = 3',), plan=False),
    # table
    Case('table_insert', g('table'), 'INSERT INTO mutable SELECT * FROM seed WHERE id <= 3000', kind='once', pre=('TRUNCATE mutable',), plan=False),
    Case('table_update', g('table'), 'UPDATE mutable SET amount = amount + 1 WHERE id BETWEEN 100 AND 200', kind='once', pre=RESET, plan=False),
    Case('table_vacuum', g('table'), 'VACUUM (DISABLE_PAGE_SKIPPING) mutable', kind='once', pre=RESET + ('DELETE FROM mutable WHERE id > 2048',), plan=False),
]

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
    out += GATE
    by = {c.id: c for c in out}
    for cid, groups in EVICT_OF.items():
        b = by[cid]
        out.append(replace(b, id=cid + '_evict', groups=groups, kind='evict', note='first statement after eviction'))
    return out
