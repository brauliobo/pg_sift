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
  ├── jrnl_blk         ──▶ Key Journal of a connected parent (SIFT_PT_KEYJRNL = 46: a header and 16 record pages, 508 records/page, appended in place)
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
- **Connected Parent-Child Protocol (`connect`):** A parent index keeps a key journal (Format 2.0: `SIFT_PT_KEYJRNL` pages named by the metapage, WAL-logged like every page; Format 1: the tombstone log). Each `UPDATE` or `DELETE` of the parent table appends the key of the version it ended through the `track_changes` row trigger, each insert counts its rows; a record carries the transaction id, so an aborted writer leaves nothing a reader counts. In Format 1 a dimension map is stamped with the lineage of the journal and the `xmin` it was read under, and a later statement shares it when the journal vouches for its snapshot (lineage, trim, completeness of the triggers) and no key changed or row came in since; the Format 2.0 journal answers the same questions (`col/am/conn_route.h`), and the Format 2.0 aggregate (`SiftAggV6`, `agg/dim_cache.c`) keeps the table of a star dimension that is a plain scan of a journaled parent, backend local under the cache budget of `core/cachebudget.h`, stamped before it is read and asked again after it: a statement reads it as kept when the journal vouches for its snapshot and no row came in and no key changed since, and a write of the parent (committed since the stamp, or the statement's own transaction's), an invalidation of the `pg_class` row of the table or its index, a row level security table, another user or a filter that is not immutable make it read the plan. A full record page is emptied and its trim says up to which transaction its records went; VACUUM and `sift.maintain()` drop the records of aborted and aged transactions. A Format 1 child keeps the per-row found flag of each connection (up to 32); a Format 2.0 child keeps none and joins through the parent lookup the journal vouches for.

## 3. 2,048-Row SIMD Vectorized Execution (`SiftAgg`)

`SiftAgg` operates directly on decoded 2,048-row column vectors (`SiftVec`) with bit-packed null masks across `AVX-512`, `AVX2`, `NEON`, and `scalar` kernels (selectable via `sift.simd`, bit-exact across all ISAs):

- **Native fixed-width types:** `SiftI128` (exact 128-bit `NUMERIC` arithmetic), `SiftUuid16` (16-byte `UUID`), and 16-byte `SiftStrView` (inline length + 4-byte prefix + pointer) eliminate PostgreSQL `Datum` boxing overhead inside inner loops.
- **Zero-heap-I/O JSONB variant exceptions:** Mismatched JSONB types on shredded paths spill into a per-granule `SEC_VARIANT_EXC` stream instead of forcing heap page fetches or invalidating clean granules.
- **64-Byte Cache-Line Salted Hash Table:** Group keys hash into a compact SwissTable-style open-addressed table (`SiftAggSlot64`) bounded strictly by `work_mem * hash_mem_multiplier`, spilling cleanly by hash partition when `SiftMemTracker` hits `SIFT_MEM_SOFT`.
- **2-Phase Radix-Partitioned Parallel Merge:** Parallel workers aggregate into thread-local hash tables, radix-partition partial states by group hash, and merge disjoint partitions in parallel — delivering ≥ 3.0× 4-worker scaling on high-cardinality `GROUP BY` and `count(DISTINCT)`.
- **Floating-point determinism:** By default (`sift.agg_exact_float = off`), the `float8` `sum` and `avg` of a Format Major 2 scan keep the three top 26-bit bins of every number they add as integer sums, so the answer has the same bits for any order of the rows and any number of workers (it can differ from the heap's row-order sum in the last digit); a sum of finite numbers that overflows raises `22003` as PostgreSQL's does. Enable `sift.agg_exact_float = on` for the row-order sum of one process.

## 4. Single-Pass Planner and Cost Model

- **Single-pass planning (< 2 ms):** The planner hook inspects each query block once (exactly one `standard_planner()` call) and prices candidate `SiftAgg` paths with a 5-parameter cost model (granules read, pre-agg hits, hash groups, lookup build, parallel merge).
- **Exactness invariant:** Any query shape `SiftAgg` cannot prove semantically equivalent to PostgreSQL's native plan is refused cleanly with a stable refusal slug and executes on PostgreSQL's own planner.

### Refusal Slugs

Inspect refusals with `SET sift.explain_refusals = on`, `EXPLAIN (SIFT)` (PG 18+), or `SELECT * FROM sift.explain_why(query);`. The reason of an arm names, in parentheses, the expression of the statement it refused (a key, an aggregate) or the capability its format lacks. List all slugs with `SELECT * FROM sift.refusal_slugs();`.

<!-- refusals:start -->
| Slug | Class | Reason | Action |
|---|---|---|---|
| `other` | semantic | the shape is not one the aggregate pushdown proves, and no more specific reason is recorded | Try a simpler form of the statement (one table, a plain GROUP BY) to find the part that is refused, and report the statement if it should be accepted. |
| `no_source` | semantic (by design) | no sift_col index or sift_table covers the table | Create a columnar index: CREATE INDEX ON <table> USING sift_col (<filter_and_group_cols>); |
| `where` | semantic | a condition of the WHERE or ON clause is not one the scan can test | Test a column of the index with a comparison, IN, BETWEEN, LIKE or IS NULL, or add the column or expression the condition reads to the sift_col index. |
| `aggregate` | semantic | an aggregate, or its FILTER, DISTINCT or ORDER BY, is not one the scan computes | Use aggregates supported by SiftAgg (see docs/AGG.md) without the unsupported clause, or compute the remaining aggregates in an outer query. |
| `key` | semantic | a GROUP BY key, a grouping set or an output of the statement is not one the scan computes | Group by columns of the index or expressions of them (date_trunc, date_bin, arithmetic), and compute the other outputs in an outer query. |
| `jsonb` | semantic | the statement reads both the JSON type and the value of a key, and the leaf of the key cannot hold the values of the other types its type test leaves out | Test the type in the WHERE clause (jsonb_typeof(doc -> 'k') = 'number'), whose scan keys drop the other rows before the value is read. |
| `json_order` | semantic | a cast of a JSON value that can raise would run for a row the normal plan tests first and drops | Guard the cast with a type test of the same key (jsonb_typeof(doc -> 'k') = 'number'), or declare the path with the type it is cast to. |
| `format` | semantic | the format of the index or sift_table lacks a capability the statement needs | Rebuild the index or sift_table in the current format (REINDEX, VACUUM FULL), or compute the part the detail names in an outer query. |
| `scope` | semantic | a join to the scope relation of a scope index is not one the scope read proves equal to the join: the join runs as written | Join the scope relation by its declared member column to the declared column of the table, bind its scope key with an equality or an IN list of constants or parameters, and read no other column of it; the detail names what differs. |
| `cost` | semantic (by design) | the normal plan is estimated cheaper than the scan | None needed, the normal plan is estimated cheaper (compare scan_cost vs normal_cost); run ANALYZE <table> if row estimates are stale. |
<!-- refusals:end -->

### `EXPLAIN` Labels

`EXPLAIN (ANALYZE, SIFT)` (PostgreSQL 18+) and `EXPLAIN (ANALYZE)` report execution counters across `SiftAgg`, `SiftScan`, and `SiftTopN`:

- **Plan & index state:** `Index`, `Sift`, `Sift Index State`, `Sift Segments`, `Sift Buffered Rows`, `Arm`, `Slug`, `Reason`, `SIMD`, `Scan`, `Name`, `Text Index`
- **Column & granule I/O:** `Units`, `Units Pruned`, `Units From Pre-aggregates`, `Units Net Of Dead Rows`, `Units With Dead Rows Scanned`, `Units Scanned By Cost`, `Units Read Without Filters`, `Units Read Again From The Heap`, `Units With Exceptions Scanned`, `Units Net Of Exceptions`, `Units Without The Rollup`, `Units Without The Sketch`, `Units Pruned By Set Summary`, `Units From Point Directory`, `Units Filtered By Sorted Run`, `Units Answered By Run Head`, `Units Read By Sorted Run`, `Rows`, `Rows Scanned`, `Tuples Aggregated`, `Rows Read`, `Rows Decided By The Log`, `Rows From Point Directory`, `Heap Fetches`, `Heap Pages`, `Heap Checks`, `Rows Vetted By Heap`, `Rows Read Again From The Heap`, `Partitions Removed`, `Superunits`, `Dense Chunks`
- **Grouping & distinct:** `Group Keys`, `Group Columns`, `Pushed Aggregates`, `Groups`, `Groups Finished`, `Group Partitions`, `Group Passes`, `Group Pass Restarts`, `Group Table Spills`, `Group Stream Batches`, `Distinct Counts`, `Distinct Pairs`, `Distinct Values`, `Distinct Sets Spilled`, `Distinct Chunks Merged`, `Distinct Stream Given Up`, `Distinct Stream Superblocks`, `Ordered Sets Flat`, `Ordered Sets Spilled`, `Approximate Distinct`, `Sets Summed From One`, `Combo Caches Restarted`, `Cell Keys`, `Arms In One Pass`
- **Filters & joins:** `Filters`, `Join Keys Pushed`, `Filters In Source`, `Type Filters In Source`, `Wide Filters In Source`, `Lookup`, `Lookup Joins`, `Lookup Rows Loaded`, `Lookup Rows Read`, `Lookup Rows Shared`, `Lookup Load Time`, `Lookup Column Loads`, `Lookup Index Scans`, `Lookup Heap Fetches`, `Lookup Heap Reason`, `Lookup Pages All Visible`, `Lookup Hint`, `Lookup Rows Classed`, `Lookup Class Workers`, `Cached Lookup Tables`, `Shared Lookup Tables`, `Shared Dictionaries`, `Dimension Maps Built`, `Dimension Maps Shared`, `Semi Lookup Tables`, `Semi Lookup Table Size`, `Join Key Filters`, `Join Key Passes`, `Join Text Filters`, `Parent Leaf Reads`, `Parent Heap Fetches`, `First Row Lookups`, `First Row Lookup Tables`, `First Row Keys Differ`, `Found Proofs`, `Found Fallback`, `Found Keys Outside The Proof`, `Child Blocks Prefetched`, `Child Direct Probes`, `Child Probe Leaves`, `Child Probe Heap Fetches`, `Child Probe Index Only Entries`, `Child Probe Upper Pages Pinned`, `Unnests`, `Unnest Value`, `Map Keys`
- **Pre-aggregates & Top-N:** `Pre-aggregates`, `Pre-aggregate Dims`, `Pre-aggregate Measures`, `Pre-aggregate Rollup`, `Rollup Refresh Queued`, `Top-N`, `Top-N Candidates`, `Top-N Sorted Rows`, `Top-N Restarts`, `Top-N Run`, `Top-N Units By Order Code`, `Order`, `Global Order`, `Group Order Keys`, `Includes Ties`, `Groups Dropped By Top-N`, `Superblocks Skipped By Top-N`, `Units Read From Their Ends`, `Sort Method`, `Sort Memory`
- **Scopes:** `Scope Index`, `Scope Cube`, `Scope Keys`, `Scope Semantics`, `Scope Columns`, `Scope Fallback`, `Scope Filter Left`, `Scope Units`, `Scope Units Read`, `Scope Units Skipped`, `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched Keys`, `Scope Patched Rows`, `Scope Patched Units`, `Scope Dedupe Keys`, `Scope Members Listed`, `Scope Group Key`, `Scope Delta Rows`, `Scope Granules Kept`, `Scope Granules Total`
- **Hybrid scans (`SiftHybridScan`, `SiftHybridAgg`):** `Hybrid Direction`, `Hybrid Store`, `Bridge Chunks`, `Bridge TIDs`, `Bridge Exact Pages`, `Bridge Lossy Pages`, `Bridge Point`, `Bridge Empty`, `Granules Seen`, `Granules Skipped`, `Rows Probed`, `Rows Emitted`, `Heap Verifies`, `Lossy Rechecks`, `Extended Predicates`, `Parts Skipped`, `Rows Folded`, `Heap Vet Rows`, `Heap Decides Rows`, `Heap Rows`, `Group Mode`
- **Projections:** `Projection Parts`, `Projection Parts Sought`, `Projection Parts Subtracted`, `Projection Base Parts`, `Projection Base Rows`, `Projection Granules Read`, `Projection Granules Skipped`
- **Fused pipelines, star joins and GPU:** `Fused Arms`, `Fused Keys`, `Fused Role`, `Shared Rows Vetted`, `Star Dimensions`, `Star Passes`, `Shared Dimensions`, `Join Type`, `Scope Read`, `GPU Parts`
- **DISTINCT ON (`SiftAggV6First`):** `Distinct On Columns`, `Order Columns`, `Rows Read`, `Groups`, `Spilled Rows`, `Passes`
- **Parallelism:** `Workers Planned`, `Workers Launched`, `Workers Priced For`, `Parallel Strategy`, `Serial Because`, `Groups Split By`, `Rows Split By`
- **BM25 Search (`SiftScan`):** `Query`, `Search`, `Search Matches`, `Index Searches`, `Ranker Searches`, `Candidates`, `Blocks Scored`, `Blocks Skipped`, `Superblocks`, `Limit Hint`, `Include Tests`, `Score Filter`, `Key Passes`, `Filter Mask`, `Filter Mask From Matches`, `Filter Mask Up To Matches`, `Filter Superblocks`, `Filter Pages Exact`, `Filter Pages Lossy`

## 5. C Memory Safety and Verification

Because `pg_sift` decodes on-disk bytes inside the PostgreSQL server process, corrupt data must always become a clean `ERRCODE_DATA_CORRUPTED` (`XX001`/`XX002`) error rather than an out-of-bounds read or write:

- **Length-checked `SiftSpan` decoders (`src/core/span.h`):** Every stored offset, length, and varint passes through a bounds-checked span before allocation (validate-before-allocate).
- **Hardened release builds (`SIFT_HARDEN=pkg`):** Release `.deb`/`.rpm`/AUR packages compile with `-fstack-protector-strong`, stack-clash protection, CET/branch protection, `-fstrict-flex-arrays=3`, `_FORTIFY_SOURCE=3`, and full RELRO (< 0.3% CPU overhead).
- **Continuous sanitizer & fuzz verification:** CI runs ASan/UBSan (`SANITIZE=1`), trap UBSan (`SIFT_HARDEN=1`), Valgrind memcheck, libFuzzer decoder targets (`scripts/fuzz.sh`), differential query fuzzers (`test/fuzz/`), PostgreSQL `--enable-cassert` builds with `CATCACHE_FORCE_RELEASE` and `debug_discard_caches = 1`, and automated backend leak detection (`scripts/leakcheck.sh`).
