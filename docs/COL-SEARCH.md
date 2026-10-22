# Text search as the row selection of an aggregate (SiftAgg)

`SELECT <aggregates> FROM t WHERE <filters> AND col @@> 'text' GROUP BY ...` runs as three independent steps that meet on row keys:

1. the BM25 index (`sift`) evaluates the query to the **match set**: the keys (`blk << 16 | off`; virtual TIDs for a `sift_table`) of the rows that match, superblock by superblock, **without reading the heap**;
2. the column engine (`sift_col` index or `sift_table`) restricts each of its units to those keys, evaluates the filters on columns, and aggregates from columns only;
3. visibility is the one rule every SiftAgg scan already has: rows that survive the filters are vetted at the end (all-visible pages pass, the other pages are checked in the heap).

Files: `src/agg/search.[ch]` (planner extraction, cost, match-set iterator), `src/agg/sel.[ch]` (the rows of a unit a match set keeps), `src/agg/colsrc.h` (`select` provider callback), hooks in `src/agg/plan.c`, `src/agg/agg.c`, `src/agg/spec.[ch]`.

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

A column provider opts in with `select` in `SiftColProvider` (`sift_colsrc_select(src, keep)`); it then calls `sift_aggsel_unit()` for each unit, which returns the number of rows the executor sees (or false when the unit has no kept row), and `sift_aggsel_read/live/vet()` for the rest. `sift_col`'s `agg/col.c` and `sift_table`'s `table/src.c` use it; a provider without `select` is never given a search (the planner checks `sift_colsrc_can_select`).

Executor contract of the match set:

- a **superset or exact** set of the visible matches of the snapshot: it may hold rows of dead or not yet visible tuples (the index is not MVCC-aware) and it must not miss a visible match;
- rows the index cannot decide (phrases are relaxed in the index) are decided on the heap text before they enter the set (`recheck_page`), the same rule as `SiftScan`/`SiftCount`; this is the only heap read of the search itself;
- the set is exact in its pages (no lossy pages), 64 bytes per page that has a match.

## Visibility semantics

The match set does not check visibility. SiftAgg already does: every unit's selection (live rows, after filters) goes through `sift_col_vet`, which passes pages that are all-visible in the visibility map and checks the others on the heap. A row that the BM25 index returns but is dead or invisible to the snapshot is dropped there; rows in the write buffer of the BM25 index (inserted, not yet merged) are covered by the iterator, and the column side treats them as it always does. Differential tests compare insert, update, delete, own-transaction rows, rollback and vacuum states against the normal plan (`test/sql/col_search.sql`, `test/col/fixtures/h_search_*.sql`).

The match set is read a chunk of superblocks ahead of the vet, and a `VACUUM` marks pages all-visible only after it has cleared the index entries of their dead rows, so the vet of a scan whose column index was changed since the scan read it (its directory version) takes no page as all-visible and checks the rows in the heap (test `vacuum_agg_snapshot`). The column index is the one whose liveness decides the rows: a dead row the match set still holds is not among them.

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

**Top-N.** `WHERE col @@> q [AND filters] ORDER BY <columns> [ASC|DESC] LIMIT n [OFFSET m]` is a SiftAgg path too (`sift.enable_topn`, default on, needs `sift.enable_agg`). The planner adds it at `UPPERREL_ORDERED` (`topn_paths`, `src/agg/plan_order.c`) when the query has no groups, aggregates, windows, `DISTINCT`, joins or row marks, `LIMIT`/`OFFSET` are constants or parameters of the statement (see below), and every sort expression is a column of the `sift_col` index or one of the expressions below (bool, int2/int4/int8, date, time, timestamp(tz), numeric(p <= 18), money, float4/float8, default btree order, any `ASC`/`DESC`/`NULLS` combination; NaN is the largest). A text, varchar, uuid, bytea or inet key is the entry of the unit's dictionary, compared as the sort operator of the query compares (`varstr_cmp` under the query's collation for text: byte order under `C`, `strcoll` and then byte order under any deterministic collation of the C library or ICU; `uuid_cmp`, `byteacmp` and the comparison function of inet), so the rows and their order are the normal plan's; a nondeterministic collation, a `char(n)`, `name` or `cidr` column and an operator class other than the type's default (`text_pattern_ops`) keep the normal plan (`topn_shape`, with the reason). **Sort expressions.** `coalesce(<int2, int4, int8, date, timestamp or timestamptz column>, <constant of the same type>)`, `lower(<text or varchar column>)` and `upper(<text or varchar column>)` are keys too, with any direction and NULLs clause. PostgreSQL evaluates a sort expression for every row that passes the `WHERE`, so the scan does too: a NULL becomes the constant, and the string goes through PostgreSQL's own `lower()` or `upper()` under the call's collation, once per distinct entry of a unit; the results are compared by the sort operator under the collation of the expression. A `coalesce` key keeps the zones: the zone of a unit is widened to the constant when it holds NULLs (a unit of NULLs only is the constant), so units are skipped and the walk and the parallel bound work as for the column. **Arithmetic and casts.** Any other immutable expression over the integer, float, numeric (18 digits or fewer), date, time, timestamp, text and uuid columns of the index is a key as well: `a + 1`, `1000 - a`, `-a`, `a * b`, `total / 3`, `a % 7`, `a::bigint`, `a::text`, `d + 30`, `t::date`, a comparison. The scan runs the expression with the backend's own expression machinery (`ExecInitExpr`, with the leaves of the unit as the columns of a virtual slot) for every row that passes the `WHERE` and the late conjuncts, and keeps the results as the vector of the key, so the heap, the sort of every row and the parallel merge are the ones of a column. The operators, casts and comparison are PostgreSQL's, hence its overflow (`integer out of range`), division by zero, NULL and NaN behavior; a query that raises raises the same SQLSTATE in both plans (the message can name another row, and which of several rows raises first is not promised). No zone bounds an expression, so a unit is read whenever it holds matching rows (the heap bar is a value of the expression); a sorted run of a column is not used for it either. The planner prices the per-row evaluation (`TOPN_EXPR_ROW`, the expression's own cost, a charge per numeric or text value) so that a shape the scan would run slower than the plan's sort keeps the plan. Such a key of strings has no zones: a first key of strings reads every unit that holds matching rows, compares each row with the n-th best (a dictionary entry once per unit, then it is remembered) and keeps its own copy of the strings of the heap rows; a heap whose keys would not fit `work_mem` is planned as the sort of every row (below), which the scan runs as a tuplesort of the strings under the query's operator and collation. The scan evaluates filters and the match set as for an aggregate; per unit the rows that pass go to a bounded heap (`src/agg/topn.c`) of the `n + m` best, ties broken by TID. Only the first sort key is decoded for all passing rows; the other keys and the TIDs are read (`read_rows`, `tids`) for the rows that beat the heap's worst, so they are read for a handful of rows. At the end the heap rows are fetched from the heap by TID in order (virtual slot of the scan tuple): the heap is read `n + m` times, whatever the number of matches. The `Limit` node stays above the scan.

**Parameters and ORs.** `LIMIT $1 [OFFSET $2]` of a generic plan (`plan_cache_mode = force_generic_plan`, or the fifth execution of a prepared statement) is accepted: the plan is priced for an assumed 1000 rows, the scan evaluates the two expressions (no sub-plan or volatile function) when it starts, and answers any value the way the `Limit` node above reads it. Up to 100000 rows and 4 sort keys the heap is the one above; `NULL` (all rows), a sum past the cap, an overflow or more than 4 sort keys keeps every row that passes as its keys and TID (an array in memory up to `hash_mem`, then a `Tuplesort` at `work_mem` that writes runs to disk), sorts them, and still fetches from the heap only the rows the `Limit` takes; zero or negative reads nothing (the `Limit` node raises its own error for a negative value). **Deep pages.** A constant `n + m` past 100000, more than 4 sort keys, or an `n + m` past 1024 whose heap costs more than the sort is planned as a sort of every row that passes (`Top-N: .. (limit N, sorted)`, priced by `topn_sort_cost` with the table read whole, and with the heap's cost for a heap of many rows); the planner takes it by cost against PostgreSQL's own sort. When the output columns are plain columns of the table the first `m` rows, which the `Limit` throws away, are handed to it as rows of NULLs and are not read from the heap (`Heap Fetches` is `n`, not `n + m`). `EXPLAIN` shows `limit by parameter`, and `EXPLAIN ANALYZE` adds `Top-N Sorted Rows` when the sort was used. The limits are in `docs/LIMITS.md` (`topn_rows`, `topn_keys`); `col_topn_deep` and `col_topn_deep_limits` test them. `WHERE key @@> a OR key @@> b ...` (every branch a match of the same BM25 index, a constant or a parameter) is a top-N too: each arm is bound as its own query (so its syntax errors, its `NOT` rule and its phrase rechecks are those of the normal plan) and the match set is the union of the arms' pages, `OR`-ed per heap page. A search ORed with a column predicate, an OR next to another match, or a branch that is an `AND` stays normal; `sift.score()` is not available here. The aggregate scans do not take an OR of searches yet.

Early stop uses the zone maps (`zone` provider op: min, max, nulls of the first sort key per unit). Before the scan six superblocks spread over the table are probed to tell whether the first key follows the table order (a clustered or monotonic column such as `created_at` or a serial `id`); if so the superblocks are visited from the end its best values are at, in windows that start at one superblock and double up to eight (the best superblock first: it usually fills the heap and the rest are skipped by their zones), and a window is read only when the zones of its units, taken together from the segment headers (`zone_range` provider op: no unit is opened), can beat the heap's worst: the probes choose the order, not the end, because the new version of an updated row is stored wherever there is room and may hold a value far from its neighbours (`col_topn_moved`). The match set is asked for exactly the first window (a match set costs by the superblocks it covers), and for the later windows in chunks of `sift.test_search_chunk_sbs`, because the match iterator restarts when asked for superblocks it has passed. For a column that does not follow the table (blocks of values in no order, random) and a scan without a search, the zones of all the superblocks are read first (segment headers, no unit opened), the superblocks are sorted by the best row their zone can hold (NULLs first under `NULLS FIRST`, a zone that is not exact first, the superblocks that hold only NULLs last under `NULLS LAST`) and visited in that order; the scan ends at the first superblock whose zone cannot beat the n-th row (`sift.topn_walk`, default on; `off` visits them in table order). When the first key ties between units (a few statuses, a day number) the walk is decided by the keys after it: the zones of the integer keys that follow bound each superblock in key order, so a superblock whose zone of the first key ties with the n-th row is left out when the zone of the next key cannot beat the n-th either (`ORDER BY status, id LIMIT 10` reads the units that hold the first ids of the first status, `sift.topn_zone_keys`). The rows it returns do not depend on the order: a superblock is left out only when no row in it can sort before the n-th, and a zone equal to the n-th is kept for the other keys and the TID. A scan with a search keeps the table order, because the match set restarts when it is asked for superblocks it has passed. `EXPLAIN` shows `Top-N: <keys> (limit n)`, `Superblocks Skipped By Top-N` and `Heap Fetches`. The cost model scales the scan by the fraction a correlated column needs (`pg_statistic` correlation >= 0.9), or, for a column that is walked by its zones, by the mean width of a zone plus `n` over the rows expected to match, except the opening of the match set (the iterator takes the posting lists of the terms whole: `sift_search_open_cost`), and adds `n` random heap fetches. Against an ordered btree on the sort column (`Index Scan Backward` with the search as its filter) PostgreSQL compares the two estimates: an uncorrelated column with a btree keeps the btree plan (`created_at` below), a rare search takes the top-N (`test/sql/col_topn.sql` checks both plans). The btree plan is priced at the declared cost of `sift.match` (COST 1000, 2.5 per row examined), which makes it about 5000 for an index scan that takes 0.5 ms; at COST 100 (what a row costs to test) the estimate is 565 and the btree wins the `id` case below. The top-N estimate (about 1600 at 10M) is near its time, so the declared cost is the only term left to change; it is the BM25 side and was not touched.

**Parallel top-N.** With `max_parallel_workers_per_gather` above 0 the scan may plan workers (`Workers Planned` / `Workers Launched`), as the aggregate scans do: the leader decides the visit order (the probes and the walk above), the workers and the leader take windows of superblocks from a shared counter (one superblock, or the chunk of a match set), and each keeps the heap of its best `n + m` rows. They share the first-key rank of the n-th row of any heap (one atomic in the shared memory, tightened whenever a participant's heap is full and its worst row has a value): a unit whose zone, and a row whose key, sorts strictly after it is left out, and a window the walk ordered by its best bound ends the walk for everyone. A participant takes a superblock of the walk only when its zones of the leading sort keys (the first, and the integer keys after it, `sift.topn_zone_keys`) can beat the n-th row of its own heap, key by key, and the first key's zone the shared rank: a first key that ties between units is decided by the next, as in the serial walk. A worker sends the keys and TIDs of its heap, not rows; the leader merges them by the keys and the TID, so the rows, their order and the tie-break are those of one process, and fetches the `n + m` rows of the result from the heap. Dirty units are read again from the heap by the worker that owns the item, under the snapshot of the statement. Not parallel: a `LIMIT` that is a parameter past the cap, `NULL` or negative (the scan sorts), and a node under a worker. Workers start at 4 to 15 ms, so the planner prices them as the aggregate scans are (`parallel_setup_cost` times `AGG_SETUP_FACTOR`) and the plans of a few milliseconds stay serial: 200,000 rows, a filter evaluated row by row (`(k % 7) * (a % 13) + (g * 3) % 11 > 40 AND ..`) and `ORDER BY` a random key `LIMIT 1000`, with the workers forced (`parallel_setup_cost = 0`): 63 ms without workers, 32 with 2, 21 with 4 (100,000 rows: 25, 17, 16; 50,000 rows: 14, 16, 14; 25,000 rows: 9, 11, 17); the same plans of 1 to 3 ms (an index-ordered key, a selective filter, `LIMIT 100` of a random key) take 10 to 19 ms with workers forced. The cost of an expression filter is still underestimated, so the planner keeps the first of these serial at its default costs.

**Joins.** `ORDER BY f.key LIMIT n` over `fact f JOIN dim d ON d.k = f.k WHERE <conditions of d>` is a top-N of the fact table when the dimension only filters: the rows of `dim` that pass their conditions are read when the scan starts and their keys become `f.k IN (keys)`, a conjunct of the scan (the join filter of the aggregate scans, `join_filters` in `agg/init_table.c`; the units whose zone holds none of the keys are not read), which the heap of the best rows then holds exactly. An `IN (SELECT ..)` or `EXISTS (..)` that PostgreSQL pulls up into a semi join is the same filter and needs no unique key (a semi join matches a row once). Otherwise that needs the dimension to have a unique index on `k` (a row of the scan matches once), the join to be an inner one on one `int2/int4/int8/date` column, the ON clause and WHERE to read the dimension's columns alone, and the select list and `ORDER BY` to read the fact table's; a non-unique key of an inner join, an outer or anti join (`NOT EXISTS`), a comparison with a scalar sub-select, a text key, an expression of the key, a condition that reads the scanned row, a column of the dimension in the result or a second table that is not a filter of this kind keeps the normal plan (`topn_shape` in `sift.explain_refusals`). The scan runs in one process. Normal plan against the top-N, 200,000 rows, `LIMIT 10`: 88 ms against 0.5 ms for half the keys, 71 ms against 2 ms for one key (`test/sql/col_topn_join.sql`).

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

- `test/sql/col_topn.sql` (pg_regress) and `test/col/fixtures/h_topn_*.sql` (coldiff): about 33 queries (search, filters, `ASC`/`DESC`, nulls first/last, float and numeric NaN, two and three keys, offset, parameters, phrase, prefix, fuzzy, boolean, a text key; `OR` and `sift_table` stay plain) compared with the sort plan in the same stages and scenarios as above.
- `test/sql/col_topn_text.sql` (pg_regress) and `test/col/fixtures/topn_key_*.sql` (coldiff): text under `C` and a locale collation, varchar, uuid, bytea and inet keys, `ASC`/`DESC`, `NULLS`, ties, empty, multi-byte and long strings, two to five keys, the heap, the sort of every row (a deep page, keys wider than `work_mem`, a spilling tuplesort), workers, rows of the write buffer and deleted and updated rows, the named refusals and a leaf with overflow values, each against the plain plan.
- `test/sql/col_topn_order.sql` (pg_regress): keys that follow the table, lie in blocks of values, are random or hold ties and NULLs; a differential fuzz of 480 queries from a fixed seed (both directions, `NULLS` placements, ties with and without a tie-break, offsets, filters) against the plain plan after the build, deletes and moved rows, rows in the write buffer and the promotion. `bench/topn_order.py` times the top-N and the sort plan by key shape.
- `test/sql/col_search.sql` (pg_regress): a 24,000-row table with wide rows (many pages), a BM25 index and a `sift_col` index with a small write buffer limit; about 26 queries (term, and, or, not, phrase, negated phrase, prefix, wildcard, regex, fuzzy, boost, no match, NULL query, parameters, filters, global aggregate, float sums, nulls, `HAVING`, expression keys, generic aggregates, `IN`, two search conjuncts, a function index) compared with the plan without SiftAgg (`EXCEPT ALL` both ways) in the stages build, chunks of one superblock, parameters, counter check, forced plan, insert, update, delete, own transaction, rollback, vacuum, column-index promotion, parallel workers, and `sift_table`. Tie order of ranked results is not touched: no ranked path changed.
- `test/col/fixtures/h_search_*.sql` (coldiff): term, boolean, phrase, prefix, fuzzy, global, no match, two conjuncts, `HAVING`, parameter, all with the DML scenarios (insert, update, delete, abort, vacuum, HOT, TID recycle, held transactions, reindex, promotion) and the five variants (parallel 0/2, SIMD scalar/auto).

## Limits

- Only top-level `AND` conjuncts `col @@> q` take part; an `OR` between a search and another predicate (or two searches) keeps the normal plan. Write several terms as one query (`'a OR b'`).
- One BM25 index per search; several conjuncts on the same index are all evaluated by it, conjuncts on two different indexes are not extracted.
- Phrase matches that the index leaves undecided need their text from the heap (`recheck_page`): phrase-heavy queries read the heap for the undecided pages only.
- Each participant of a parallel scan evaluates the query for its own superblocks (the set is not shared), so the posting decode is not divided by the worker start-up cost.
- Filters that the source applies on encoded values (`docs/COL-PLAN.md`, "Filters in the source") are not used in a scan with a search: the rows of a match set may be renumbered, so the executor tests them.
- Top-N: `sift_col` and `sift_table` (the table source gives the TIDs of its rows, the zones of its units and the sort values of the best rows, `tbl_topn`; it decides a window of superblocks by the zones of its units alone, because the new version of an updated row is in the write buffer whatever its value, so its superblocks do not follow the column), workers keep a heap each and the leader merges them (see above), sort keys must be plain columns of the index with the types above, `coalesce(column, constant)`, `lower(column)`, `upper(column)` or an immutable arithmetic or cast expression of the columns (other expressions and `sift.score` mixed with columns keep the normal plan), no `OR` of a search and a column predicate, a constant `n + m <= 100000` (a parameter past it sorts), and the tie-break column (usually `id`) must be in the `sift_col` index. `sift.facets()` on keyword indexes is unchanged.
- Units are skipped only when none of their keys is in the match set; term statistics are not used to skip units.

## Hooks in files owned by other work

- `src/agg/plan.c`, `plan_int.h` and `plan_cost.c`: `AggCtx.search` and the call `sift_search_extract(...)` after `agg_spec_new` in `sift_agg_upper_paths`; `agg_cost` adds `sift_search_cost`; `agg_finish` copies the spec; `spec_deps` records the index dependency.
- `src/agg/spec.[ch]`: `Node *search` in `SiftAggSpec`, packed last in `custom_private` (`put_node`/`get_node`).
- `src/agg/agg.c`: open/close of the match set in `engine_open/close`, EXPLAIN lines; `src/agg/par.c`: chunking in `scan_range`, worker counters.
- `src/agg/colsrc.h`, `src/agg/colsrc.c`, `src/agg/col.c`, `src/agg/col_read.c`: the optional `select` provider callback, and for top-N the `tids`, `zone` and `read_rows` ops (`SiftSrcZone`).
- Top-N: `src/agg/plan.c`, `plan_order.c` and `plan_cost.c` (`spec_finish` split out of `agg_finish`, `topn_paths`, the `UPPERREL_ORDERED` dispatch, heap-fetch and correlation terms in `agg_cost`), `src/agg/spec.[ch]` (`topn`, `desc_topn`), `src/agg/agg.c` (`topn_run`/`topn_next`, `engine_open/close`, EXPLAIN, GUC `sift.enable_topn`), `src/agg/unit.c` (`topn_unit`, hooks in `process_unit`), `src/col/codec.c` (`sift_cc_decode_sel` goes dense earlier for delta-coded nodes), `src/col/enc.[ch]`, `src/col/scan*.c`, `src/col/col.h` (`sift_cunit_row_keys`, `sift_col_row_tids`).
- `src/col/scan*.c`, `src/col/enc.[ch]`, `src/col/col.h`: `sift_col_filter_tids` (match-driven), `sift_col_read_idx`, `sift_col_unit_allvisible`, sparse page masks, a faster `sift_col_vet`; `src/core/filter.[ch]`: `sift_filter_create/reset/add_page`.
- BM25 side: only the existing iterator entry points (`sift_plan_bind`, `sift_iter_*`, `sift_plan_recheck`, `sift_vis_fetch`, `sift_estimate_cached`). The search commits changed nothing in `bm25/cscan.c`, `bm25/eval.c` or `bm25/segment.c` (the BM25 segment format 2 of 3.0, landed separately).
