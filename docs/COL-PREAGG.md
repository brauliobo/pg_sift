# Pre-aggregates of a unit (metadata-only and low-cardinality aggregation)

Status: the writer, the format, the provider callbacks and the SQL hooks are implemented (column format 1.2); the `SiftAgg` consumption (section 6) is designed here and not yet coded: `src/agg/sift_agg*.c` belong to the planner track.
Files: `src/col/sift_col_preagg.[ch]` (formats, option, compute, verify; portable), `src/col/sift_col_seg.c` (written with the unit, read per segment), `src/col/sift_col_pagg.[ch]` (through the column source), `src/agg/sift_colsrc.h` (`unit_exact`, `unit_agg`, `unit_hist`, `preagg`), test `col_preagg`.

## 1. What other engines precompute, and what is taken

| System | What it keeps | Taken here |
|---|---|---|
| Parquet / ORC | per row group and column: `null_count`, `min`, `max`, (ORC: `sum`, `count`) in the footer ([Parquet statistics](https://github.com/apache/parquet-format/blob/master/README.md), [ORC ColumnStatistics](https://orc.apache.org/specification/ORCv1/)); `count/min/max` answered from the footer | the zone maps already hold this (`SiftCZone`); exact 128-bit `sum`/`sum of squares` added (the zone's int64 sum overflows) |
| ClickHouse | row counts and min/max of a part answer `count()` and `min/max` with no scan (`optimize_trivial_count_query`, implicit `_minmax_count_projection`); aggregating projections, `AggregatingMergeTree` ([projections](https://clickhouse.com/docs/en/sql-reference/statements/alter/projection)) | the per-unit cube is a projection `GROUP BY dim` with `count`, `sum` per measure; no merge-time state, the unit is immutable |
| Apache Pinot | star-tree: `dimensionsSplitOrder`, `functionColumnPairs` (`SUM__col`), `maxLeafRecords`; every group-by/filter column must be in the tree ([star-tree index](https://docs.pinot.apache.org/basics/indexing/star-tree-index); Xin et al., *Star-Cubing*, VLDB 2003; Gray et al., *Data Cube*, ICDE 1996) | the option `cube(dim: m1, m2)` is `dimensionsSplitOrder` of depth 1 with `SUM`/`COUNT` pairs; a query may use it only if all its aggregates and its one group column are stored (same rule); depth > 1 rejected (section 8) |
| Druid | rollup at ingestion, segment metadata ([rollup](https://druid.apache.org/docs/latest/ingestion/rollup/)) | rollup per unit, kept beside the rows (the rows stay: results are exact) |
| DuckDB | zonemaps per row group; row-group counts answer `count(*)`; statistics propagation | `sift_col_unit_exact` + zone cover test |
| Snowflake | micro-partition metadata answers `count/min/max` ([micro-partitions](https://docs.snowflake.com/en/user-guide/tables-clustering-micropartitions)) | same |
| PostgreSQL | index-only scans trust the visibility map; aggregates split into transition and combine functions ([xaggr](https://www.postgresql.org/docs/current/xaggr.html), [index-only scans](https://www.postgresql.org/docs/current/indexes-index-only-scans.html)) | visibility rule of section 4; section 5 builds PostgreSQL's own transition states |

Differences that matter: a unit is immutable, so there are no mutations to maintain (ClickHouse) and no segment updates (Elasticsearch); correctness under DML is the visibility rule alone. Results are PostgreSQL-exact: nothing is approximate (no sketches).

## 2. What is precomputed (written when a unit is built, merged, promoted, rewritten by VACUUM's merge, and by every worker of a parallel build)

- **Placement**: after the zone arrays, inside the `ZONES` section of the segment (the segment header has no free section: BM25 shares it). Header `COLP` (version, ncols, nsb, offsets); no unit record is touched, so a metadata query never reads a chunk. Format minor 2; older segments are refused with the REINDEX hint.
- **Leaf sums** (every INT-class leaf, children of lists included): exact `sum` and `sum of squares` as int128 (`u64 lo`, `i64 hi` arrays over the units, 40 bytes per leaf and unit). `sumsq` can leave int128 only for int8 values of about 10^18: flag `SIFT_CAGG_SUMSQ_OVF`, the consumer then scans. Zone data are reused for `count`, `nnull`, `min`, `max`, float `sum` (row order of the unit).
- **Cube** (`sift_cpagg_cube`): per unit and dimension `GROUP BY dim -> count(*)`, and per measure the non-NULL count and the sum (INT measure: exact int128; float: double in row order, seeded with the first value as `float8pl` does, so a group of `-0` sums to `-0`). Group keys are in key domain, ascending, the NULL group last: a text dictionary leaf by **segment code** (translate by value through the segment dictionary), an integer, `date`, `bool` or `numeric(p,s)` leaf by its stored value (a leaf holding `date_trunc('month', ts)` is the time dimension). Stored as varints (counts, deltas of keys, zigzag int128), framed by a directory so one dimension decodes alone.
- **Dimensions** are scalar INT/STR leaves (no floats: `-0` and `NaN` group by value in PostgreSQL), top level, not inline text, no overflow value. A dimension with more than 256 groups in a unit has no histogram in that unit (cap; `cube(...)`: 4096). **Measures**: `numeric`, `real`, `double`, `int2/4/8` leaves, at most 3 per dimension in the default (decimals and floats first, then integers).
- **Option** `preagg`: `'default'` (every dimension candidate, 3 measures, the unit's cube takes at most 2% of the unit's encoded bytes, at least 2 kB: the smallest histograms are kept first); `'off'` (leaf sums only); `'cube(country: amount, quantity; status)'` (exactly these, no budget; names are attribute names or shredder paths). A change applies to the segments written afterwards.
- **Size and time**, store benchmark index (customer_id, country, category, status, amount, quantity, created_at, month), median of 5 builds, busy host (load 12 to 16):

| rows | units | index bytes, core2 | with `preagg` default | sidecar | build serial (core2 / off / default) | build, 2 workers (core2 / off / default) |
|---|---:|---:|---:|---:|---|---|
| 300 000 | 16 | 3 424 256 | 3 465 216 (+1.2%) | 38 432 (2.4 kB per unit) | 203 / 181 / 202 ms | 77 / 90 / 87 ms |
| 3 000 000 | 154 | 34 078 720 | 34 455 552 (+1.1%) | 376 888 (2.4 kB per unit) | 2324 / 1344 / 1306 ms | 718 / 696 / 710 ms |

  `off` costs 0.07% (leaf sums). The serial core2 column of the 3 M row ran in a busier window and is not comparable; the quiet-window pair (off 2048, default 2127 ms) puts the cube at +3.9% serial CPU (per row: one hash per candidate dimension, three adds per stored measure; a high-cardinality dimension stops after 257 groups); with workers the difference is inside the noise (±5%).

## 3. Interface (phase 1)

- Engine (`sift_col_pagg.h`): `sift_col_unit_preagg_ok(unit)` (stored unit, `sift_col_unit_exact`), `sift_col_unit_agg(unit, leaf, &SiftCAgg)`, `sift_col_unit_hist(unit, dim, mcxt, &SiftCHistDim, &SiftColDict)` (false: the unit scans). A unit of the write buffer or a delta batch has no sidecar.
- Provider seam (`sift_colsrc.h`, additive, NULL = none): `preagg(rel, &SiftSrcPreagg)` (planner: dimensions and measures the index writes), `unit_exact`, `unit_agg` (`SiftSrcUnitAgg`: counts, min/max, exact sums, sumsq, float sum, flags), `unit_hist` (`SiftSrcHist`: groups, `SiftSrcDict` of the segment, count, per measure `nn`, `sum`/`sumf`). Memory valid until the next `unit_hist`. Implemented for `sift_col` (`sift_aggcol.c`).
- SQL: `sift.col_unit_agg(index, leaf)` and `sift.col_unit_hist(index, dim, measure)` (one row per stored unit / group; `exact` is the visibility rule), `sift.col_index_info` columns `preagg_bytes`, `preagg_units`; `sift.col_verify` recomputes sums and cubes from the chunks (C4). Test `col_preagg`: every leaf and every histogram against direct SQL after build, insert, update, delete, VACUUM, promote, merge and a parallel REINDEX; random data with NULLs, `NaN`, `-0`, infinities, int8 near 10^19, negative integers; invalid options. `sift._test_col_pagg_fold` is the benchmark of the metadata path.
- **sift_table owner**: sealed segments already carry the sidecar (same writer). Add the three callbacks over the table's `SiftColSource` (the same `sift_col_unit_*` calls), and make `SiftColVisibility.sb_mask` return all pages set only for a unit whose segment tag (xmin) is visible to the snapshot, that has no delete-vector bit and no newer row version (UPDATE/DELETE invalidate only the SBs they touch); delta pages are in-memory units and scan.

## 4. When a unit's pre-aggregate may answer

1. **Visible and clean** (`unit_exact`): stored unit, no dead row (SB not dirty in the segment's liveness bitmap: VACUUM marks it, a merge rewrites it and recomputes), and every heap page it has rows on is all-visible to the snapshot (VM, as index-only scans; never for a non-MVCC snapshot). Rows inserted later are other units (write buffer, newer segment), scanned by the normal path and added to the result: the aggregate describes exactly its own rows. Inserts, updates and deletes cost only the units they touch.
2. **Predicate covers the unit** (consumer): no predicate; or the predicate on the dimension itself (`=`, `IN`, range: select groups by key value, text by the dictionary); or a zone-covered predicate (`min..max` of the leaf inside the range and `nnull` accounted: the unit answers whole, a disjoint unit is pruned as today). A unit the predicate cuts through is scanned. A predicate on another leaf than the dimension is never answered from a histogram.
3. **Leaf not lossy**: no `SIFT_COLZ_OVERFLOW` in the leaf and unit (`unit_agg` false), no inline-text zone for text min/max.

## 5. PostgreSQL-exact semantics (what the consumer builds)

| aggregate | from | notes |
|---|---|---|
| `count(*)`, `count(x)` | `nvals`, `nvals - nnull`; group `count`, `nn` | `int8` |
| `sum(int2/int4)` | int128 sum | `int8` when it fits (else the unit scans, PostgreSQL would raise); `sum(int8)` is `numeric` (its state is an int128 sum) |
| `sum/avg(numeric(p,s))` | scaled sum / 10^s | every value has dscale s, so the `NumericAggState` (N, sumX, maxScale s) is built from (count, sum); final function is PostgreSQL's |
| `avg(int)` | (count, sum) | build `int8[2]` / `Int128AggState`, call `int4_avg`, `int8_avg` |
| `stddev/variance(int, numeric)` | (count, sum, sumsq) | exact `Int128AggState`/`NumericAggState` with `sumX2`; sumsq overflow: scan |
| `sum/avg(float8)`, `avg(float4)` | unit sums in row order | same order caveat as PostgreSQL's parallel sum (float addition is not associative); refused: `sum(float4)` (float4 accumulator), float `stddev` (Youngs-Cramer `Sxx` is not stored) |
| `min/max` | zone key | integer, date, time, numeric, float (`NaN` last, as PostgreSQL); `-0` vs `0` ties may print either; not text (codes and prefix keys are not order) |

NULL: an all-NULL measure group (`nn = 0`) yields NULL, the group still counts its rows; a NULL key is its own group. Group keys of text are compared bytewise: deterministic collations only (as `SiftAgg` today).

## 6. Consumption in SiftAgg (built)

- **Shapes** (`sift_aggspec_preagg`, `sift_aggspec.c`: the planner costs with it, the executor starts with it). A row level scan of one `sift_col` index (no join, combo, set, arm or unnest) with no key, or one key that is a dictionary text, `int2/int4`, date or bool column the index writes a histogram of (`SiftSrcPreagg`); aggregates `count(*)`, `count(m)`, `sum/avg(int2, int4)`, `sum/avg(numeric)` of a measure `m` of that histogram, `count(key)`, and, without a key, `min/max` of an integer, date or numeric column. Conjuncts: on the key's column (without a key: on one column) any test the executor evaluates (`=`, `IN`, `IS NULL`, `LIKE`, comparisons, expressions of the column) runs on the groups; on any other column only an integer or date comparison that the unit's zone lies inside (and without NULLs) keeps the unit answerable. Everything else is scanned as before: float sums (the order of the additions), `count(DISTINCT)`, `sum(int8)` (PostgreSQL's own aggregate), `min/max` per group, two keys, tests on other columns, text `min/max`.
- **Executor** (`sift_agg.c`, `preagg_unit`, called by `process_unit` first): `sift_colsrc_unit_exact`, the zone test of the other columns, then the unit's sums (no key) or its histogram. A text group is translated to the key id once per dictionary (`dict_kmap`, as reads do), an integer group through the range table (`key_cover`, the part of `key_prepare` it shares); the conjuncts on the key column are evaluated by `eval_pred` on a vector whose rows are the groups; each kept group adds its rows to `seen` and its `PaVal` (rows, non-null rows, exact int128 sum) to the aggregate states of the group table (`preagg_add`: `count`, `sum`/`avg` of int, `sum`/`avg` of numeric in 128 bits, `min`/`max`). A unit that cannot answer (dirty page, write buffer, no histogram for the column in that unit, a measure the histogram lacks, an overflow value) is scanned into the same table, so a mixed table is exact and workers need no change (each folds its share; the blobs merge as before).
- **Cost** (`agg_cost`, `preagg_share`): the share of the units that are exact is the share of all-visible pages (`allvisible_frac`); those units cost `AGG_UNIT_COST + groups * AGG_PREAGG_GROUP` (25 operators per group, about 80 ns), the others the scan as before. From 98% exact no parallel path is offered: starting workers costs 3.1 ms (1 M) to 6.5 ms (10 M) (`BENCH-GAP.md`), more than the answer. Question 04's plan flip (`SiftAgg` against a parallel heap aggregate, 0.9% apart in cost, 10x in time) is gone because the exact units are no longer costed as a scan.
- **Switches and EXPLAIN**: `sift.agg_preagg` (on) turns it off for the plan and the executor; `EXPLAIN ANALYZE` shows `Units From Pre-aggregates` next to `Units` and counts `Rows Scanned` for the scanned units only (the line appears only when the shape qualifies).
- **Tests**: `agg_preagg` (regress: 21 shapes on a default, an explicit-cube and a `preagg = 'off'` index through build, update, delete, VACUUM, write buffer, promote, merge, parallel build, parallel scan, numeric NaN, a rolled-back delete: each shape three ways, pre-aggregates, scan of every unit and the heap, must agree; `pre` says all, some or none of the units answered), the `pa_*` fixtures of `scripts/coldiff.sh` (every variant and heap state), and `docs/coverage.tsv` rows AP01 to AP04.
- **Not done**: a metadata pass of the leader that decides the workers at run time (the planner decides: a table whose pages are not all-visible when planned gets the parallel path, the exact units are still answered by whoever scans them); a depth-2 cube; per-group `min/max`; float sums.

## 7. Acceptance: the ten questions of the store benchmark (10 M rows = 511 units, 1 M = 52)

Times: psql `\timing` client wall time (median of 41 runs, min in brackets), `sift.agg_preagg` on against off, same plan otherwise, in a quiet window: CPU pressure (`/proc/pressure/cpu`, some avg10) 2 to 9% and IO pressure 0% before and after every batch (a batch that went above 10% was repeated). ClickHouse and Elasticsearch are the figures of `BENCH-GAP.md`.

| q | question | from pre-aggregates | 10 M scan -> pre-aggregates / ClickHouse / ES | 3 M scan -> pre-aggregates |
|---|---|---|---|---|
| 01 | one order by id | no (btree) | 0.41 / 9.4 / 2.6 ms | unchanged |
| 02 | orders per country | **yes**: histogram of `country`, 30 groups | 8.0 -> 2.1 [1.7] / 13.2 / 5.7 ms | 8.1 -> 0.60 [0.41] ms |
| 03 | revenue per month | **yes**: histogram of the `date_trunc('month')` leaf, `sum(amount)`, 36 groups | 35.0 -> 1.4 [1.3] / 64.3 / 225 ms | 12.7 -> 0.65 [0.43] ms |
| 04 | top 10 categories by revenue | **yes**: histogram of `category`, `sum(amount)`, 40 groups, sorted above | 46.8 -> 3.6 [3.1] / 48.7 / 268 ms | 14.3 -> 0.64 [0.42] ms |
| 05 | average of shipped, DE, 2025 | no: three predicates on three dimensions, no unit covered (rows are in id order, dates spread) | 39.2 / 13.1 / 12.9 ms | scan (predicate order is the planner track's) |
| 06 | customers per month | no: `count(DISTINCT)` is exact in PostgreSQL; a per-unit set costs as much as the scan | 600 / 151 / 484 ms | scan |
| 07 | orders over 100 in 30 days | no: row-level `amount` compare, no unit covered | 14.2 / 5.8 / 3.1 ms | scan |
| 08 | search count | no (BM25 count) | 3.5 / 4.5 / 3.3 ms | unchanged |
| 09 | search facets | no: the BM25 match set is a row subset; partial units cannot use a histogram | 46.5 / 36.6 / 7.4 ms | match set into `SiftAgg` (other track) |
| 10 | search, newest | no | 46 / 22.9 / 8.6 ms | other track |

Instructions per execution (user space), 10 M: 02 4.76 M -> 0.97 M, 03 5.35 M -> 1.10 M, 04 13.3 M -> 0.29 M (3 M: 1.51 M -> 0.32 M, 1.69 M -> 0.36 M, 4.19 M -> 0.22 M; the counts do not depend on the host). All 511 units (154 at 3 M) are answered from metadata, Rows Scanned 0, results identical to the scan and the heap.

So 02, 03 and 04 are answered entirely from metadata at 10 M: 6x, 46x and 13x faster than ClickHouse and 2.7x, 160x and 74x faster than Elasticsearch (medians). 02 spends 3.3x the instructions of 04 for the same unit count and 04 is the slowest of the three in wall time (top-10 sort over 40 groups above the scan); neither is explained yet, they are the first candidates for the next trim. 05 and 07 stay scans; a depth-2 cube (`country, status, month`: up to 5 400 groups per 19.6 k rows, about 12% of the index) would answer 05 but is not worth it by default. 06 stays the distinct-set work of the planner track.

## 8. Not done, risks

- Time buckets derived inside the writer (`date_trunc` of a timestamp leaf) were rejected: `timestamptz` buckets depend on the session `TimeZone`, month arithmetic is the host's. A leaf that holds the expression (as the benchmark index does) is the dimension; for time-clustered tables the zone cover test already answers units inside one month from the unit sum.
- No composite dimensions (depth 1 only), no per-group `min/max`, no float variance, no sketches. A dimension over the cap in a unit silently has no histogram there: the consumer must scan that unit (it does, `unit_hist` false).
- Format minor 2 must merge with other tracks' bumps (take the highest) and `REINDEX`; the cube costs about +4% serial build CPU and 1.1% bytes (2.4 kB per unit; the budget caps it at 2% of the unit).
- Churn: a unit with a dead row (its liveness bit cleared by VACUUM) or a page that is not all-visible answers from its rows, never from the sums; the correctness never changes. The way back needs no `REINDEX`: `VACUUM` rewrites a lone segment more than `dead_rewrite_pct` percent dead (`sift_col` option, default 10, `ALTER INDEX ... SET`; 100 never; the BM25 index keeps 50), `sift.col_promote` and the worker give the rows written since stored units, and the tier merge rewrites the rest (`col_preagg_maint`). Below the threshold a segment keeps its dirty units on purpose: uniform deletes dirty every unit at 0.1% dead, and each rewrite costs the whole segment. PostgreSQL's `VACUUM` skips the index and leaves the pages not all-visible when few tuples are dead (`INDEX_CLEANUP auto`, under 2% of the pages): units come back at the first `VACUUM` that vacuums the index, as for index-only scans. A `sift_table` has no pre-aggregates yet, so nothing of this applies to it.
- Dictionaries are per segment, parallel builds have one per participant: translation by value is per segment, not per unit.
