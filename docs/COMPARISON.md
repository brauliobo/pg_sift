# Feature Comparison

Objective feature comparisons for `pg_sift` 6.0 against PostgreSQL built-ins, in-PostgreSQL extensions, and external analytics and search engines. Facts checked against official documentation (2026-10-18) and updated for `pg_sift` 6.0 capabilities.

## 1. In-PostgreSQL Analytics

| Feature | `pg_sift` 6.0 | PG 18 Heap | Citus Columnar | TimescaleDB Hypercore |
|---|---|---|---|---|
| Columnar storage | `sift_col` index & `sift_table` | No (row heap) | `USING columnar` table | Hybrid row + column chunks |
| Vectorized aggregation | Yes (2,048-row SIMD `SiftAgg`) | No (row-at-a-time) | No (row-at-a-time) | Yes (batch aggregates) |
| Works on existing heap table | Yes (`sift_col` index) | Native | No (table rewrite) | No (chunk conversion) |
| Exact `count(DISTINCT)` | Yes (opt-in HLL also available) | Yes | Yes | Yes (HLL in Toolkit) |
| Full `UPDATE` / `DELETE` + MVCC | Yes (COW DVs, O(1) MVCC) | Yes | No (append-only) | Yes (decompresses batch) |
| Pre-aggregates & M:N scopes | Yes (`preagg` + Entity-Once `scope`) | Matviews (full refresh) | PG matviews | Continuous aggregates |
| `shared_preload_libraries` | Not required (except `sift_table` pub) | n/a | Required | Required |
| Physical & logical replication | Yes (generic WAL; logical rows) | Yes | Physical only | Streaming replication |
| License | PostgreSQL License | PostgreSQL License | AGPL-3.0 | Apache-2.0 / Timescale |

*Note:* In `pg_sift` 6.0, `sift_col` and `sift_table` share 100% of Format Major 2 (up to 1,024 columns, 64 `GROUP BY` keys, online 8-column `key_order`, pre-aggregates, and zero-refusal `COPY`).

## 2. External Analytical & Search Engines

| Feature | `pg_sift` 6.0 | ClickHouse | DuckDB | Elasticsearch |
|---|---|---|---|---|
| Runs inside PostgreSQL | Yes (same transaction) | No (separate cluster) | Embedded (`pg_duckdb` ext) | No (separate cluster) |
| Standard PostgreSQL SQL & joins | Yes (native fallback) | Own SQL dialect | PG-inspired dialect | DSL / ES\|QL (limited joins) |
| ACID transactions & MVCC | Full PostgreSQL MVCC | Experimental | Single-writer MVCC | Optimistic per-doc (NRT) |
| Exact `count(DISTINCT)` | Yes | Yes (`uniqExact` default) | Yes | Approximate only (HLL++) |
| Scale-out petabyte sharding | No (single PG node) | Yes (`ReplicatedMergeTree`) | Single-node | Yes (sharded cluster) |
| Vectorized SIMD execution | AVX-512 / AVX2 / NEON | AVX-512 / AVX2 / NEON | Vectorized CPU | Lucene block-max / SIMD |
| Secondary sort and aggregate `PROJECTION`s | Yes (`sift.create_projection`, automatic choice) | Yes (`PROJECTION`) | No | No |
| Cold object-storage tier & part-drop TTL | Yes (`file://`, `s3://`, `r2://`; `sift_table`) | Yes (S3 disks, TTL) | Parquet files | ILM / searchable snapshots |
| `argMax` / `ASOF` / `date_bin` / multi-`FILTER` | Yes (`sift.arg_max`, `LATERAL` ASOF, `date_bin`, 64 `FILTER`s) | Yes (native) | Yes (native) | Limited |
| License | PostgreSQL License | Apache-2.0 | MIT | AGPL-3.0 / SSPL / Elastic |

## 3. Full-Text Search

| Feature | `pg_sift` 6.0 (`sift`) | PG FTS (`tsvector` GIN) | ParadeDB (`pg_search`) | Elasticsearch |
|---|---|---|---|---|
| Exact BM25 ranking | Yes (`k1`, `b`) | No (`ts_rank` only) | Yes | Yes |
| Phrases, prefix, wildcard, regex, fuzzy | All five | Phrase & prefix only | Phrase, regex, fuzzy | All five |
| Filter pushdown in ranked scan | Yes (`INCLUDE` + bitmap) | Post-filter / multi-GIN | Yes (indexed columns) | Yes (`bool` filter) |
| Exact facets | Yes (`sift.facet_counts`) | No | Yes (`pdb.agg`) | Yes (aggregations) |
| Transactional visibility | Immediate (same MVCC) | Immediate (same MVCC) | Immediate | Near real time (refresh) |
| Physical replication (hot standby) | Yes (generic WAL) | Yes | Enterprise only | Shard replicas |
| Analyzers & tokenizers | PG `pg_ts_config` (29 langs) | PG `pg_ts_config` | 20 Snowball, ICU, CJK | 36 langs, 15 tokenizers |
| `shared_preload_libraries` | No | n/a | Yes (+ `pgvector`) | Separate cluster |
| License | PostgreSQL License | PostgreSQL License | AGPL-3.0 / Commercial | AGPL-3.0 / SSPL / Elastic |

## 4. Where `pg_sift` Is Weaker

Be aware of these trade-offs before choosing `pg_sift`:

- **Single-node architecture:** `pg_sift` scales vertically inside PostgreSQL (CPU cores, SIMD, RAM, NVMe). For multi-node petabyte warehousing or distributed cross-shard search, use ClickHouse or Elasticsearch.
- **Primary-key point lookups:** Keep standard PostgreSQL B-tree indexes for single-row `WHERE id = $1` lookups and uniqueness constraints.
- **General multi-way joins & window functions:** `SiftAgg` accelerates single-table scans, star-schema broadcast dimension lookups, and Entity-Once scopes; Grace-partitioned hash joins above `work_mem / 2`, the `ASOF` `LATERAL` idiom, and windows with `FILTER`, `LAG`/`LEAD`, and moving `ROWS` frames run in `SiftAgg`; other multi-way joins and window shapes execute on PostgreSQL's native planner (with `SiftScan`/`SiftAgg` leaves where applicable).
- **Text tokenization & BM25F:** `sift` uses a Unicode word tokenizer paired with PostgreSQL `pg_ts_config` stemmers/dictionaries — there is no built-in CJK word segmenter (jieba/lindera), edge n-gram tokenizer, or per-field length-normalized BM25F (weighted fields sum per-field BM25 like Elasticsearch `most_fields`).
- **One key per BM25 index:** Multi-column search concatenates fields with `sift.sections(...)` rather than declaring multiple indexed text columns directly.
- **`sift_table` updates:** Unlike heap tables, `sift_table` does not perform in-place HOT updates; tables with heavy single-row update churn on unqueried columns are better served by a heap table plus a `sift_col` index.

## 5. Reference Sources

Comparisons above reflect official project documentation verified on 2026-10-18:
- PostgreSQL 18 (`REL_18_STABLE`) text search, GIN, and executor documentation (`postgresql.org/docs/18/`)
- Citus v14.2.0 (`citusdata/citus` columnar README and table management docs)
- TimescaleDB 2.30.2 Hypercore columnstore and continuous aggregates documentation (`tigerdata.com/docs/`)
- ClickHouse 26.8 LTS / 26.9 MergeTree, codecs, and transaction documentation (`clickhouse.com/docs/`)
- DuckDB v1.5.6 and `pg_duckdb` v1.1.1 (`duckdb.org/docs/`)
- ParadeDB `pg_search` 0.26.2 Community (`paradedb.com/docs/`)
- Elasticsearch 9.5.5 query DSL, ES|QL, and similarity docs (`elastic.co/docs/`)
