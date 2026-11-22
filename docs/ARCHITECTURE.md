# Architecture

Internals of `pg_sift` 6.0: unified Format Major 2 storage (`sift_col` and `sift_table`), two-tier adaptive compression, the 2,048-row SIMD vectorized `SiftAgg` engine, the single-pass planner, refusal slugs, `EXPLAIN` labels, and C memory safety.

## 1. On-Disk Storage: Format Major 2 (`2.0`)

`pg_sift` 6.0 unifies `sift_col` and `sift_table` on a single on-disk layout (**Format Major `2`, Minor `0`**, `SIFT_FORMAT2_MAJOR = 2`, `SIFT_FORMAT2_MINOR = 0`). BM25 full-text indexes (`sift`) remain on **Format `1.7`** so existing search indexes upgrade without rebuilding.

```text
Block 0: SiftColMetaPageData (424 B, CRC32C)
  ├── partdir_root_blk ──▶ 2-Level COW PartitionDirectory (SIFT_PT_PARTDIR)
  │                          └── SIFT_PT_PARTLIST ──▶ Immutable SiftPart containers
  │                                ├── SIFT_PT_PARTHDR + TLV Section Directory
  │                                ├── 8,192-row sorted granules (SEC_ZONES, SEC_GRANULES, SEC_DATA)
  │                                ├── SEC_PREAGG / SEC_PREAGG_ROLLUP / SEC_VARIANT_EXC
  │                                ├── COW Deletion Vectors (SIFT_PT_DELVEC + SIFT_PT_PEND_DEL)
  │                                └── O(1) MVCC [min_xid, max_xid] + auto-frozen SEC_VERS
  ├── scope_dir_blk    ──▶ Dynamic COW Scope Directory (SIFT_PT_SCOPEDIR, 507 scopes/page, chained)
  │                          └── SIFT_SEG_SCOPE_META (L0 slices, L1 nodes, L2 roots, 768 B Top-K, MonthMap)
  ├── proj_dir_blk     ──▶ Chained COW Projection Directory (SIFT_PT_PROJDIR = 44, 63 refs/page)
  ├── tier_dir_blk     ──▶ Chained COW Tier Directory (SIFT_PT_TIERDIR = 45, 84 refs/page)
  └── Block 1: SiftFsmMetaPageData ──▶ 2-Zone Slab Allocator (low-block metadata slab + data extents)
```

### Core Storage Primitives

- **Immutable TLV `SiftPart` containers:** Every part is write-once and verified with CRC32C page checksums. A part holds 8,192-row granules sorted by `key_order` (1..8 columns, encoded into 40-byte big-endian `memcmp` bounds).
- **2-Level COW Manifest:** Partition directories (`SIFT_PT_PARTDIR`) and part lists (`SIFT_PT_PARTLIST`) publish atomically via a single `GenericXLog` metapage swap on Block 0. Readers see consistent snapshots with zero lock contention.
- **2-Zone `SiftFsm` Slab Allocator:** Separates low-block metadata pages from tightly packed data extents (targeting ≥ 95% page utilization), reclaiming obsolete COW pages once `safexid` passes `OldestXmin`.
- **3-Primitive Mutation Lifecycle:**
  1. *Ingest:* Small OLTP batches append to partition-local `SIFT_PT_L0_SLAB` pages; bulk writes (`>= 64` rows/partition or `COPY`) stage in `SiftTxnStage` and flush sorted L0 micro-parts.
  2. *Delete:* Row deletes append to per-part `SIFT_PT_PEND_DEL` logs and fold into immutable COW `SIFT_PT_DELVEC` bitmaps (`sift_dv_mask64`).
  3. *Visibility:* Each part records `[min_xid, max_xid]`. Once `max_xid < OldestXmin`, the part flips all-frozen and drops per-row `SEC_VERS` metadata on compaction.
- **Optional sections for ClickHouse-class scans:** `SEC_KEY_MARKS` (`0x020D`: the first and last composite key of every granule, binary-searched in memory), `SEC_COL_STATS` (`0x020E`: bytes, distinct estimate, and dominant encoding per column, which seed the `PREWHERE` ranking), and `SEC_SCHEMA_DEFAULTS` (`0x0305`: the defaults of columns added later, so `ALTER TABLE ADD/DROP COLUMN` rewrites nothing). None is required to read a part: a reader that does not know one skips it, and a part without it scans as before.
- **`SiftPartDesc` stays 224 B:** the 16-bit `level` is two bytes, `level` (`0` micro-part, `1` warm, `2` cold) and `ext_flags` (`REMOTE`, `KEY_MARKS`, `COL_STATS`, `EXPIRED`), binary-identical to Format 2.0 parts written without them. A `REMOTE` part has `header_blk = InvalidBlockNumber`; `sift_part_open_desc()` and the byte readers dispatch to its object, so executors never see the difference. The metapage records `SIFT_COMPAT_REMOTE_PARTS` (required to read) and `SIFT_ROCOMPAT_PROJECTIONS` / `SIFT_ROCOMPAT_TTL` (required to write) only on a relation that uses the feature.
- **Remote object (`.siftpart`):** `[64 B header][part header image][stream][range table with CRC32C per range][16 B footer]`, addressed with HTTP `Range` requests and no 8 KB page framing.
- **Secondary projections:** each projection is its own chained part directory of derived parts, linked to the base part generation and deletion-vector generation it reflects and committed in the same manifest record as the base change; a stale link means "read the base".
- **Dynamic Scope Directory (`SIFT_PT_SCOPEDIR`):** Replaces fixed metapage scope slots with a chained COW page list (507 scopes per 8 KB page, no on-disk ceiling), storing zero extra pages when an index has no scopes.

## 2. Two-Tier Adaptive Compression and Shared Value Store

- **Tier 1 — Hot L0 (Write-Optimized):** Minimal-CPU encoding for `SIFT_PT_L0_SLAB` and L0 micro-parts to keep OLTP `INSERT` WAL ≤ 220 B/row.
- **Tier 2 — Cold L1/L2 (Analytics-Optimized):** Vertical SIMD bitpacking, Frame-of-Reference (`FOR`), `DELTA`, `RLE`, scaled integers, `ALP` floats, `FSST` string compression, and `LZ4`/`ZSTD` frame compression.
- **Chunked Streamed Dictionaries (`SEC_DICTS`):** Supports dictionary entries up to 1,024 bytes and flushes self-contained, CRC32C-verified dictionary chunks under `SiftMemTracker` pressure instead of failing at 64 MiB.
- **Shared Value Store (`sift_values`):** Normalizes repeated strings across partitions and connected/scoped relations using compact 30-bit codes (`2^30` codes/epoch), automatically triggering background epoch compaction at 75% utilization when `sift.async_maintenance = on`.
- **Connected Parent-Child Protocol (`connect`):** Stores connection metadata dynamically in `SEC_CONN_META` (up to 32 connections per index) with parent insert/delete watermarks so child scans resolve parent leaves directly and detect staleness cleanly.

## 3. 2,048-Row SIMD Vectorized Execution (`SiftAgg`)

`SiftAgg` operates directly on decoded 2,048-row column vectors (`SiftVec`) with bit-packed null masks across `AVX-512`, `AVX2`, `NEON`, and `scalar` kernels (selectable via `sift.simd`, bit-exact across all ISAs):

- **Native fixed-width types:** `SiftI128` (exact 128-bit `NUMERIC` arithmetic), `SiftUuid16` (16-byte `UUID`), and 16-byte `SiftStrView` (inline length + 4-byte prefix + pointer) eliminate PostgreSQL `Datum` boxing overhead inside inner loops.
- **Zero-heap-I/O JSONB variant exceptions:** Mismatched JSONB types on shredded paths spill into a per-granule `SEC_VARIANT_EXC` stream instead of forcing heap page fetches or invalidating clean granules.
- **64-Byte Cache-Line Salted Hash Table:** Group keys hash into a compact SwissTable-style open-addressed table (`SiftAggSlot64`) bounded strictly by `work_mem * hash_mem_multiplier`, spilling cleanly by hash partition when `SiftMemTracker` hits `SIFT_MEM_SOFT`.
- **2-Phase Radix-Partitioned Parallel Merge:** Parallel workers aggregate into thread-local hash tables, radix-partition partial states by group hash, and merge disjoint partitions in parallel — delivering ≥ 3.0× 4-worker scaling on high-cardinality `GROUP BY` and `count(DISTINCT)`.
- **Floating-point determinism:** By default (`sift.agg_exact_float = off`), parallel `float8` sums use fast IEEE-754 summation matching PostgreSQL/ClickHouse/DuckDB. Enable `sift.agg_exact_float = on` for bit-identical row-order determinism.

## 4. Single-Pass Planner and Cost Model

- **Single-pass planning (< 2 ms):** The planner hook inspects each query block once (exactly one `standard_planner()` call) and prices candidate `SiftAgg` paths with a 5-parameter cost model (granules read, pre-agg hits, hash groups, lookup build, parallel merge).
- **Exactness invariant:** Any query shape `SiftAgg` cannot prove semantically equivalent to PostgreSQL's native plan is refused cleanly with a stable refusal slug and executes on PostgreSQL's own planner.

### Refusal Slugs

Inspect refusals with `SET sift.explain_refusals = on`, `EXPLAIN (SIFT)` (PG 18+), or `SELECT * FROM sift.explain_why(query);`. List all slugs with `SELECT * FROM sift.refusal_slugs();`.

<!-- refusals:start -->
| Slug | Class | Reason | Action |
|---|---|---|---|
| `other` | semantic | the shape is not one the aggregate pushdown proves, and no more specific reason is recorded | Try a simpler form of the statement (one table, a plain GROUP BY) to find the part that is refused, and report the statement if it should be accepted. |
| `disabled` | semantic (by design) | sift.enable_agg is off | SET sift.enable_agg = on (it is off in this session, role or database). |
| `query_kind` | semantic (by design) | the query has a set-returning function in the select list, row locks or a recursive CTE | None needed, the normal plan is the answer; take the set-returning function, FOR UPDATE or the recursive CTE out of the statement to get the scan. |
| `from_shape` | semantic | the FROM list is not one table with the inner, left, semi or anti joins and lateral functions the scan reads | Write the FROM list as one table with inner or left joins and lateral functions, or move the other sources into a sub-select the scan can read. |
| `table_kind` | semantic (by design) | the table is inherited, sampled, row-secured or not a plain, materialized or partitioned table | None needed, the normal plan is the answer; the scan reads plain, materialized and partitioned tables that are not inherited, sampled or row-secured. |
| `no_source` | semantic (by design) | no sift_col index or sift_table covers the table | Create a columnar index: CREATE INDEX ON <table> USING sift_col (<filter_and_group_cols>); |
| `partition_source` | semantic | a partition has no column index the plan can use, or the partitions do not share a layout | Create the same sift_col index (same columns and options) on every partition, as CREATE INDEX on the parent does. |
| `partition_count` | limit | the table has more partitions left by PostgreSQL's pruning than a scan reads | Filter on the partition key so that fewer partitions are left, or query the partitions that matter. |
| `join_child` | semantic | a joined sub-select, VALUES list, CTE or function is not one the scan can read or run as a child | Join a table the scan reads, or write the sub-select, VALUES list or CTE as a single-table SELECT with a WHERE; the detail names the child. |
| `outer_join` | semantic | an outer join of the scanned table keeps rows the scan cannot reproduce | Use an inner join if the unmatched rows are not needed, or aggregate the joined table in a sub-select first. |
| `unnest_leaf` | semantic | the array or object a lateral function unnests is not a leaf of the column index | Create a replacement index online with paths containing the array or object (CREATE INDEX CONCURRENTLY ... WITH (paths = '...')), then DROP INDEX CONCURRENTLY the old index. |
| `where` | semantic | a condition of the WHERE or ON clause is not one the scan can test | Test a column of the index with a comparison, IN, BETWEEN, LIKE or IS NULL, or add the column or expression the condition reads to the sift_col index. |
| `jsonb_path_not_declared` | semantic | a containment (@>) or key test (?, ?\|, ?&) on a jsonb column names a path the index does not declare in paths, so no leaf holds its values | Declare the path in the paths of the index (sift.suggest_paths writes the spec), or test a path it declares; the detail names the path. |
| `join_condition` | semantic | a join condition is not an equality or filter the scan can look up | Join on an equality of columns of the scanned table and the other side, or an IN list, and filter the rest in the WHERE. |
| `group_key` | semantic | a group key is not a leaf, or an expression of one, that the scan reads | Add the column the key reads to the sift_col index, or group by the column itself. |
| `too_many_keys` | limit | the query groups by more columns than the scan holds | Group by fewer columns; the detail names the limit. |
| `error_parity` | semantic | the scan could raise, or not raise, an error for a value where the normal plan does the other (a cast of a JSON null before PostgreSQL 18, a guard of jsonb_typeof around a read) | None needed, the normal plan keeps SQL's errors; guard the cast so that it returns NULL, or use PostgreSQL 18, to get the scan. |
| `output_expr` | semantic | the select list or HAVING reads something besides group keys and the aggregates the scan computes | Select the group keys and the aggregates only, and compute the expression over them in an outer query. |
| `aggregate` | semantic | an aggregate, or its FILTER, DISTINCT or ORDER BY, is not one the scan computes | Use aggregates supported by SiftAgg (see docs/AGG.md) without the unsupported clause, or compute the remaining aggregates in an outer query. |
| `distinct_on` | semantic (by design) | DISTINCT ON keeps a row the scan cannot pick: more than four sort columns after the ON columns, a sort or select-list column of a joined table, or a volatile select list | None needed, the normal plan is the answer; keep the sort columns after the ON columns to four and the select list on the scanned table to get the scan. |
| `grouping_sets` | semantic | the arm does not group by grouping sets | Use a plain GROUP BY for this arm; the single-scan arm answers ROLLUP, CUBE and GROUPING SETS. |
| `dirty_units` | semantic | some granule holds values a leaf the query reads cannot store, so the scan cannot guarantee SQL error parity | Inspect sift.col_leaves(<index>) for overflow leaves, fix the data or path type, and rebuild with REINDEX INDEX CONCURRENTLY. |
| `dirty_order` | semantic (by design) | a condition casts a path whose value some granule cannot store, and PostgreSQL may evaluate it before a filter that drops the row | None needed, PostgreSQL's order of the conditions decides; put the guard in a CASE or a sub-select so that it runs first. |
| `volatile` | semantic (by design) | the query calls a volatile function, which PostgreSQL evaluates for every row and the scan cannot reproduce | None needed, the normal plan is the answer; compute the volatile function outside the statement or in an outer query to get the scan. |
| `first_memory` | limit | the first rows of the groups would not fit in the memory of a hash aggregate | Raise work_mem or group by fewer values; the detail names the limit. |
| `group_memory` | limit (by design) | the groups would not fit in the memory of a hash aggregate, and the scan cannot spill them, or the normal plan, which spills them, is estimated cheaper than the scan with its spill | Raise work_mem so that the groups fit a hash aggregate, or leave the normal plan, which spills them. |
| `lateral_limit` | semantic | a LATERAL sub-select with LIMIT 1 stays a child that runs for each key: no unique index holds its key, or its ORDER BY leaves ties | Add a unique index on the key of the LATERAL sub-select, or an ORDER BY that makes its LIMIT 1 unique. |
| `distinct_set` | semantic | count(DISTINCT x) keeps the distinct values of each group: x is not a NOT NULL column of the scanned table under a unique single-column index, so its rows cannot be counted in its place | Count DISTINCT over a NOT NULL column under a unique single-column index, or leave the normal plan. |
| `two_arms` | semantic | the arms of the two level aggregate are not UNION ALL branches of aggregates over one scan, each grouped by a unique row id | Write the arms as UNION ALL branches of aggregates over one scan, each grouped by a unique row id. |
| `two_keys` | semantic | a key of the two level aggregate is not one every arm groups by | Group every arm by the keys the outer aggregate groups by. |
| `two_aggs` | semantic | an aggregate over the arms is not one the two level scan computes | Use sum, count, min or max over the arms, or compute the other aggregate in an outer query. |
| `spine_shape` | semantic | the join of the arms to a spine is not an equality of their keys, or the WHERE reads the arms | Join the arms to the spine on an equality of their keys, and keep the WHERE off the arms. |
| `shape_width` | limit | the query has more joined tables, value combinations, leaves, arms or columns than the scan holds: the detail names the limit and its value | Reduce the joined tables, value combinations, leaves, arms or columns below the limit the detail names. |
| `spine_rows` | limit | the rows of the spine are estimated above half of work_mem | Raise work_mem, or filter the spine to fewer rows. |
| `rows_shape` | semantic | the grouping of the rows of a SiftAgg plan has no count(DISTINCT) of integers, or a key or aggregate it cannot group | Group by integer keys with aggregates the scan computes (count(DISTINCT) of integers), or leave the normal plan. |
| `topn_shape` | semantic | the ORDER BY with LIMIT is not over the scanned table's columns with a condition the scan tests | ORDER BY columns of the scanned table with a WHERE filter the scan tests, followed by LIMIT. |
| `window_first` | semantic | the first rows of each group, asked with a window function (rn = 1, rn <= N), are not a lone row_number(), rank() or dense_rank() with PARTITION BY and ORDER BY in a sub-select that does nothing else, over a table with a column index for N > 1 (rank and dense_rank: up to 1,000) | Write the first rows of each group as a lone row_number(), rank() or dense_rank() OVER (PARTITION BY .. ORDER BY ..) in a sub-select that does nothing else, kept with rn = 1 or rn <= N. |
| `scope` | semantic | a join to the scope relation of a scope index is not one the scope read proves equal to the join: the join runs as written | Join the scope relation by its declared member column to the declared column of the table, bind its scope key with an equality or an IN list of constants, and read no other column of it; the detail names what differs. |
| `cost` | semantic (by design) | the normal plan is estimated cheaper than the scan | None needed, the normal plan is estimated cheaper (compare scan_cost vs normal_cost); run ANALYZE <table> if row estimates are stale. |
| `cte_cost` | semantic (by design) | the shared CTE plan is estimated cheaper than the plan with the CTE inlined | None needed, the cheaper form was chosen; MATERIALIZED or NOT MATERIALIZED on the CTE picks the other. |
<!-- refusals:end -->

### `EXPLAIN` Labels

`EXPLAIN (ANALYZE, SIFT)` (PostgreSQL 18+) and `EXPLAIN (ANALYZE)` report execution counters across `SiftAgg`, `SiftScan`, and `SiftTopN`:

- **Plan & index state:** `Index`, `Sift`, `Sift Index State`, `Sift Segments`, `Sift Buffered Rows`, `Arm`, `Slug`, `Reason`, `SIMD`, `Scan`, `Name`
- **Column & granule I/O:** `Units`, `Units Pruned`, `Units From Pre-aggregates`, `Units Net Of Dead Rows`, `Units With Dead Rows Scanned`, `Units Scanned By Cost`, `Units Read Without Filters`, `Units Read Again From The Heap`, `Units With Exceptions Scanned`, `Units Net Of Exceptions`, `Units Without The Rollup`, `Units Without The Sketch`, `Units Pruned By Set Summary`, `Units From Point Directory`, `Units Filtered By Sorted Run`, `Units Answered By Run Head`, `Units Read By Sorted Run`, `Rows`, `Rows Scanned`, `Tuples Aggregated`, `Rows Read`, `Rows Decided By The Log`, `Rows From Point Directory`, `Heap Fetches`, `Heap Pages`, `Heap Checks`, `Rows Vetted By Heap`, `Rows Read Again From The Heap`, `Partitions Removed`, `Superunits`, `Dense Chunks`
- **Grouping & distinct:** `Group Keys`, `Group Columns`, `Pushed Aggregates`, `Groups`, `Groups Finished`, `Group Partitions`, `Group Passes`, `Group Pass Restarts`, `Group Table Spills`, `Group Stream Batches`, `Distinct Counts`, `Distinct Pairs`, `Distinct Values`, `Distinct Sets Spilled`, `Distinct Chunks Merged`, `Distinct Stream Given Up`, `Distinct Stream Superblocks`, `Ordered Sets Flat`, `Ordered Sets Spilled`, `Approximate Distinct`, `Sets Summed From One`, `Combo Caches Restarted`, `Cell Keys`, `Arms In One Pass`
- **Filters & joins:** `Filters`, `Filters In Source`, `Type Filters In Source`, `Wide Filters In Source`, `Lookup`, `Lookup Joins`, `Lookup Rows Loaded`, `Lookup Rows Read`, `Lookup Rows Shared`, `Lookup Load Time`, `Lookup Column Loads`, `Lookup Index Scans`, `Lookup Heap Fetches`, `Lookup Heap Reason`, `Lookup Pages All Visible`, `Lookup Hint`, `Lookup Rows Classed`, `Lookup Class Workers`, `Cached Lookup Tables`, `Shared Lookup Tables`, `Shared Dictionaries`, `Dimension Maps Built`, `Dimension Maps Shared`, `Semi Lookup Tables`, `Semi Lookup Table Size`, `Join Key Filters`, `Join Key Passes`, `Join Text Filters`, `Parent Leaf Reads`, `Parent Heap Fetches`, `First Row Lookups`, `First Row Lookup Tables`, `First Row Keys Differ`, `Found Proofs`, `Found Fallback`, `Found Keys Outside The Proof`, `Child Blocks Prefetched`, `Child Direct Probes`, `Child Probe Leaves`, `Child Probe Heap Fetches`, `Child Probe Index Only Entries`, `Child Probe Upper Pages Pinned`, `Unnests`
- **Pre-aggregates & Top-N:** `Pre-aggregates`, `Pre-aggregate Dims`, `Pre-aggregate Measures`, `Pre-aggregate Rollup`, `Rollup Refresh Queued`, `Top-N`, `Top-N Candidates`, `Top-N Sorted Rows`, `Top-N Restarts`, `Top-N Run`, `Top-N Units By Order Code`, `Order`, `Global Order`, `Group Order Keys`, `Includes Ties`, `Groups Dropped By Top-N`, `Superblocks Skipped By Top-N`, `Units Read From Their Ends`, `Sort Method`, `Sort Memory`
- **Scopes:** `Scope Index`, `Scope Keys`, `Scope Semantics`, `Scope Columns`, `Scope Fallback`, `Scope Filter Left`, `Scope Units`, `Scope Units Read`, `Scope Units Skipped`, `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched Keys`, `Scope Patched Rows`, `Scope Patched Units`, `Scope Dedupe Keys`, `Scope Members Listed`, `Scope Group Key`, `Scope Delta Rows`
- **Hybrid scans (`SiftHybridScan`, `SiftHybridAgg`):** `Hybrid Direction`, `Hybrid Store`, `Bridge Chunks`, `Bridge TIDs`, `Bridge Exact Pages`, `Bridge Lossy Pages`, `Bridge Point`, `Bridge Empty`, `Granules Seen`, `Granules Skipped`, `Rows Probed`, `Rows Emitted`, `Heap Verifies`, `Lossy Rechecks`
- **Projections:** `Projection Parts`, `Projection Parts Sought`, `Projection Parts Subtracted`, `Projection Base Parts`, `Projection Base Rows`, `Projection Granules Read`, `Projection Granules Skipped`
- **Fused pipelines, star joins and GPU:** `Fused Arms`, `Fused Keys`, `Fused Role`, `Shared Rows Vetted`, `Star Dimensions`, `Star Passes`, `Join Type`, `Scope Read`, `GPU Parts`
- **Parallelism:** `Workers Planned`, `Workers Launched`, `Workers Priced For`, `Serial Because`, `Groups Split By`, `Rows Split By`
- **BM25 Search (`SiftScan`):** `Query`, `Search`, `Search Matches`, `Index Searches`, `Ranker Searches`, `Candidates`, `Blocks Scored`, `Blocks Skipped`, `Superblocks`, `Limit Hint`, `Include Tests`, `Score Filter`, `Key Passes`, `Filter Mask`, `Filter Mask From Matches`, `Filter Mask Up To Matches`, `Filter Superblocks`, `Filter Pages Exact`, `Filter Pages Lossy`

## 5. C Memory Safety and Verification

Because `pg_sift` decodes on-disk bytes inside the PostgreSQL server process, corrupt data must always become a clean `ERRCODE_DATA_CORRUPTED` (`XX001`/`XX002`) error rather than an out-of-bounds read or write:

- **Length-checked `SiftSpan` decoders (`src/core/span.h`):** Every stored offset, length, and varint passes through a bounds-checked span before allocation (validate-before-allocate).
- **Hardened release builds (`SIFT_HARDEN=pkg`):** Release `.deb`/`.rpm`/AUR packages compile with `-fstack-protector-strong`, stack-clash protection, CET/branch protection, `-fstrict-flex-arrays=3`, `_FORTIFY_SOURCE=3`, and full RELRO (< 0.3% CPU overhead).
- **Continuous sanitizer & fuzz verification:** CI runs ASan/UBSan (`SANITIZE=1`), trap UBSan (`SIFT_HARDEN=1`), Valgrind memcheck, libFuzzer decoder targets (`scripts/fuzz.sh`), differential query fuzzers (`test/fuzz/`), PostgreSQL `--enable-cassert` builds with `CATCACHE_FORCE_RELEASE` and `debug_discard_caches = 1`, and automated backend leak detection (`scripts/leakcheck.sh`).
