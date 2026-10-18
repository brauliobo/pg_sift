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

**Top-N.** `WHERE col @@> q [AND filters] ORDER BY <columns> [ASC|DESC] LIMIT n [OFFSET m]` is a SiftAgg path too (`sift.enable_topn`, default on, needs `sift.enable_agg`). The planner adds it at `UPPERREL_ORDERED` (`topn_paths`, `src/sift_aggplan.c`) when the query has no groups, aggregates, windows, `DISTINCT`, joins or row marks, `LIMIT`/`OFFSET` are constants with `n + m <= 100000`, and every sort expression is a column of the `sift_col` index (int2/int4/int8, date, timestamp(tz), numeric, float4/float8, default btree order, any `ASC`/`DESC`/`NULLS` combination; NaN is the largest). The scan evaluates filters and the match set as for an aggregate; per unit the rows that pass go to a bounded heap (`src/sift_topn.c`) of the `n + m` best, ties broken by TID. Only the first sort key is decoded for all passing rows; the other keys and the TIDs are read (`read_rows`, `tids`) for the rows that beat the heap's worst, so they are read for a handful of rows. At the end the heap rows are fetched from the heap by TID in order (virtual slot of the scan tuple): the heap is read `n + m` times, whatever the number of matches. The `Limit` node stays above the scan.

Early stop uses the zone maps (`zone` provider op: min, max, nulls of the first sort key per unit). Before the scan six superblocks spread over the table are probed to tell whether the first key follows the table order (a clustered or monotonic column such as `created_at` or a serial `id`); if so the superblocks are visited from the end its best values are at, in windows that start at one superblock and double up to eight (the best superblock first: it usually fills the heap and the rest are skipped by their zones), and the scan stops at the first window whose best superblock cannot beat the heap's worst. The match set is asked for exactly the first window (a match set costs by the superblocks it covers), and for the later windows in chunks of `sift.test_search_chunk_sbs`, because the match iterator restarts when asked for superblocks it has passed. For columns that do not follow the table every unit is still skipped when its zone cannot beat the worst row, and nothing else changes. `EXPLAIN` shows `Top-N: <keys> (limit n)`, `Superblocks Skipped By Top-N` and `Heap Fetches`. The cost model scales the scan by the fraction a correlated column needs (`pg_statistic` correlation >= 0.9), except the opening of the match set (the iterator takes the posting lists of the terms whole: `sift_search_open_cost`), and adds `n` random heap fetches. Against an ordered btree on the sort column (`Index Scan Backward` with the search as its filter) PostgreSQL compares the two estimates: an uncorrelated column with a btree keeps the btree plan (`created_at` below), a rare search takes the top-N (`test/sql/col_topn.sql` checks both plans). The btree plan is priced at the declared cost of `sift.match` (COST 1000, 2.5 per row examined), which makes it about 5000 for an index scan that takes 0.5 ms; at COST 100 (what a row costs to test) the estimate is 565 and the btree wins the `id` case below. The top-N estimate (about 1600 at 10M) is near its time, so the declared cost is the only term left to change; it is the BM25 side and was not touched.

`ORDER BY sift.score(...) DESC LIMIT n` is the ranked `SiftScan`, unchanged.

## Measured

Orders table of `bench/sql/store` (title BM25 index, `sift_col` over the report columns), 1M and 10M rows, one backend, `max_parallel_workers_per_gather = 0` unless stated, warm cache, pgbench latency; instructions are user space of the backend (`perf stat -p`) per execution. Before is `sift.enable_agg = off` (the plan without the feature: heap plan with `SiftScan` and a hash aggregate); the planner's default choice is shown where it differs.

Bench question 09 (`status = 'shipped' AND amount >= 50 AND title @@> 'wireless headphones' GROUP BY category`, top 10):

| Rows | Before | After | Speedup | Instructions before | after | Default plan |
|---|---|---|---|---|---|---|
| 1M | 6.4 ms | 2.1 ms | 3.1x | 21.6 M | 11.6 M | 1.8 ms |
| 10M | 60.6 ms | 23.3 ms | 2.6x | 213.7 M | 116.1 M | 21.3 ms |

Top-N, same table and conditions (`title @@> 'wireless headphones'` unless noted), serial, warm; before is `sift.enable_topn = off` (`Limit -> Sort -> SiftScan`). Bench question 10 is the first row (the earlier measurement of this cluster for the sort plan was 6.0 ms at 1M and 52.8 ms at 10M with another build of the data; the column "Before" is measured now on the same table):

| Query | 1M before | 1M after | 10M before | 10M after |
|---|---|---|---|---|
| `ORDER BY created_at DESC, id DESC LIMIT 10` (Q10) | 7.1 ms | 2.4 ms | 191 ms | 19.5 ms |
| `ORDER BY created_at, id LIMIT 10` | 7.5 ms | 3.1 ms | 288 ms | 24.5 ms |
| `... LIMIT 100` (Q10 shape) | 6.0 ms | 3.1 ms | 189 ms | 22.0 ms |
| `ORDER BY amount DESC, id LIMIT 20` | 101 ms | 6.7 ms | 842 ms | 38.2 ms |
| `ORDER BY customer_id, id LIMIT 10` | 18.9 ms | 4.2 ms | 487 ms | 34.1 ms |
| Q10 shape, 292k matches (`wireless`) | 96 ms | 5.1 ms | 785 ms | 29.1 ms |
| Q10 shape, no search, filters only | 91.7 ms | 4.2 ms | 1196 ms | 19.5 ms |
| `ORDER BY id DESC LIMIT 10` (id follows the table) | 0.41 ms | 0.32 ms | 11 ms (default plan) | 0.74-1.1 ms |

Against ordered btrees (pgbench latency, serial; orders of 1M and 10M rows with random titles of three words from thirty, a btree on `id` and on `created_at`, `title @@> 'wireless headphones'`, about 0.5% of the rows; host CPU pressure 4-6%, I/O 0% before and after each batch). "Before" is `sift.enable_topn = off`, which leaves the btree plan where there is one:

| Query | 1M before | 1M after | 10M before | 10M after |
|---|---|---|---|---|
| `ORDER BY id DESC LIMIT 10` | 0.47 ms | 0.28 ms | 0.57 ms | 0.71 ms |
| `ORDER BY created_at DESC LIMIT 10` (btree plan kept) | 2.7 ms | 1.3 ms | 1.34 ms | 1.55 ms |
| `ORDER BY amount DESC, id LIMIT 20` | 1.3 ms | 1.4 ms | 26.4 ms | 10.6 ms |
| `ORDER BY customer_id, id LIMIT 10` | 1.2 ms | 1.0 ms | 22.8 ms | 10.2 ms |

At 10M the top-N on `id` is 1.25x slower than the btree (0.7 against 0.57 ms, planning included in both): opening the two posting lists costs the top-N a copy of their bytes, which the btree scan of 2,000 rows does not pay. The same query measured 2.0 ms before the visit order changed (the best superblock now goes first and the scan reads one unit instead of eight). Where the btree plan stays (`created_at`), the two columns differ by noise.

`created_at` is not ordered like the table in this data, so Q10 reads all units whose zone can beat the heap; the gain is the avoided heap fetches and sort. A column that follows the table (`id`) stops after the first window: 2 units of 496 superblocks read at 1M, 8 units at 10M.


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

- `test/sql/col_topn.sql` (pg_regress) and `test/col/fixtures/h_topn_*.sql` (coldiff): about 33 queries (search, filters, `ASC`/`DESC`, nulls first/last, float and numeric NaN, two and three keys, offset, parameters, phrase, prefix, fuzzy, boolean; text key, `OR` and `sift_table` stay plain) compared with the sort plan in the same stages and scenarios as above.
- `test/sql/col_search.sql` (pg_regress): a 24,000-row table with wide rows (many pages), a BM25 index and a `sift_col` index with a small mutable limit; about 26 queries (term, and, or, not, phrase, negated phrase, prefix, wildcard, regex, fuzzy, boost, no match, NULL query, parameters, filters, global aggregate, float sums, nulls, `HAVING`, expression keys, generic aggregates, `IN`, two search conjuncts, a function index) compared with the plan without SiftAgg (`EXCEPT ALL` both ways) in the stages build, chunks of one superblock, parameters, counter check, forced plan, insert, update, delete, own transaction, rollback, vacuum, column-index promotion, parallel workers, and `sift_table`. Tie order of ranked results is not touched: no ranked path changed.
- `test/col/fixtures/h_search_*.sql` (coldiff): term, boolean, phrase, prefix, fuzzy, global, no match, two conjuncts, `HAVING`, parameter, all with the DML scenarios (insert, update, delete, abort, vacuum, HOT, TID recycle, held transactions, reindex, promotion) and the five variants (parallel 0/2, SIMD scalar/auto).

## Limits

- Only top-level `AND` conjuncts `col @@> q` take part; an `OR` between a search and another predicate (or two searches) keeps the normal plan. Write several terms as one query (`'a OR b'`).
- One BM25 index per search; several conjuncts on the same index are all evaluated by it, conjuncts on two different indexes are not extracted.
- Phrase matches that the index leaves undecided need their text from the heap (`recheck_page`): phrase-heavy queries read the heap for the undecided pages only.
- Each participant of a parallel scan evaluates the query for its own superblocks (the set is not shared), so the posting decode is not divided by the worker start-up cost.
- Filters that the source applies on encoded values (`docs/COL-PLAN.md`, "Filters in the source") are not used in a scan with a search: the rows of a match set may be renumbered, so the executor tests them.
- Top-N: only `sift_col` (not `sift_table`, whose source has no TIDs or zones), serial scans only, sort keys must be plain columns of the index with the numeric/date types above (text keys, expressions, `sift.score` mixed with columns keep the normal plan), no `OR` of searches, `n + m <= 100000`, and the tie-break column (usually `id`) must be in the `sift_col` index. `sift.facets()` on keyword indexes is unchanged.
- Units are skipped only when none of their keys is in the match set; term statistics are not used to skip units.

## Hooks in files owned by other work

- `src/sift_aggplan.c`: `AggCtx.search` and the call `sift_search_extract(...)` after `agg_spec_new` in `sift_agg_upper_paths`; `agg_cost` adds `sift_search_cost`; `agg_finish` copies the spec; `spec_deps` records the index dependency.
- `src/sift_aggspec.[ch]`: `Node *search` in `SiftAggSpec`, packed last in `custom_private` (`put_node`/`get_node`).
- `src/sift_agg.c`: open/close of the match set in `engine_open/close`, chunking in `scan_range`, EXPLAIN lines, worker counters.
- `src/sift_colsrc.h`, `src/sift_aggsrc.c`, `src/sift_aggcol.c`: the optional `select` provider callback, and for top-N the `tids`, `zone` and `read_rows` ops (`SiftSrcZone`).
- Top-N: `src/sift_aggplan.c` (`spec_finish` split out of `agg_finish`, `topn_paths`, the `UPPERREL_ORDERED` dispatch, heap-fetch and correlation terms in `agg_cost`), `src/sift_aggspec.[ch]` (`topn`, `desc_topn`), `src/sift_agg.c` (`topn_run`/`topn_unit`/`topn_next`, hooks in `process_unit`, `engine_open/close`, EXPLAIN, GUC `sift.enable_topn`), `src/sift_colcodec.c` (`sift_cc_decode_sel` goes dense earlier for delta-coded nodes), `src/sift_col_enc.[ch]`, `src/sift_col_scan.c`, `src/sift_col.h` (`sift_cunit_row_keys`, `sift_col_row_tids`).
- `src/sift_col_scan.c`, `src/sift_col_enc.[ch]`, `src/sift_col.h`: `sift_col_filter_tids` (match-driven), `sift_col_read_idx`, `sift_col_unit_allvisible`, sparse page masks, a faster `sift_col_vet`; `src/sift_filter.[ch]`: `sift_filter_create/reset/add_page`.
- BM25 side: only the existing iterator entry points (`sift_plan_bind`, `sift_iter_*`, `sift_plan_recheck`, `sift_vis_fetch`, `sift_estimate_cached`); `feat/v3-bm25fmt` has not landed, so nothing in `sift_scan.c`, `sift_eval.c` or `sift_segment.c` changed.
