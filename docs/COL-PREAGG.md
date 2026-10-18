# Pre-aggregates of a unit (metadata-only and low-cardinality aggregation)

Status: the writer, the format, the provider callbacks and the SQL hooks are implemented (column format 1.2); the `SiftAgg` consumption (section 6) is designed here and not yet coded: `src/sift_agg*.c` belong to the planner track.
Files: `src/sift_col_preagg.[ch]` (formats, option, compute, verify; portable), `src/sift_col_seg.c` (written with the unit, read per segment), `src/sift_col_pagg.[ch]` (through the column source), `src/sift_colsrc.h` (`unit_exact`, `unit_agg`, `unit_hist`, `preagg`), test `col_preagg`.

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

## 6. Consumption in SiftAgg (phase 2: mechanical)

- **Shapes**: (a) whole table, no key: `count/sum/avg/min/max/stddev` over leaf aggregates; (b) `GROUP BY <one dimension leaf or an expression the planner already maps to it>` with aggregates inside {`count(*)`, `count(m)`, `sum(m)`, `avg(m)`} of the stored measures; (c) the same with a predicate on the dimension or covering zones (section 4). `HAVING`, `ORDER BY`, `LIMIT`, projections stay above.
- **Matching**: at plan time `sift_colsrc_preagg(index)` lists dimensions and measures; the shape matches only if every pushed aggregate and the key are in one dimension entry (Pinot's rule). Hook: `process_unit()` (`sift_agg.c`) calls `try_preagg_unit(ex)` first: `sift_colsrc_unit_exact`, then `unit_hist(dim)` (false: fall through to the scan of that unit), folds each group into the executor's own group table (a text key is translated once per `SiftSrcDict.id`, as reads already do; integer keys directly) as one row with `count` and the measures' partial states, and returns. Scanned and pre-aggregated units meet in the same table; finalization is unchanged. Units are independent, so morsels, workers and `lanes` need no change.
- **Cost**: unit cost = `groups * AGG_GROUP` for an exact unit (about 3 us measured per unit with 30 to 40 groups: 0.15 ns per row of a 19.6 k row unit against 3 ns scanned) and the existing row cost otherwise; exact fraction estimated from `allvisible_frac` (a probe of the VM at plan time, 1 page per 100 superblocks, is the better input). When the exact fraction is about 1 the path is costed ~0 and planned **without workers**: starting them costs 3.1 ms (1 M) to 6.5 ms (10 M) (docs/BENCH-GAP.md), more than the whole answer; question 04's plan flip (`SiftAgg` against a parallel heap aggregate, 0.9% apart in cost, 10x apart in time) disappears because the path is no longer costed as a scan.
- **Parallel**: if the planner could not know, the leader runs a metadata pass over all units first (cheap: unit head, VM mask, cube) and launches workers only for the units that must scan.
- **EXPLAIN**: `Units: 511 (511 from pre-aggregates, 0 scanned)`, `Rows Scanned: 0`.
- **Tests**: `coldiff` rows with the index built `preagg = 'default'` against the heap for every shape, then after DML (dirty units, write buffer, VACUUM, merge) and with `sift.enable_agg` off.

## 7. Acceptance: the ten questions of the store benchmark (10 M rows = 511 units, 1 M = 52)

Times: measured today (`BENCH-GAP.md`), then the estimate from the measured fold of 154 units (0.43 ms with the SQL round trip, about 3 us per unit) scaled to 511 units (1.5 ms) plus executor start and planning (0.2 ms).

| q | question | from pre-aggregates | 10 M now / ClickHouse / ES | expected 10 M (1 M) |
|---|---|---|---|---|
| 01 | one order by id | no (btree) | 0.41 / 9.4 / 2.6 ms | unchanged |
| 02 | orders per country | **yes**: histogram of `country`, 30 groups | 13.5 / 13.2 / 5.7 ms | about 1.7 ms (0.4) |
| 03 | revenue per month | **yes**: histogram of the `date_trunc('month')` leaf, `sum(amount)`, 36 groups | 51.1 / 64.3 / 225 ms | about 1.8 ms (0.4) |
| 04 | top 10 categories by revenue | **yes**: histogram of `category`, `sum(amount)`, 40 groups, sorted above | 812 (plan flip) / 48.7 / 268 ms | about 1.8 ms (0.4) |
| 05 | average of shipped, DE, 2025 | no: three predicates on three dimensions, no unit covered (rows are in id order, dates spread) | 39.2 / 13.1 / 12.9 ms | scan (predicate order is the planner track's) |
| 06 | customers per month | no: `count(DISTINCT)` is exact in PostgreSQL; a per-unit set costs as much as the scan | 600 / 151 / 484 ms | scan |
| 07 | orders over 100 in 30 days | no: row-level `amount` compare, no unit covered | 14.2 / 5.8 / 3.1 ms | scan |
| 08 | search count | no (BM25 count) | 3.5 / 4.5 / 3.3 ms | unchanged |
| 09 | search facets | no: the BM25 match set is a row subset; partial units cannot use a histogram | 46.5 / 36.6 / 7.4 ms | match set into `SiftAgg` (other track) |
| 10 | search, newest | no | 46 / 22.9 / 8.6 ms | other track |

So three of ten (02, 03, 04) are answered entirely from metadata, each about 8x (02) to 36x (03) faster than ClickHouse and 2x faster than a cached Elasticsearch (3.9, 4.2, 3.8 ms). 05 and 07 stay scans; a depth-2 cube (`country, status, month`: up to 5 400 groups per 19.6 k rows, about 12% of the index) would answer 05 but is not worth it by default. 06 stays the distinct-set work of the planner track.

## 8. Not done, risks

- Time buckets derived inside the writer (`date_trunc` of a timestamp leaf) were rejected: `timestamptz` buckets depend on the session `TimeZone`, month arithmetic is the host's. A leaf that holds the expression (as the benchmark index does) is the dimension; for time-clustered tables the zone cover test already answers units inside one month from the unit sum.
- No composite dimensions (depth 1 only), no per-group `min/max`, no float variance, no sketches. A dimension over the cap in a unit silently has no histogram there: the consumer must scan that unit (it does, `unit_hist` false).
- Format minor 2 must merge with other tracks' bumps (take the highest) and `REINDEX`; the cube costs about +4% serial build CPU and 1.1% bytes (2.4 kB per unit; the budget caps it at 2% of the unit).
- Heavy UPDATE/DELETE traffic makes units inexact until VACUUM + merge rewrite them: the benefit decays with churn and returns after the merge, the correctness never changes.
- Dictionaries are per segment, parallel builds have one per participant: translation by value is per segment, not per unit.
