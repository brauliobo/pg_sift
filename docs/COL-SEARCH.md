# Text search as the row selection of an aggregate (SiftAgg)

`SELECT <aggregates> FROM t WHERE <filters> AND col @@> 'text' GROUP BY ...` runs as three independent steps that meet on row keys:

1. the BM25 index (`sift`) evaluates the query to the **match set**: the keys (`blk << 16 | off`; virtual TIDs for a `sift_table`) of the rows that match, superblock by superblock, **without reading the heap**;
2. the column engine (`sift_col` index or `sift_table`) restricts each of its units to those keys, evaluates the column filters, and aggregates from columns only;
3. visibility is the rule every SiftAgg scan has: rows that survive the filters are vetted at the end (all-visible pages pass, the others are checked in the heap).

Code: `src/agg/search.[ch]` (planner extraction, cost, match-set iterator), `src/agg/sel.[ch]` (the rows of a unit a match set keeps), `src/agg/colsrc.h` (`select` provider callback), hooks in `src/agg/plan/`, `src/agg/agg.c`, `src/agg/spec.[ch]`.

## What other engines do

| Engine | Text query | Doc values / columns | Combination |
|---|---|---|---|
| Lucene / Elasticsearch | a `Scorer`/`DocIdSetIterator` over the postings of each segment | doc values (columnar, per segment) | a `Collector` pulls `nextDoc()` and reads the doc values of that doc id; filters are cached bitsets intersected with the iterator |
| ClickHouse | the `text` index is an inverted index; per token a posting list of row numbers, AND/OR-ed ([docs](https://clickhouse.com/docs/engines/table-engines/mergetree-family/textindexes), [blog](https://clickhouse.com/blog/clickhouse-full-text-search)) | MergeTree columns | the index result filters the granule's rows; other columns are read only for surviving rows |
| Pinot / Druid | inverted index (bitmap per dictionary id; Druid: roaring or concise bitmaps) | forward index per column | filter operators produce a doc id set; aggregation iterates it over the forward index |

Common design: the inverted index yields a **doc id set** in a compact ordered form, the columnar side reads values only for selected doc ids, deletes are a live-docs bitmap. In pg_sift the doc id is the heap key, the segment is the superblock (256 heap pages), the doc id set is a `SiftFilter` (per-superblock page masks plus a 64-byte offset mask per page), and the live-docs bitmap is the MVCC vet.

## Interface

Provider of another match source (`sift_match_bitmap`-like):

```c
/* the matches in the superblocks [lo, hi), valid until the next call; calls ascend, or the iteration starts again */
const SiftFilter *sift_search_range(SiftSearch *s, uint32 lo, uint32 hi);
```

A column provider opts in with `select` in `SiftColProvider` (`sift_colsrc_select(src, keep)`); it calls `sift_aggsel_unit()` per unit (the number of rows the executor sees, or false when no row is kept) and `sift_aggsel_read/live/vet()` for the rest. `sift_col`'s `agg/col.c` and `sift_table`'s `table/src.c` use it; a provider without `select` is never given a search (`sift_colsrc_can_select`).

Contract of the match set:

- an **exact or wider** set of the visible matches of the snapshot: it may hold dead or not yet visible tuples (the index is not MVCC-aware), never miss a visible match;
- rows the index cannot decide (phrases are relaxed in the index) are decided on the heap text before they enter the set (`recheck_page`, as `SiftScan`/`SiftCount`): the only heap read of the search itself;
- exact in its pages (no lossy pages), 64 bytes per page that has a match.

## Visibility semantics

The match set does not check visibility; SiftAgg does. Every unit's selection goes through `sift_col_vet`: pages all-visible in the visibility map pass, the others are checked in the heap, so a dead or invisible row the index returns is dropped. Rows in the BM25 write buffer (inserted, not yet merged) are covered by the iterator. Differential tests compare insert, update, delete, own-transaction rows, rollback and vacuum states against the normal plan (`test/sql/col_search.sql`, `test/col/fixtures/h_search_*.sql`).

- The match set is read a chunk of superblocks ahead of the vet, and a `VACUUM` marks pages all-visible only after it has cleared the index entries of their dead rows. So the vet of a scan whose column index changed since the scan read it (its directory version) takes no page as all-visible and checks the heap (test `vacuum_agg_snapshot`). The column index's liveness decides the rows: a dead row the match set still holds is not among them.
- A query with the search in the same transaction as writes sees its own rows: the vet reads the heap for pages that are not all-visible, and the iterator reads the write buffer.

## Planner

`sift_search_extract` runs in `sift_agg_upper_paths` after the shape matcher. From the base restrictions it takes the conjuncts `key @@> q` of **one** BM25 index of the table (single column; a partial index only when the query implies the predicate; not hypothetical), with `q` free of Vars, volatile functions and sub-plans (a constant, a statement parameter, a stable function of them). They leave the filter list and become `SiftAggSpec.search` (the index Oid and the query operands, serialized for parallel workers). Anything else keeps the old plan (the `@@>` conjunct stays an ordinary heap filter).

Cost (`sift_search_cost`), in operator costs:

```
bind (20000) + terms * 400 + postings * 0.25 + matching pages * 12
```

`docs` comes from `clauselist_selectivity` of the conjuncts (the BM25 estimator, the one `SiftScan` uses), postings/terms from the cached term statistics for constant queries. The scan then reads `min(rows * unitsFraction, docs * 8)` values per leaf in the units that have a match. The plain plan is always generated and stays when cheaper (few matches and no group by, `df = 0`, a table without a column index).

`EXPLAIN` shows `Search: orders_title @@> 'wireless headphones'`; `EXPLAIN ANALYZE` adds `Search Matches` and the unit counters (`units pruned` counts the units without a match).

## Executor

- Queries are bound to the index once per participant (`sift_plan_bind`); a NULL query matches nothing.
- The scan walks units in morsels (`MORSEL_SBS = 2` superblocks). Before a morsel, `scan_range` asks the match set for a chunk of superblocks (`sift.test_search_chunk_sbs`, default 32, so 32 * 64 B * pages in memory at most); the source restricts each unit with `sift_col_filter_tids` (rank-based and match-driven: a unit with 20 matches in 100k rows touches those 20 rows).
- A unit with few kept rows (`count * 8 <= rows`) is returned **compacted**: rows renumbered, each leaf decoded for those rows only (`sift_col_read_idx`, `sift_cchunk_decode_sel`). A unit with many keeps its numbering and its liveness mask is the kept rows.
- Float sums keep the lane order of `SIFT_AGG_LANES`. Each participant of a parallel scan holds its own match set (iterator restarted when a lane's superblock order goes backwards), so the posting decode is not divided by the worker start-up cost.
- All aggregates, group keys, filters, lookup joins, grouping sets, generic aggregates, `HAVING` and parallel workers work unchanged: the search only changes which rows are alive in the unit.

## Facets and top-N

**Facets.** `SELECT category, count(*) FROM t WHERE text @@> q [AND filters] GROUP BY category ORDER BY 2 DESC LIMIT n` runs this path: counts per category from the column over the match set, like `sift.facets()` on a keyword index but with column filters and any other aggregate. At 1M (`sift.facets` on a keyword index of `category` against the SiftAgg query, same results):

| Query | `sift.facets` | SiftAgg search |
|---|---|---|
| `wireless` (292k matches) | 7.5 ms | 4.6 ms |
| `wireless headphones` (7k) | 7.0 ms | 2.7 ms |
| `cookware` (9k) | 6.6 ms | 1.4 ms |

**Top-N.** `WHERE col @@> q [AND filters] ORDER BY <columns> [ASC|DESC] LIMIT n [OFFSET m]` is a SiftAgg path too (`sift.enable_topn`, default on, needs `sift.enable_agg`). The planner adds it at `UPPERREL_ORDERED` (`topn_paths`, `src/agg/plan_order.c`) when the query has no groups, aggregates, windows, `DISTINCT`, joins or row marks, `LIMIT`/`OFFSET` are constants or statement parameters, and every sort expression is a column of the `sift_col` index or one of the expressions below.

Sort keys:

- **Columns**: bool, int2/int4/int8, date, time, timestamp(tz), numeric(p <= 18), money, float4/float8, default btree order, any `ASC`/`DESC`/`NULLS` combination; NaN is the largest. A text, varchar, uuid, bytea or inet key is the entry of the unit's dictionary, compared as the query's sort operator does (`varstr_cmp` under the query's collation for text: byte order under `C`, `strcoll` then byte order under any deterministic collation of the C library or ICU; `uuid_cmp`, `byteacmp`, inet's comparison function), so rows and order are the normal plan's. A nondeterministic collation, a `char(n)`, `name` or `cidr` column and a non-default operator class (`text_pattern_ops`) keep the normal plan.
- **`coalesce`, `lower`, `upper`**: `coalesce(<int2, int4, int8, date, timestamp or timestamptz column>, <constant of the same type>)`, `lower(<text or varchar column>)`, `upper(...)`, with any direction and NULLs clause. A NULL becomes the constant; strings go through PostgreSQL's own `lower()`/`upper()` under the call's collation, once per distinct entry of a unit, and are compared by the sort operator under the expression's collation. A `coalesce` key keeps the zones (the zone of a unit is widened to the constant when it holds NULLs), so units are skipped and the walk and the parallel bound work as for the column.
- **Arithmetic and casts**: any other immutable expression over the integer, float, numeric (18 digits or fewer), date, time, timestamp, text and uuid columns of the index: `a + 1`, `1000 - a`, `-a`, `a * b`, `total / 3`, `a % 7`, `a::bigint`, `a::text`, `d + 30`, `t::date`, a comparison. The scan runs it with the backend's expression machinery (`ExecInitExpr`, the unit's leaves as a virtual slot) for every row that passes the `WHERE` and the late conjuncts, and keeps the results as the key vector, so the heap, the sort of every row and the parallel merge are the column's. Operators, casts and comparison are PostgreSQL's, hence its overflow (`integer out of range`), division by zero, NULL and NaN behavior; a query that raises raises the same SQLSTATE in both plans (the message can name another row; which of several rows raises first is not promised). No zone bounds an expression, so a unit is read whenever it holds matching rows; a sorted run of a column is not used. The planner prices the per-row evaluation (`TOPN_EXPR_ROW`, the expression's own cost, a charge per numeric or text value) and keeps the plan's sort when the scan would be slower.
- **Control flow and tests**: `CASE` (searched and simple), `coalesce` of several columns, `NULLIF`, `GREATEST`/`LEAST`, `AND`/`OR`/`NOT`, `IS [NOT] NULL`, `IS [NOT] TRUE`, `IS DISTINCT FROM`, `IN (..)`/`= ANY(..)` of constants, comparisons and functions of strings with a number result (`length(s)`, `strpos(s, 'a')`, `s < v`), alone or inside arithmetic. The evaluator keeps the lazy order of `CASE` and `coalesce` (a division in an untaken branch does not raise), three-valued NULL logic and the collation of string comparisons.
- **Functions and parameters**: any immutable function or operator, as PostgreSQL evaluates it, whatever the types between the columns and the key (only the leaves and the result are stored): `lower(s || v)`, `upper(lower(s))`, `left(s, 3)`, `substr`, `initcap`, `md5(s)`, `lpad`, `abs(a)`, `mod(a, 7)`, `round(n, 1)`, `sqrt(abs(f))`, `date_trunc('month', ts)`, `extract(month FROM d)`, `make_date(..)`, `ts + g * interval '1 hour'`, `length(b)` and `encode(b, 'hex')` of a bytea, `host(ip)` and `masklen(ip)` of an inet, a plpgsql function declared `IMMUTABLE`. The result is a number, date, time, string, uuid, bytea or inet; a string result has the expression's collation. A statement parameter (`ORDER BY a + $1` in a generic plan) is read from the leader's executor state, so the scan runs in the leader without workers, as for a function that is not parallel safe.
- **Refused** (normal plan, reason in `topn_shape`): a volatile or stable expression (`random()`, `now()`, `concat()`, `timestamptz + interval`, the text of a date), a sub-select, a result that is an interval, an array or jsonb, a numeric leaf of 19 or more digits.
- **String keys** have no zones: a first key of strings reads every unit that holds matching rows, compares each row with the n-th best (a dictionary entry once per unit, then remembered) and keeps its own copy of the strings of the heap rows. A heap whose keys would not fit `work_mem` is planned as the sort of every row (below), run as a tuplesort of the strings under the query's operator and collation.

Mechanism: filters and match set are evaluated as for an aggregate; per unit the passing rows go to a bounded heap (`src/agg/topn.c`) of the `n + m` best, ties broken by TID. Only the first sort key is decoded for all passing rows; the other keys and TIDs are read (`read_rows`, `tids`) for rows that beat the heap's worst. At the end the heap rows are fetched from the heap by TID in order (virtual slot of the scan tuple): `n + m` heap reads whatever the number of matches. The `Limit` node stays above the scan.

**Parameters, deep pages, ORs.** `LIMIT $1 [OFFSET $2]` of a generic plan (`plan_cache_mode = force_generic_plan`, or the fifth execution of a prepared statement) is accepted: the plan is priced for an assumed 1000 rows and the scan evaluates the two expressions (no sub-plan or volatile function) when it starts.

- Up to 100000 rows and 8 sort keys the heap is the one above. `NULL` (all rows), a sum past the cap, an overflow or more than 8 sort keys keeps every passing row as keys and TID (an array in memory up to `hash_mem`, then a `Tuplesort` at `work_mem` that writes runs to disk), sorts them, and still fetches from the heap only the rows the `Limit` takes. Zero or negative reads nothing (the `Limit` node raises its own error for a negative value).
- **Deep pages**: a constant `n + m` past 100000, more than 8 sort keys, or an `n + m` past 1024 whose heap costs more than the sort is planned as a sort of every passing row (`Top-N: .. (limit N, sorted)`, priced by `topn_sort_cost` with the table read whole and the heap's cost for many rows) and taken by cost against PostgreSQL's sort. When the output columns are plain columns, the first `m` rows, which the `Limit` discards, are handed to it as NULL rows and not read from the heap (`Heap Fetches` is `n`, not `n + m`). `EXPLAIN` shows `limit by parameter`; `EXPLAIN ANALYZE` adds `Top-N Sorted Rows` when the sort was used. Limits: `docs/LIMITS.md` (`topn_rows`, `topn_keys`); tests `col_topn_deep`, `col_topn_deep_limits`.
- **Trailing sort keys**: `ORDER BY event_time, watch_id LIMIT 10` where only the first keys are in the index (`watch_id` is not, or is an expression of columns that are not) is a top-N of the first keys under PostgreSQL's own `Incremental Sort` of the whole `ORDER BY` (`Presorted Key`), which puts the rows that tie on them in order and passes the first `n` to the `Limit`. The first keys must be integer, date or timestamp columns of the index (their equality is the same to the scan and to the sort) and the statistics of the first key must say that at most 8192 rows share a value (a NULL group counts when it sorts first and is larger than the `LIMIT`); a window function, a repeated sort key or a text key among the first keys keeps the normal plan. The scan keeps `n + m` plus twice the expected tie size rows (`Top-N Candidates` in `EXPLAIN ANALYZE`), and gives the rows up to the last one that ties with the `n + m`-th on the first keys. When the rows that tie with the last row kept may not all be in the heap (the statistics did not know a large group) the scan starts again with a heap 16 times larger, twice, then keeps every row (`Top-N Restarts`); the result is the same, a restart costs one more scan. Not parallel. 1,000,000 rows, wide table, `ORDER BY event_time, watch_id LIMIT 10`: 108 ms (sequential scan and sort) to 0.4 ms; 10,000,000 rows: 940 to 0.5 ms; the one-key top-N takes 0.35 and 0.51 ms. Every row of a tie group is fetched from the heap for the keys that follow, so the gain falls with the group: 1,000,000 rows, `ORDER BY k, w LIMIT 10` with `w` not in the index, 85 ms for the sequential scan and sort against 15.7 ms at 2,500 rows a value, 21.9 at 5,000, 37 at 10,000, 71 at 16,667 (the cost model keeps the sort from 25,000 rows a value on); the 8192 limit keeps the gain above 2x. Test `col_topn_trail`, fuzz family `trail`, shapes `b07` to `b09` of the plan gate.
- **ORs**: `WHERE key @@> a OR key @@> b ...` (every branch a match of the same BM25 index, a constant or a parameter) is a top-N too: each arm is bound as its own query (so syntax errors, the `NOT` rule and phrase rechecks are the normal plan's) and the match set is the union of the arms' pages, `OR`-ed per heap page. A search ORed with a column predicate, an OR next to another match, or a branch that is an `AND` stays normal; `sift.score()` is not available here. The aggregate scans do not take an OR of searches.

**Early stop and visit order** (zone maps, `zone` provider op: min, max, nulls of the first sort key per unit).

- *Key follows the table* (a clustered or monotonic column such as `created_at` or a serial `id`): detected by probing six superblocks spread over the table. Superblocks are visited from the end where the best values are, in windows that start at one superblock and double up to eight (the best superblock first usually fills the heap, so the rest are skipped by their zones). A window is read only when the zones of its units, taken together from the segment headers (`zone_range` op: no unit is opened), can beat the heap's worst. The probes choose the order, not the end, because the new version of an updated row is stored wherever there is room and may hold a value far from its neighbours (`col_topn_moved`). The match set is asked for exactly the first window (it costs by the superblocks it covers), then for later windows in chunks of `sift.test_search_chunk_sbs`, because the iterator restarts when asked for superblocks it has passed.
- *Key does not follow the table* (random, or blocks of values in no order), scan without a search: the zones of all superblocks are read first (segment headers, no unit opened), superblocks are sorted by the best row their zone can hold (NULLs first under `NULLS FIRST`, a non-exact zone first, NULL-only superblocks last under `NULLS LAST`) and visited in that order; the scan ends at the first superblock whose zone cannot beat the n-th row (`sift.topn_walk`, default on; `off` visits in table order). A scan with a search keeps the table order, for the iterator reason above.
- *Ties*: when the first key ties between units (a few statuses, a day number) the zones of the following integer keys bound each superblock in key order (`ORDER BY status, id LIMIT 10` reads the units holding the first ids of the first status, `sift.topn_zone_keys`). The result does not depend on the order: a superblock is left out only when no row in it can sort before the n-th, and a zone equal to the n-th is kept for the other keys and the TID.

`EXPLAIN` shows `Top-N: <keys> (limit n)`, `Superblocks Skipped By Top-N`, `Heap Fetches`.

*Cost.* The scan is scaled by the fraction a correlated column needs (`pg_statistic` correlation >= 0.9) or, for a column walked by its zones, by the mean zone width plus `n` over the expected matches, except the opening of the match set (the iterator takes the terms' posting lists whole: `sift_search_open_cost`), plus `n` random heap fetches. Against an ordered btree on the sort column (`Index Scan Backward` with the search as its filter) PostgreSQL compares the two estimates: an uncorrelated column with a btree keeps the btree plan (`created_at` below), a rare search takes the top-N (`test/sql/col_topn.sql` checks both). The btree plan is priced at the declared cost of `sift.match` (COST 1000, 2.5 per row examined), about 5000 for an index scan that takes 0.5 ms; at COST 100 the estimate is 565 and the btree wins the `id` case below. The top-N estimate (about 1600 at 10M) is near its time, so the declared cost is the only term left to change; it belongs to the BM25 side and is unchanged.

**Parallel top-N.** With `max_parallel_workers_per_gather` above 0 the scan may plan workers (`Workers Planned` / `Workers Launched`) as the aggregate scans do. The leader decides the visit order; workers and leader take windows of superblocks (one superblock, or a match-set chunk) from a shared counter, each keeping the heap of its best `n + m` rows.

- They share the first-key rank of the n-th row of any heap (one atomic in shared memory, tightened whenever a participant's heap is full): a unit whose zone, and a row whose key, sorts strictly after it is dropped, and a window ordered by its best bound ends the walk for everyone.
- A participant takes a superblock only when the zones of its leading sort keys (the first, and the integer keys after it, `sift.topn_zone_keys`) can beat the n-th row of its own heap key by key, and the first key's zone the shared rank.
- A worker sends the keys and TIDs of its heap, not rows; the leader merges by keys and TID, so rows, order and tie-break are those of one process, and fetches the `n + m` result rows. Dirty units are read again from the heap by the worker that owns the item, under the statement snapshot.
- Not parallel: a `LIMIT` that is a parameter past the cap, `NULL` or negative (the scan sorts), and a node under a worker.
- Workers start at 4 to 15 ms, so the planner prices them as for the aggregate scans (`parallel_setup_cost` times `AGG_SETUP_FACTOR`) and plans of a few milliseconds stay serial. With workers forced (`parallel_setup_cost = 0`), a filter evaluated row by row (`(k % 7) * (a % 13) + (g * 3) % 11 > 40 AND ..`) and `ORDER BY` a random key `LIMIT 1000`: 200,000 rows 63 ms without workers, 32 with 2, 21 with 4; 100,000 rows 25, 17, 16; 50,000 rows 14, 16, 14; 25,000 rows 9, 11, 17. Plans of 1 to 3 ms (an index-ordered key, a selective filter, `LIMIT 100` of a random key) take 10 to 19 ms with workers forced. The cost of an expression filter is still underestimated, so the planner keeps the first of these serial at default costs.

**Joins.** `ORDER BY f.key LIMIT n` over `fact f JOIN dim d ON d.k = f.k WHERE <conditions of d>` is a top-N of the fact table when the dimension only filters: the `dim` rows that pass their conditions are read when the scan starts and their keys become the conjunct `f.k IN (keys)` (the join filter of the aggregate scans, `join_filters` in `agg/init_table.c`; units whose zone holds none of the keys are not read). An `IN (SELECT ..)` or `EXISTS (..)` that PostgreSQL pulls up into a semi join is the same filter and needs no unique key (a semi join matches a row once). Otherwise it needs a unique index on `dim.k`, an inner join on one `int2/int4/int8/date` column, the ON clause and WHERE reading the dimension's columns alone, and the select list and `ORDER BY` reading the fact table's. Everything else keeps the normal plan (`topn_shape` in `sift.explain_refusals`): a non-unique key of an inner join, an outer or anti join (`NOT EXISTS`), a scalar sub-select comparison, a text key, an expression of the key, a condition that reads the scanned row, a dimension column in the result, a second table that is not such a filter. The scan runs in one process. 200,000 rows, `LIMIT 10`, normal plan against top-N: 88 ms against 0.5 ms for half the keys, 71 ms against 2 ms for one key (`test/sql/col_topn_join.sql`).

`ORDER BY sift.score(...) DESC LIMIT n` is the ranked `SiftScan`, unchanged.

## Measured

Orders table of `bench/sql/store` (title BM25 index, `sift_col` over the report columns), 1M and 10M rows, one backend, `max_parallel_workers_per_gather = 0` unless stated, warm cache, pgbench latency; instructions are user space of the backend (`perf stat -p`) per execution. Before is `sift.enable_agg = off` (heap plan with `SiftScan` and a hash aggregate); the planner's default choice is shown where it differs.

Bench question 09 (`status = 'shipped' AND amount >= 50 AND title @@> 'wireless headphones' GROUP BY category`, top 10):

| Rows | Before | After | Speedup | Instructions before | after | Default plan |
|---|---|---|---|---|---|---|
| 1M | 6.4 ms | 2.1 ms | 3.1x | 21.6 M | 11.6 M | 1.8 ms |
| 10M | 60.6 ms | 23.3 ms | 2.6x | 213.7 M | 116.1 M | 21.3 ms |

Top-N, same table and conditions (`title @@> 'wireless headphones'` unless noted), serial, warm; before is `sift.enable_topn = off` (`Limit -> Sort -> SiftScan`). Bench question 10 is the first row (an earlier sort-plan measurement was 6.0 ms at 1M and 52.8 ms at 10M on another build of the data):

| Query | 1M before | 1M after | 10M before | 10M after |
|---|---|---|---|---|
| `ORDER BY created_at DESC, id DESC LIMIT 10` (search-newest) | 7.1 ms | 2.4 ms | 191 ms | 19.5 ms |
| `ORDER BY created_at, id LIMIT 10` | 7.5 ms | 3.1 ms | 288 ms | 24.5 ms |
| `... LIMIT 100` (search-newest shape) | 6.0 ms | 3.1 ms | 189 ms | 22.0 ms |
| `ORDER BY amount DESC, id LIMIT 20` | 101 ms | 6.7 ms | 842 ms | 38.2 ms |
| `ORDER BY customer_id, id LIMIT 10` | 18.9 ms | 4.2 ms | 487 ms | 34.1 ms |
| search-newest shape, 292k matches (`wireless`) | 96 ms | 5.1 ms | 785 ms | 29.1 ms |
| search-newest shape, no search, filters only | 91.7 ms | 4.2 ms | 1196 ms | 19.5 ms |
| `ORDER BY id DESC LIMIT 10` (id follows the table) | 0.41 ms | 0.32 ms | 11 ms (default plan) | 0.74-1.1 ms |

Against ordered btrees (pgbench latency, serial; orders of 1M and 10M rows with random titles of three words from thirty, a btree on `id` and on `created_at`, `title @@> 'wireless headphones'`, about 0.5% of the rows; host CPU pressure 4-6%, I/O 0% before and after each batch). "Before" is `sift.enable_topn = off`, which leaves the btree plan where there is one:

| Query | 1M before | 1M after | 10M before | 10M after |
|---|---|---|---|---|
| `ORDER BY id DESC LIMIT 10` | 0.47 ms | 0.28 ms | 0.57 ms | 0.71 ms |
| `ORDER BY created_at DESC LIMIT 10` (btree plan kept) | 2.7 ms | 1.3 ms | 1.34 ms | 1.55 ms |
| `ORDER BY amount DESC, id LIMIT 20` | 1.3 ms | 1.4 ms | 26.4 ms | 10.6 ms |
| `ORDER BY customer_id, id LIMIT 10` | 1.2 ms | 1.0 ms | 22.8 ms | 10.2 ms |

At 10M the top-N on `id` is 1.25x slower than the btree (0.7 against 0.57 ms, planning included): opening the two posting lists copies their bytes, which the btree scan of 2,000 rows does not pay (2.0 ms before the best-superblock-first order: one unit read instead of eight). Where the btree plan stays (`created_at`) the two columns differ by noise. `created_at` does not follow the table, so search-newest reads all units whose zone can beat the heap; the gain is the avoided heap fetches and sort. A column that follows the table (`id`) stops after the first window: 2 units of 496 superblocks at 1M, 8 units at 10M.

Selectivity at 1M (`... AND status = 'shipped' GROUP BY category`, serial):

| Query | Matches | After | Before |
|---|---|---|---|
| `cookware` | 9,371 | 1.2 ms | 5.7 ms |
| `detergent` | 4,235 | 0.9 ms | 1.5 ms |
| `wireless headphones` | 7,143 | 1.4 ms | 5.7 ms |
| `wireless` | 291,928 | 5.8 ms | 81.7 ms |
| `portable OR compact` | 217,481 | 5.5 ms | 69.3 ms |
| `smart OR classic OR ultra` | 194,659 | 6.6 ms | 59.0 ms |

The gain grows with the match count: the heap plan pays a heap fetch per match, the search path a posting decode plus a column read. With few matches the fixed cost (binding, about 60 us; walking unit headers) dominates. A profile at 10M: whole-chunk column reads (`sift_sr_get`) about 40%, BM25 iteration about 27%, then per-unit setup; a sparse unit read by position (`SEL_SPARSE`) costs about 8 dense values, which the cost model uses. Worker start-up is about 6.5 ms at 10M, so parallel plans win only when the serial scan is well above that; the cost model chooses serial at 1M and 4 workers at 10M when `parallel_setup_cost` is low enough.

## Tests

- `test/sql/col_topn.sql` and `test/col/fixtures/h_topn_*.sql` (coldiff): about 33 top-N queries (search, filters, directions, nulls first/last, NaN, two and three keys, offset, parameters, phrase, prefix, fuzzy, boolean, a text key; `OR` and `sift_table` stay plain) against the sort plan.
- `test/sql/col_topn_text.sql`, `test/col/fixtures/topn_key_*.sql`: string-like keys (text under `C` and a locale, varchar, uuid, bytea, inet), ties, multi-byte and long strings, the sort of every row (deep page, keys wider than `work_mem`, spilling tuplesort), workers, write buffer, deleted and updated rows, the named refusals.
- `test/sql/col_topn_order.sql`: keys that follow the table, lie in blocks, are random or hold ties and NULLs; a differential fuzz of 480 queries from a fixed seed after build, deletes, moved rows, write buffer and promotion. `bench/topn_order.py` times top-N and sort plan by key shape.
- `test/sql/col_search.sql`: a 24,000-row table with wide rows, a BM25 index and a `sift_col` index with a small write buffer limit; about 26 query shapes (term, boolean, phrase, prefix, wildcard, regex, fuzzy, boost, NULL query, parameters, `HAVING`, expression keys, `IN`, two search conjuncts, a function index) against the plan without SiftAgg (`EXCEPT ALL` both ways) through build, one-superblock chunks, insert, update, delete, own transaction, rollback, vacuum, promotion, parallel workers and `sift_table`.
- `test/col/fixtures/h_search_*.sql` (coldiff): the same families with the DML scenarios (abort, HOT, TID recycle, held transactions, reindex, promotion) and five variants (parallel 0/2, SIMD scalar/auto).

## Limits

- Only top-level `AND` conjuncts `col @@> q` take part; an `OR` between a search and another predicate (or two searches) keeps the normal plan (top-N takes an OR of searches). Write several terms as one query (`'a OR b'`).
- One BM25 index per search: several conjuncts on the same index are all evaluated by it, conjuncts on two different indexes are not extracted.
- Phrase matches the index leaves undecided read their text from the heap (`recheck_page`), for the undecided pages only.
- Filters the source applies on encoded values (`docs/COL-PLAN.md`, "Filters in the source") are not used in a scan with a search: match-set rows may be renumbered, so the executor tests them.
- Units are skipped only when none of their keys is in the match set; term statistics are not used to skip units.
- Top-N: `sift_col` and `sift_table` (the table source gives the TIDs, unit zones and sort values of the best rows, `tbl_topn`; it decides a window of superblocks by the zones alone, because the new version of an updated row is in the write buffer whatever its value). Sort keys are the columns and expressions above (others and `sift.score` mixed with columns keep the normal plan); no `OR` of a search and a column predicate; a constant `n + m <= 100000` (a parameter past it sorts); the tie-break column (usually `id`) must be in the `sift_col` index. `sift.facets()` on keyword indexes is unchanged.

## Code map

- `src/agg/plan/`, `plan_int.h`, `cost/model.c`: `AggCtx.search` and the call `sift_search_extract(...)` after `agg_spec_new` in `sift_agg_upper_paths`; `agg_cost` adds `sift_search_cost`; `agg_finish` copies the spec; `spec_deps` records the index dependency.
- `src/agg/spec.[ch]`: `Node *search` in `SiftAggSpec`, packed last in `custom_private` (`put_node`/`get_node`).
- `src/agg/agg.c`: open/close of the match set in `engine_open/close`, EXPLAIN lines; `src/agg/par.c`: chunking in `scan_range`, worker counters.
- `src/agg/colsrc.h`, `src/agg/colsrc.c`, `src/agg/col.c`, `src/agg/col_read.c`: the optional `select` provider callback, and for top-N the `tids`, `zone` and `read_rows` ops (`SiftSrcZone`).
- Top-N: `src/agg/plan/`, `plan_order.c`, `cost/model.c` (`spec_finish` split out of `agg_finish`, `topn_paths`, the `UPPERREL_ORDERED` dispatch, heap-fetch and correlation terms in `agg_cost`), `src/agg/spec.[ch]` (`topn`, `desc_topn`), `src/agg/agg.c` (`topn_run`/`topn_next`, `engine_open/close`, EXPLAIN, GUC `sift.enable_topn`), `src/agg/unit.c` (`topn_unit`, hooks in `process_unit`), `src/col/codec.c` (`sift_cc_decode_sel` goes dense earlier for delta-coded nodes), `src/col/enc.[ch]`, `src/col/scan*.c`, `src/col/col.h` (`sift_cunit_row_keys`, `sift_col_row_tids`).
- `src/col/scan*.c`, `enc.[ch]`, `col.h`: `sift_col_filter_tids` (match-driven), `sift_col_read_idx`, `sift_col_unit_allvisible`, sparse page masks, a faster `sift_col_vet`; `src/core/filter.[ch]`: `sift_filter_create/reset/add_page`.
- BM25 side: only the existing iterator entry points (`sift_plan_bind`, `sift_iter_*`, `sift_plan_recheck`, `sift_vis_fetch`, `sift_estimate_cached`); `bm25/cscan.c`, `bm25/eval.c` and `bm25/segment.c` (BM25 segment format 2 of 3.0) are unchanged.
