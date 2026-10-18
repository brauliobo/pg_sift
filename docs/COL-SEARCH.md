# Text search as the row selection of an aggregate (SiftAgg)

`SELECT <aggregates> FROM t WHERE <filters> AND col @@> 'text' GROUP BY ...` runs as three independent steps that meet on row keys:

1. the BM25 index (`sift`) evaluates the query to the **match set**: the keys (`blk << 16 | off`; virtual TIDs for a `sift_table`) of the rows that match, superblock by superblock, **without reading the heap**;
2. the column engine (`sift_col` index or `sift_table`) restricts each of its units to those keys, evaluates the filters on columns, and aggregates from columns only;
3. visibility is the one rule every SiftAgg scan already has: rows that survive the filters are vetted at the end (all-visible pages pass, the other pages are checked in the heap).

Files: `src/sift_search.[ch]` (planner extraction, cost, match-set iterator), `src/sift_aggsel.[ch]` (the rows of a unit a match set keeps), `src/sift_colsrc.h` (`select` provider callback), hooks in `src/sift_aggplan.c`, `src/sift_agg.c`, `src/sift_aggspec.[ch]`.

## What other engines do

| Engine | Text query | Doc values / columns | Combination |
|---|---|---|---|
| Lucene / Elasticsearch | the query is a `Scorer`/`DocIdSetIterator` over the postings of each segment | doc values (columnar, per segment) | a `Collector` pulls `nextDoc()` and reads the doc values of that doc id; filters are cached bitsets (roaring-like) intersected with the iterator; terms aggregations are a collector over doc values |
| ClickHouse | the `text` index is an inverted index; each token has a posting list of row numbers, lists are AND/OR-ed ([docs](https://clickhouse.com/docs/engines/table-engines/mergetree-family/textindexes), [blog](https://clickhouse.com/blog/clickhouse-full-text-search)) | MergeTree columns | the row-level result of the index (direct read) is a filter on the granule's rows; the other columns are read only for the surviving rows, the text column not at all |
| Pinot / Druid | inverted index (bitmap per dictionary id; Druid: roaring or concise bitmaps) | forward index per column | filter operators produce a bitmap (doc id set); aggregation operators iterate the set over the forward index |

The common design: an inverted index yields a **doc id set** in a compact ordered form (bitmap or iterator), the columnar side takes it as a **selection** and reads values only for selected doc ids. The doc id is segment-local and stable; deletes are a live-docs bitmap applied by the collector.

pg_sift maps this directly. The doc id is the heap key (`blk << 16 | off`), the segment is the superblock (256 heap pages), the doc id set is a `SiftFilter` (per-superblock page masks plus a 64-byte offset mask per page), and the live-docs bitmap is the MVCC vet.

## Interface

The minimal provider for another match source (`sift_match_bitmap`-like):

```c
/* the matches in the superblocks [lo, hi), valid until the next call; calls ascend, or the iteration starts again */
const SiftFilter *sift_search_range(SiftSearch *s, uint32 lo, uint32 hi);
```

A column provider opts in with `select` in `SiftColProvider` (`sift_colsrc_select(src, keep)`); it then calls `sift_aggsel_unit()` for each unit, which returns the number of rows the executor sees (or false when the unit has no kept row), and `sift_aggsel_read/live/vet()` for the rest. `sift_col`'s `sift_aggcol.c` and `sift_table`'s `sift_table_src.c` use it; a provider without `select` is never given a search (the planner checks `sift_colsrc_can_select`).

Executor contract of the match set:

- a **superset or exact** set of the visible matches of the snapshot: it may hold rows of dead or not yet visible tuples (the index is not MVCC-aware) and it must not miss a visible match;
- rows the index cannot decide (phrases are relaxed in the index) are decided on the heap text before they enter the set (`recheck_page`), the same rule as `SiftScan`/`SiftCount`; this is the only heap read of the search itself;
- the set is exact in its pages (no lossy pages), 64 bytes per page that has a match.

## Visibility semantics

The match set does not check visibility. SiftAgg already does: every unit's selection (live rows, after filters) goes through `sift_col_vet`, which passes pages that are all-visible in the visibility map and checks the others on the heap. A row that the BM25 index returns but is dead or invisible to the snapshot is dropped there; rows in the write buffer of the BM25 index (inserted, not yet merged) are covered by the iterator, and the column side treats them as it always does. Differential tests compare insert, update, delete, own-transaction rows, rollback and vacuum states against the normal plan (`test/sql/col_search.sql`, `test/col/fixtures/h_search_*.sql`).

A query with the search in the same transaction as writes sees the transaction's own rows because the vet reads the heap for pages that are not all-visible, and the iterator reads the write buffer.

## Planner

`sift_search_extract` runs in `sift_agg_upper_paths` after the shape matcher: from the base restrictions of the relation it takes the conjuncts `key @@> q` of **one** BM25 index of the table (single column; a partial index only when the query implies the predicate; not hypothetical), with `q` free of Vars, volatile functions and sub-plans (a constant, a statement parameter, a stable function of them). They leave the filter list and become `SiftAggSpec.search` (the index Oid and the query operands, serialized for parallel workers). Anything else keeps the old plan (the `@@>` conjunct is then an ordinary filter on the heap).

Cost (`sift_search_cost`), in operator costs:

```
bind (20000) + terms * 400 + postings * 0.25 + matching pages * 12
```

`docs` comes from `clauselist_selectivity` of the conjuncts (the BM25 estimator: df of the terms, the same one `SiftScan` uses), the number of postings/terms from the cached term statistics for constant queries. The scan then reads `min(rows * unitsFraction, docs * 8)` values per leaf in the units that have a match, instead of all of them. The result is compared with the normal plan by the planner as any other path; the plain plan is always generated and stays when cheaper (a query with few matches and no group by, `df = 0`, a table without a column index).

`EXPLAIN` shows `Search: orders_title @@> 'wireless headphones'`; `EXPLAIN ANALYZE` adds `Search Matches` and the existing unit counters (`units pruned` counts the units without a match).

## Executor

- The queries are bound to the index once per participant (`sift_plan_bind`); a NULL query matches nothing.
- The scan walks the units in morsels (`MORSEL_SBS = 2` superblocks). Before a morsel, `scan_range` asks the match set for the superblocks of a chunk (`sift.test_search_chunk_sbs`, default 32, so 32 * 64 B * pages in memory at most), the source restricts each unit with `sift_col_filter_tids` (rank-based and match-driven: a unit with 20 matches in 100k rows touches those 20 rows, not the unit).
- A unit with few kept rows (`count * 8 <= rows`) is returned **compacted**: the rows renumbered, each leaf decoded for those rows only (`sift_col_read_idx`, `sift_cchunk_decode_sel`). A unit with many keeps its numbering and its liveness mask is the kept rows.
- Float sums keep the lane order of `SIFT_AGG_LANES`; participants of a parallel scan each hold their own match set (iterator restarted when a lane's superblock order goes backwards).
- All aggregates, group keys, filters, lookup joins, grouping sets, generic aggregates, `HAVING`, parallel workers work unchanged: the search only changes which rows are alive in the unit.

## Facets and top-N

**Facets.** `SELECT category, count(*) FROM t WHERE text @@> q [AND filters] GROUP BY category ORDER BY 2 DESC LIMIT n` is this path: counts per category from the column, over the match set. It does what `sift.facets()` does for a keyword index, but with filters from columns and any other aggregate. Measured at 1M (`sift.facets` on a keyword index of `category` against the SiftAgg query, same results):

| Query | `sift.facets` | SiftAgg search |
|---|---|---|
| `wireless` (292k matches) | 7.5 ms | 4.6 ms |
| `wireless headphones` (7k) | 7.0 ms | 2.7 ms |
| `cookware` (9k) | 6.6 ms | 1.4 ms |

**Top-N.** `ORDER BY created_at DESC LIMIT n` with a search is **not** done by the column engine yet (see Limits): it still runs as `Limit -> Sort -> SiftScan` and fetches every match from the heap. `ORDER BY sift.score(...) DESC LIMIT n` is the ranked `SiftScan`, which already restricts itself to a filter mask (`sift_pushdown.c`, bitmap path over other indexes). The design for both: the same `select` seam in the opposite direction, `sift_colsrc_tids(src, sel)` giving the keys of the rows that pass column filters, converted to the `SiftFilter` the ranked scan ANDs into its candidates (so the filter comes from `sift_col` instead of a bitmap heap scan), and for order by a column a top-N collector over the match set with the key columns decoded for the kept rows and the heap read for the n winners only.

## Measured

Orders table of `bench/sql/store` (title BM25 index, `sift_col` over the report columns), 1M and 10M rows, one backend, `max_parallel_workers_per_gather = 0` unless stated, warm cache, pgbench latency; instructions are user space of the backend (`perf stat -p`) per execution. Before is `sift.enable_agg = off` (the plan without the feature: heap plan with `SiftScan` and a hash aggregate); the planner's default choice is shown where it differs.

Bench question 09 (`status = 'shipped' AND amount >= 50 AND title @@> 'wireless headphones' GROUP BY category`, top 10):

| Rows | Before | After | Speedup | Instructions before | after | Default plan |
|---|---|---|---|---|---|---|
| 1M | 6.4 ms | 2.1 ms | 3.1x | 21.6 M | 11.6 M | 1.8 ms |
| 10M | 60.6 ms | 23.3 ms | 2.6x | 213.7 M | 116.1 M | 21.3 ms |

Bench question 10 (`ORDER BY created_at DESC, id DESC LIMIT 10`) is unchanged by this work: 6.0 ms at 1M, 52.8 ms at 10M (216 M instructions).

Selectivity at 1M (`... AND status = 'shipped' GROUP BY category`, serial):

| Query | Matches | After | Before |
|---|---|---|---|
| `cookware` | 9,371 | 1.2 ms | 5.7 ms |
| `detergent` | 4,235 | 0.9 ms | 1.5 ms |
| `wireless headphones` | 7,143 | 1.4 ms | 5.7 ms |
| `wireless` | 291,928 | 5.8 ms | 81.7 ms |
| `portable OR compact` | 217,481 | 5.5 ms | 69.3 ms |
| `smart OR classic OR ultra` | 194,659 | 6.6 ms | 59.0 ms |

The gain grows with the match count because the heap plan pays a heap fetch per match and the search path pays a posting decode plus a column read per match. With few matches the fixed cost (binding the queries, about 60 us; walking the units' headers) dominates and the gain is smaller. A profile at 10M shows the time in whole-chunk reads of the column (`sift_sr_get`, about 40%), the BM25 iteration (about 27%) and per-unit setup; a sparse unit read by position (`SEL_SPARSE`) costs about 8 dense values, which the cost model uses.

Worker start-up is about 6.5 ms at 10M, so parallel plans only win when the serial scan is well above that; the cost model chooses serial at 1M and 4 workers at 10M when `parallel_setup_cost` is low enough.

## Tests

- `test/sql/col_search.sql` (pg_regress): a 24,000-row table with wide rows (many pages), a BM25 index and a `sift_col` index with a small mutable limit; about 26 queries (term, and, or, not, phrase, negated phrase, prefix, wildcard, regex, fuzzy, boost, no match, NULL query, parameters, filters, global aggregate, float sums, nulls, `HAVING`, expression keys, generic aggregates, `IN`, two search conjuncts, a function index) compared with the plan without SiftAgg (`EXCEPT ALL` both ways) in the stages build, chunks of one superblock, parameters, counter check, forced plan, insert, update, delete, own transaction, rollback, vacuum, column-index promotion, parallel workers, and `sift_table`. Tie order of ranked results is not touched: no ranked path changed.
- `test/col/fixtures/h_search_*.sql` (coldiff): term, boolean, phrase, prefix, fuzzy, global, no match, two conjuncts, `HAVING`, parameter, all with the DML scenarios (insert, update, delete, abort, vacuum, HOT, TID recycle, held transactions, reindex, promotion) and the five variants (parallel 0/2, SIMD scalar/auto).

## Limits

- Only top-level `AND` conjuncts `col @@> q` take part; an `OR` between a search and another predicate (or two searches) keeps the normal plan. Write several terms as one query (`'a OR b'`).
- One BM25 index per search; several conjuncts on the same index are all evaluated by it, conjuncts on two different indexes are not extracted.
- Phrase matches that the index leaves undecided need their text from the heap (`recheck_page`): phrase-heavy queries read the heap for the undecided pages only.
- Each participant of a parallel scan evaluates the query for its own superblocks (the set is not shared), so the posting decode is not divided by the worker start-up cost.
- Top-N on a column order is not implemented (see above); `sift.facets()` on keyword indexes is unchanged.
- Units are skipped only when none of their keys is in the match set; term statistics are not used to skip units.

## Hooks in files owned by other work

- `src/sift_aggplan.c`: `AggCtx.search` and the call `sift_search_extract(...)` after `agg_spec_new` in `sift_agg_upper_paths`; `agg_cost` adds `sift_search_cost`; `agg_finish` copies the spec; `spec_deps` records the index dependency.
- `src/sift_aggspec.[ch]`: `Node *search` in `SiftAggSpec`, packed last in `custom_private` (`put_node`/`get_node`).
- `src/sift_agg.c`: open/close of the match set in `engine_open/close`, chunking in `scan_range`, EXPLAIN lines, worker counters.
- `src/sift_colsrc.h`, `src/sift_aggsrc.c`: the optional `select` provider callback.
- `src/sift_col_scan.c`, `src/sift_col_enc.[ch]`, `src/sift_col.h`: `sift_col_filter_tids` (match-driven), `sift_col_read_idx`, `sift_col_unit_allvisible`, sparse page masks, a faster `sift_col_vet`; `src/sift_filter.[ch]`: `sift_filter_create/reset/add_page`.
- BM25 side: only the existing iterator entry points (`sift_plan_bind`, `sift_iter_*`, `sift_plan_recheck`, `sift_vis_fetch`, `sift_estimate_cached`); `feat/v3-bm25fmt` has not landed, so nothing in `sift_scan.c`, `sift_eval.c` or `sift_segment.c` changed.
