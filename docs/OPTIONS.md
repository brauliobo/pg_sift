# Options and Tuning

`pg_sift` 6.0 ships **63 total configuration knobs**: 27 server settings (`sift.*`, 4 of them for the opt-in [Vulkan backend](GPU.md)), 12 `sift_col` reloptions, 14 `sift` (BM25) reloptions, and 10 `sift_table` options (5 core storage options plus the 5 analytical options it shares with `sift_col`: `scope`, `connect`, `preagg`, `sketches`, `skip_index`). Most workloads run well with defaults, and most tables set none.

**Everything that is strictly better is automatic and has no knob:** compression codec choice (FSST, ALP, byte-stream-split, dictionaries, sparse and 1-bit encodings), late materialization and direct-on-compressed filtering, per-granule skipping indexes (dictionary, small-integer bitset, exact set, and medium-cardinality Bloom), hybrid heap/column scans (including `SiftHybridScan` of a `sift` BM25 index with a `sift_col` index or a `sift_table`) and runtime join filters, parallel build and compaction sizing, read streams and asynchronous I/O batching, time-bound pruning, range partitioning by `key_order` (the 5.x `key_cut` option is gone), change tracking by heap address on any table (no primary key needed), and primary-parent scope clustering. `sift.parallel_scan` is `on` by default and is only a kill switch.

## The 8 Knobs Worth Touching

| Knob | Default | When to change it |
|---|---|---|
| `key_order` (`sift_col` / `sift_table`) | `''` | Set 1..8 sort/partition columns (`tenant_id, created_at`) for date-range or tenant pruning |
| `paths` (`sift_col` / `sift_table`) | `''` | Declare typed JSONB paths (`'auto'` or explicit) to shred JSONB into columnar leaves |
| `scope` (`sift_col` / `sift_table`) | `''` | Declare M:N tenant/hierarchy scopes (`name: fk -> rel(member, key) [cluster]`) |
| `sift.backend_cache_kb` | `16384` (16 MB) | Lower to `4096`–`8192` for 500+ pooled connections; raise for heavy analytical sessions |
| `sift.shared_cache_kb` | `65536` (64 MB) | Raise to `128MB`–`256MB` when many backends share lookup tables and string dictionaries |
| `sift.enable_agg` | `on` | Set `off` per session to verify native PG equivalence or bypass `SiftAgg` in an incident |
| `sift.explain_refusals` | `off` | Set `on` (or use `EXPLAIN (SIFT)` on PG 18+) to diagnose why a query stayed on the heap |
| `sift.maintenance_wal_rate` | `0` (unlimited) | Throttle background merge/defrag WAL generation (kB/s) on write-heavy replicas |

## Server Settings

27 release GUCs (`sift.*`): 23 core settings and the 4 `sift.gpu*` settings of the opt-in Vulkan backend ([GPU.md](GPU.md)). Set via `SET`, `ALTER DATABASE`, `ALTER ROLE`, or `ALTER SYSTEM`. Query and maintenance memory obey PostgreSQL's `work_mem`, `hash_mem_multiplier`, and `maintenance_work_mem` directly.

<!-- sift-settings:start -->
| Setting | Default | Range | Context | Purpose |
|---|---|---|---|---|
| `sift.agg_exact_float` | `off` | `on`, `off` | session | Force bit-identical row-order float8 summation instead of parallel IEEE-754 sums ([ARCHITECTURE.md](ARCHITECTURE.md#3-2048-row-simd-vectorized-execution-siftagg)) |
| `sift.async_maintenance` | `on` | `on`, `off` | superuser | Run the background maintenance scheduler: L0 flush, compaction, scope refresh, and sift_table defrag ([OPERATIONS.md](OPERATIONS.md#5-emergency-switches)) |
| `sift.backend_cache_kb` | `16384` (16 MB) | `0..max` kB | superuser | Hard per-backend LRU cache cap (-1 unlimited removed in 6.0) ([OPERATIONS.md](OPERATIONS.md#1-the-3-budget-o1-memory-model)) |
| `sift.count_refusals` | `off` | `on`, `off` | session | Record aggregate refusal counts in sift.stats() ([ARCHITECTURE.md](ARCHITECTURE.md#refusal-slugs)) |
| `sift.dead_rewrite_pct` | `15` | `1..100` | session | Default dead-row percentage triggering compaction of sift_col and sift_table (per relation: dead_rewrite_pct) ([OPERATIONS.md](OPERATIONS.md#2-disk-compression-and-wal-sizing)) |
| `sift.enable_agg` | `on` | `on`, `off` | session | Allow planner to select Custom Scan (SiftAgg) and Top-N pushdown ([ANALYTICS.md](ANALYTICS.md#1-designing-a-sift_col-index)) |
| `sift.enable_col_scan` | `on` | `on`, `off` | session | Allow planner to use sift_col bitmap and index-only scans ([ANALYTICS.md](ANALYTICS.md#6-bitmap-and-index-only-scans-siftcol__ops)) |
| `sift.enable_scan` | `on` | `on`, `off` | session | Allow planner to use sift BM25 ranked and count(*) index scans ([SEARCH.md](SEARCH.md#1-creating-a-sift-index-and-include-filters)) |
| `sift.explain_refusals` | `off` | `on`, `off` | session | Emit NOTICE explaining why a candidate query kept PostgreSQL's native plan ([ARCHITECTURE.md](ARCHITECTURE.md#refusal-slugs)) |
| `sift.filter_pushdown` | `on` | `off`, `on`, `force` | session | Push bitmap filters from other indexes into BM25 ranked scans ([SEARCH.md](SEARCH.md#1-creating-a-sift-index-and-include-filters)) |
| `sift.gpu` | `off` | `off`, `auto`, `on` | superuser | Run the integer aggregates of a SiftAgg scan on a Vulkan device (opt-in; `make VULKAN=1`) ([GPU.md](GPU.md#settings)) |
| `sift.gpu_cache_mb` | `256 MB` | `16..max` MB | superuser | Device memory per backend for column chunks kept on the Vulkan device between queries ([GPU.md](GPU.md#settings)) |
| `sift.gpu_device` | `''` | index or name part | superuser | Vulkan device used by sift.gpu: its index or part of its name (empty: the best one) ([GPU.md](GPU.md#settings)) |
| `sift.gpu_min_rows` | `1000000` | `0..max` | session | Index rows above which sift.gpu = auto sends a grouped aggregate to the device ([GPU.md](GPU.md#settings)) |
| `sift.log_min_duration` | `1000 ms` | `-1..max` ms | superuser | Log background maintenance passes taking at least this long (-1 disables) ([OPERATIONS.md](OPERATIONS.md#5-emergency-switches)) |
| `sift.maintenance_cost_delay` | `0` | `0..100` ms | superuser | Sleep delay per maintenance I/O batch to limit background CPU/disk impact ([OPERATIONS.md](OPERATIONS.md#2-disk-compression-and-wal-sizing)) |
| `sift.maintenance_naptime` | `2000 ms` | `200..60000` ms | reload | Base interval and backoff floor for background maintenance workers ([OPERATIONS.md](OPERATIONS.md#5-emergency-switches)) |
| `sift.maintenance_wal_rate` | `0` | `0..1048576` kB/s | superuser | Max WAL rate for background compaction and sift_table defrag ([OPERATIONS.md](OPERATIONS.md#2-disk-compression-and-wal-sizing)) |
| `sift.max_expansions` | `512` | `1..1000000` | superuser | Max terms expanded by a wildcard, prefix, regex, or fuzzy BM25 leaf before merging ([SEARCH.md](SEARCH.md#2-siftql-query-syntax)) |
| `sift.max_maint_workers` | `2` | `0..8` | reload | Cap on the dynamic pool of background maintenance workers ([OPERATIONS.md](OPERATIONS.md#5-emergency-switches)) |
| `sift.on_corruption` | `quarantine` | `quarantine`, `error` | superuser | Quarantine damaged indexes and fall back to heap, or raise errors immediately ([OPERATIONS.md](OPERATIONS.md#6-quarantine-and-corruption-recovery)) |
| `sift.parallel_scan` | `on` | `on`, `off` | session | Allow parallel scan, aggregation, build, and compaction plans (bounded by max_parallel_workers_per_gather) ([ARCHITECTURE.md](ARCHITECTURE.md#3-2048-row-simd-vectorized-execution-siftagg)) |
| `sift.rls_scores` | `off` | `on`, `off` | superuser | Permit BM25 ranked scans under row-level security ([SEARCH.md](SEARCH.md#8-row-level-security-siftrls_scores)) |
| `sift.shared_cache_kb` | `65536` (64 MB) | `0..max` kB | superuser | Unified server-wide DSM cap across lookup tables, dim maps, dicts, and leaf stats ([OPERATIONS.md](OPERATIONS.md#1-the-3-budget-o1-memory-model)) |
| `sift.simd` | `auto` | `auto`, `scalar`, `avx2`, `avx512`, `neon` | session | Override runtime SIMD kernel selection for testing or diagnostics ([ARCHITECTURE.md](ARCHITECTURE.md#3-2048-row-simd-vectorized-execution-siftagg)) |
| `sift.standby_conflicts` | `on` | `on`, `off` | superuser | Delay extent reuse on primary to reduce retryable 40001 conflicts on standbys ([OPERATIONS.md](OPERATIONS.md#7-crash-safety-locks-and-replication)) |
| `sift.vacuum_rewrite_limit` | `128 MB` | `0..1048576` MB | session | Max bytes VACUUM rewrites per pass on a single index (0 = unlimited) ([OPERATIONS.md](OPERATIONS.md#2-disk-compression-and-wal-sizing)) |
<!-- sift-settings:end -->

## Index Reloptions

Passed in `CREATE INDEX ... WITH (...)` or modified with `ALTER INDEX ... SET (...)`.

<!-- sift-reloptions:start -->
### Column Index (`sift_col`) — 12 Reloptions

| Option | Default | Values | Purpose |
|---|---|---|---|
| `compression` | `''` | `auto`, `lz4`, `zstd`, `none`, `col=codec` | Compression codec override for L1/L2 parts ([ARCHITECTURE.md](ARCHITECTURE.md#2-two-tier-adaptive-compression-and-shared-value-store)) |
| `connect` | `''` | `'fk -> [schema.]parent(key)'` | Connect up to 32 parent sift_col indexes for heap-free parent joins ([ANALYTICS.md](ANALYTICS.md#5-joins-set-filters-and-connect)) |
| `dead_rewrite_pct` | `0` | `0..100` | Dead-row percentage triggering compaction (0 inherits sift.dead_rewrite_pct, 100 = never) ([OPERATIONS.md](OPERATIONS.md#2-disk-compression-and-wal-sizing)) |
| `key_order` | `''` | `'c1 [DESC], ...'` (1..8), `auto`, `off` | Physical sort and partition key inside Format Major 2 parts ([ANALYTICS.md](ANALYTICS.md#2-physical-sort-and-partitioning-key_order)) |
| `paths` | `''` | `auto` or path spec | JSONB shredding paths ([ANALYTICS.md](ANALYTICS.md#4-shredding-jsonb-columns-paths)) |
| `preagg` | `''` | `auto`, `off`, `cube(...); rollup(...); calendar(...)` | Granule pre-aggregate cube, rollup, and calendar definitions ([ANALYTICS.md](ANALYTICS.md#3-pre-aggregates-preagg)) |
| `projection` | `''` | tag string | Named projection registry tag ([REFERENCE.md](REFERENCE.md#column-index-and-scopes)) |
| `scope` | `''` | `'name: fk -> rel(member, key) [cluster] [topk(...)] [null_filter(...)]'` | Entity-Once multi-tenant/hierarchy scope declarations with automatic primary-parent clustering ([SCOPES.md](SCOPES.md#1-declaring-a-scope)) |
| `sketches` | `''` | `'hll(col, ...); kll(col, ...)'` | Opt-in approximate sketches for sift.approx_count_distinct() and quantiles ([ANALYTICS.md](ANALYTICS.md#8-approximate-distinct-counts-siftapprox_count_distinct)) |
| `skip_index` | `''` | `'column: bloom'`, `'column: trigram'`, comma separated | High-cardinality skipping index per column: Bloom for equality and IN, trigram for LIKE and ILIKE ([ANALYTICS.md](ANALYTICS.md#1-designing-a-sift_col-index)) |
| `track_changes` | `'auto'` | `auto`, `off` | Heap-address (ctid) change tracking for COW deletion vectors; works on any table, no primary key needed ([ANALYTICS.md](ANALYTICS.md#9-change-tracking-track_changes)) |
| `vacuum_truncate` | `true` | `true`, `false` | Truncate reclaimed trailing blocks during VACUUM ([OPERATIONS.md](OPERATIONS.md#34-space-free-extents-and-vblocks_left-siftspace)) |

### Full-Text Search Index (`sift`) — 14 Reloptions

| Option | Default | Values | Purpose |
|---|---|---|---|
| `accent_folding` | `true` | `true`, `false` | Fold diacritics (defaults off with pg_ts_config) ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `analyzer` | `text` | `text`, `keyword` | Word tokenizer or whole-string keyword analyzer ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `b` | `0.75` | `0.0..1.0` | Okapi BM25 document length normalization factor ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `case_folding` | `true` | `true`, `false` | Lowercase tokens during indexing and querying ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `dead_rewrite_pct` | `25` | `1..100` | Dead-document percentage triggering segment rewrite during VACUUM ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `fields` | `''` | comma-separated field names | Named sections for sift.sections() and sift.field_query() ([SEARCH.md](SEARCH.md#6-weighted-fields-siftsections-and-siftfield_query)) |
| `k1` | `1.2` | `0.0..1000.0` | Okapi BM25 term-frequency saturation factor ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `merge_fanin` | `4` | `2..64` | Number of BM25 segments merged per compaction pass ([OPERATIONS.md](OPERATIONS.md#2-disk-compression-and-wal-sizing)) |
| `positions` | `true` | `true`, `false` | Store word positions in postings (false shrinks index; phrases recheck heap) ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `prefix_index` | `0` | `0..15` | Pre-index token prefixes of length 1..N for fast autocomplete ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `stemmer` | `''` | `'en'` or `pg_ts_config` name | Snowball English or any PostgreSQL text search configuration ([SEARCH.md](SEARCH.md#5-analyzers-stemmers-and-prefixes)) |
| `vacuum_segments` | `2` | `1..128` | Target BM25 segment count after VACUUM ([OPERATIONS.md](OPERATIONS.md#2-disk-compression-and-wal-sizing)) |
| `vacuum_truncate` | `true` | `true`, `false` | Truncate reclaimed trailing blocks during VACUUM ([OPERATIONS.md](OPERATIONS.md#34-space-free-extents-and-vblocks_left-siftspace)) |
| `write_buffer_limit` | `4096` | `64..1048576` kB | Write-buffer flush threshold before sealing an immutable segment ([SEARCH.md](SEARCH.md#1-creating-a-sift-index-and-include-filters)) |
<!-- sift-reloptions:end -->

## Table Options (`sift_table`)

Set per table via `SELECT sift.table_set_option(rel, name, value);`, read with `sift.table_get_option`, and reset with `sift.table_reset_option`.

<!-- sift-table-options:start -->
| Option | Default | Values | Purpose |
|---|---|---|---|
| `auto_defrag` | `on` | `true`, `false` | Background compaction toggle (inherits sift.async_maintenance) ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `compression` | `''` | `auto`, `lz4`, `zstd`, `none`, `col=codec` | Per-table or per-column compression codec override ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `connect` | `''` | `'fk -> [schema.]parent(key)'` | Connect up to 32 parent sift_col indexes for heap-free parent joins ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `dead_rewrite_pct` | `0` | `0..100` | Dead-row percentage triggering partition compaction (0 inherits sift.dead_rewrite_pct) ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `key_order` | `''` | `'c1 [DESC], ...'` (1..8) | Online physical sort and partition key ([TABLES.md](TABLES.md#4-online-key_order-and-partition-management)) |
| `paths` | `''` | `auto` or path spec | JSONB shredding paths ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `preagg` | `''` | `auto`, `off`, `cube(...); rollup(...); calendar(...)` | Granule pre-aggregate cube, rollup, and calendar definitions ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `scope` | `''` | `'name: fk -> rel(member, key) [cluster] [topk(...)] [null_filter(...)]'` | Entity-Once multi-tenant/hierarchy scope declarations with automatic primary-parent clustering ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `sketches` | `''` | `'hll(col, ...); kll(col, ...)'` | Opt-in approximate sketches for sift.approx_count_distinct() and quantiles ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
| `skip_index` | `''` | `'column: bloom'`, `'column: trigram'`, comma separated | High-cardinality skipping index per column: Bloom for equality and IN, trigram for LIKE and ILIKE ([TABLES.md](TABLES.md#3-per-table-options-10-options)) |
<!-- sift-table-options:end -->

`sift_table` has no `track_changes`, `projection`, or `vacuum_truncate` option: it tracks its own changes natively, and `VACUUM` truncates reclaimed tail blocks by default. 5.1 had 17 table options; the ones not listed above are automatic in 6.0 and still accepted (see [Deprecated & Format 1 Compatibility Options](#deprecated--format-1-compatibility-options)).

## Deprecated & Format 1 Compatibility Options

The options below are not part of the 63 knobs of 6.0. They stay registered so that upgraded Format 1 indexes and tables, `pg_dump` scripts, `pg_upgrade`, and `ALTER INDEX ... RESET (...)` keep working. A Format 1 relation reads them as before. A Format 2.0 relation accepts and ignores them: `CREATE INDEX`, `ALTER INDEX ... SET`, and `sift.table_set_option` say so with a `NOTICE`, and `RESET` removes them without one. Format 2.0 chooses what they set by itself unless a replacement is named.

<!-- sift-deprecated-options:start -->
| Option | Applies to | Replaced by in 6.0 | Former purpose |
|---|---|---|---|
| `array_lengths` | `sift_col` | automatic in Format 2.0 | Keep the element counts of array columns so cardinality() and array_length() are answered from the index |
| `calendar_columns` | `sift_col` | `preagg` | Keep calendar cells of date and timestamp columns so date_trunc, extract and date ranges read cells |
| `cluster_advice` | `sift_col` | automatic in Format 2.0 | Let sift.cluster_advice() recommend a physical order or key_cut, and CREATE INDEX suggest key_cut |
| `count_columns` | `sift_col` | automatic in Format 2.0 | Order the rows of each dictionary string so count(*) of a prefix, range or equality reads no row |
| `cube_minmax` | `sift_col` | `preagg` | Keep min and max per group in the cube so grouped min/max reads no row |
| `dict_lookup` | `sift_col` | automatic in Format 2.0 | Test equality, IN and prefix on a dictionary of 1,024 strings or more in place, without parsing it whole |
| `hll_columns` | `sift_col` | `sketches` | Keep a HyperLogLog sketch per unit for sift.approx_count_distinct; count(DISTINCT) stays exact |
| `key_columns` | `sift_col` | `skip_index` | Keep per-unit key lists, bitmaps or bloom filters for count(DISTINCT) and equality or IN skipping |
| `key_cut` | `sift_col` | `key_order` | Cut units by the range of up to 4 key columns so a lookup or range of the key reads only its units |
| `key_cut_rows` | `sift_col` | none (ignored) | Deprecated and ignored; accepted so that dumps and ALTER INDEX .. RESET keep working |
| `key_table` | `sift_col` | automatic in Format 2.0 | Answer count, sum, min and max for a key = x, IN, range or GROUP BY of the key from per-key lists |
| `order_array_columns` | `sift_col` | `key_order` | Keep a sorted array of every row key so a range or ORDER BY col LIMIT n reads only the entries needed |
| `order_array_group` | `sift_col` | `key_order` | Group the order arrays by a column so WHERE group = x ORDER BY col LIMIT n reads n entries |
| `order_columns` | `sift_col` | `key_order` | Deprecated and ignored; order_array_columns answers what its sorted runs did |
| `order_layout` | `sift_col` | `key_order` | Lay out the order arrays whole (exact key) or compact (heap address and bucketed key) |
| `point_columns` | `sift_col` | automatic in Format 2.0 | Deprecated and ignored; point_tid_columns lists the heap addresses of the rows of a column |
| `point_tid_columns` | `sift_col` | automatic in Format 2.0 | List heap addresses in the point directory so an equality or short IN list reads no unit |
| `return_columns` | `sift_col` | automatic in Format 2.0 | Return text, varchar and uuid columns from the segment dictionary in an index-only scan |
| `rollup` | `sift_col` | `preagg` | Deprecated alias of rollup_leaves, which wins when both are set |
| `rollup_columns` | `sift_col` | `preagg` | Pick the low-cardinality column pairs whose GROUP BY and filter are read from a per-unit rollup |
| `rollup_dims` | `sift_col` | `preagg` | Older spelling of rollup_columns, still accepted |
| `rollup_leaves` | `sift_col` | `preagg` | Name the leaves of array elements and map values that the rollup of every unit groups by |
| `rollup_on_demand` | `sift_col` | `preagg` | A scan that reads units for lack of their rollups asks the worker to rebuild them |
| `superunit` | `sift_col` | `preagg` | Store merged sums and cubes per group of units so a scan no predicate cuts through reads one summary |
| `write_buffer_limit` | `sift_col` | automatic in Format 2.0 | Write-buffer size before it is sealed for promotion; the base of the merge tiers |
| `build_segments` | `sift` | none (diagnostic) | Segments written by CREATE INDEX (0: one per build participant); a diagnostic that turns the parallel build off |
| `mutable_limit` | `sift` | `write_buffer_limit` | Deprecated alias of write_buffer_limit, accepted forever |
| `async_promote` | `sift_table` | automatic in Format 2.0 | Ask a worker to promote a closed buffer so the pass stays out of the statement |
| `auto_defrag_budget` | `sift_table` | `auto_defrag` | Megabytes a background defrag pass may rewrite |
| `auto_defrag_coalesce` | `sift_table` | `auto_defrag` | Let background defrag merge neighbouring small parts into one |
| `auto_defrag_free_pct` | `sift_table` | `auto_defrag` | Free-space percent of a table that makes background defrag start |
| `auto_defrag_indexes` | `sift_table` | `auto_defrag` | Let background defrag rebuild the secondary indexes of the table |
| `backpressure` | `sift_table` | automatic in Format 2.0 | A writer that needs a new buffer first waits for maintenance and merges once the directory passes this percent |
| `bulk_groups` | `sift_table` | automatic in Format 2.0 | COPY, CREATE TABLE AS and REFRESH MATERIALIZED VIEW write straight into column groups |
| `dict_lookup` | `sift_table` | automatic in Format 2.0 | Test strings against a dictionary of 1,024 strings or more in place, without loading it whole |
| `hot_update` | `sift_table` | automatic in Format 2.0 | An UPDATE that keeps the indexed columns adds no index entries; old entries lead to the new version |
| `inline_promote` | `sift_table` | automatic in Format 2.0 | Promote closed write buffers into columns in the writer while rows are inserted |
| `key_table` | `sift_table` | automatic in Format 2.0 | Answer count, sum, min and max for a key = x, IN, range or GROUP BY of the key from per-key lists |
| `wide_rows` | `sift_table` | automatic in Format 2.0 | Write a row that does not fit a write buffer (up to 1 GB) instead of refusing it |
| `write_buffer_limit` | `sift_table` | automatic in Format 2.0 | Row-format write buffer size before it is closed and promoted into columns |
<!-- sift-deprecated-options:end -->
