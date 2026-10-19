# Compared with other systems

Feature facts for search and for analytics, pg_sift against the PostgreSQL options and the servers most often used for the same job. The short tables are in the
[README](../README.md#compared-with); this file has the full tables, a note per row, the sources and what could not be verified.

- **Checked on 2026-10-18** against each project's own documentation or repository (sources below, with the version read). Re-check a column before quoting it later: these projects release often.
- Some columns are not in the benchmarks at all.
- Where pg_sift is weaker or lacks something the cell says so, and the lists after the tables repeat it.
- **Cells:** `Yes`; `No`; `Partly (condition)`; `n/a` (the question does not apply to that system); `?` (not verified from official documentation: listed under [Unverified](#unverified)).
  A `No` for a feature that the documentation never mentions, where no page says it is unsupported, is listed under [Absence-based](#absence-based-no-and-inferred-yes-cells) too.
- **Columns.** The search columns are the PostgreSQL full-text options plus Elasticsearch; the analytics columns are PostgreSQL itself, other column stores and the systems the benchmarks use.
  A cell describes the version named in [Sources](#sources).

## Search

| Feature | pg_sift | PG FTS (tsvector, GIN) | pg_search (ParadeDB) | pg_textsearch | rum | pgx-bm25 | TIN (PlanetScale) | Elasticsearch |
|---|---|---|---|---|---|---|---|---|
| S1 BM25 ranking | Yes (exact; `k1`, `b` options) | No (`ts_rank`, `ts_rank_cd`) | Yes | Yes | No (`<=>` distance) | Yes | Yes | Yes |
| S2 Phrase queries | Yes (positions in the index; `positions = off` rechecks in the heap) | Yes (`<->`; GIN rechecks in the heap) | Yes | Yes (heap recheck when needed) | Yes (positions in the index) | Yes | Yes | Yes |
| S3 Prefix and wildcard | Yes (`batt*`, `sq?eaky`) | Partly (prefix `:*` only) | Partly (prefix through regex, phrase-prefix or edge n-grams; no wildcard operator found) | Partly (prefix `:*` only) | Partly (prefix only) | Partly (prefix wildcard only) | Yes | Yes |
| S4 Regex | Yes (`/re/`, no back-references) | No | Yes | No | No | No | Yes | Yes |
| S5 Fuzzy (edit distance) | Yes (`~1`, `~2`, first letter fixed) | No | Yes (match and term queries) | No | No | No | Yes (single token) | Yes |
| S6 Field weights | Partly (`fields` option; one key per index) | Yes (`setweight` A-D) | Partly (boost per predicate at query time) | No (one column per index) | Partly (tsvector weights) | Yes (per-field boost) | Partly (`^N` per term; one column per index, scores added) | Yes |
| S7 Facets (counts per value) | Partly (`sift.facets`: keyword-analyzer index, text keys) | No | Yes (`pdb.agg`; needs a ParadeDB operator in the query) | No | No | No | No | Yes |
| S8 Filters on other columns inside the ranked scan | Yes (`sift.filter_pushdown`: bitmap of the other quals) | Partly (extra scalar columns in a multicolumn GIN, ranking is a separate sort) | Yes (columns in the index) | No (post-filter) | Partly (one attached column) | No (filters rechecked) | Partly (btree-indexed conditions narrow the search; others checked per candidate) | Yes (`bool` filter context) |
| S9 Top-N ranked with other filters in one query | Yes | No (bitmap scan, then sort) | Yes (top-K scan: needs `LIMIT`, at most 3 `ORDER BY` columns) | Partly (ordered scan with post-filters; boolean filter plus ranking is not one index scan) | Yes (`ORDER BY <=>` in the index scan) | Partly (needs `WHERE c @@@ q ORDER BY c &@@ q`) | Yes | Yes |
| S10 `count(*)` answered by the index | Yes (`SiftCount`) | No (GIN has no index-only scan) | Partly (when a ParadeDB operator is in the query) | No | No | No | Yes (heap checks until the next VACUUM) | Yes (`_count`) |
| S11 MVCC, own writes visible in the same transaction | Yes | Yes (GIN pending list is searched too) | Yes (docs: index updates are real-time in the transaction) | Yes | Yes (inferred) | Yes (inferred) | Yes (docs: MVCC-correct) | No (near real time: searchable after a refresh, 1 s default) |
| S12 WAL-logged, physical replication (hot standby serves queries) | Yes (generic WAL) | Yes | No (Community: index not physically replicated; Enterprise edition: yes) | Yes (since 1.2.0) | Yes (generic WAL) | Yes (generic WAL; a standby query can fail with 40001, retry) | Yes (needs `hot_standby_feedback`; 40001 possible) | n/a (translog, shard replicas) |
| S13 Logical replication of the indexed table | Yes (rows replicate, the subscriber builds its own index) | Yes | Yes (subscriber builds its own index; PG 17+; DDL not replicated) | ? | ? | ? | ? | n/a |
| S14 Runs inside PostgreSQL | Yes | Yes (core) | Yes (extension) | Yes (extension) | Yes (extension) | Yes (extension) | Yes (extension) | No (separate server) |
| S15 Needs `shared_preload_libraries` | No (only to publish a `sift_table`) | n/a | Yes | Yes | No (README: `CREATE EXTENSION` only) | No (README: `CREATE EXTENSION` only) | No (docs: no restart) | n/a |
| S16 Extra service or process | No (background worker) | No | No (background worker) | No (optional beta background compaction needs the pg_durable extension) | No | No | No (maintenance worker) | Yes (server or cluster) |
| S17 Immutable segments with background merge | Yes (write buffers sealed and merged by a worker) | No (in place; GIN pending list) | Yes (write buffer, immutable segments, LSM merge) | Partly (memtable, immutable segments in levels; compaction runs in the writing session unless background mode, beta, is on) | No (in place) | Partly (pending list sealed into segments; merge by `bm25_merge()`) | Yes (write buffer promoted by a background worker) | Yes |
| S18 Analyzers, stemming | Snowball English only; `keyword` analyzer; no stopword list; fixed per index | Snowball for 29 languages (+ `porter`); stop words for 15; synonym, thesaurus, ispell, unaccent | 20 Snowball stemmers, 15 stopword languages, 13 tokenizers (n-gram, ICU, jieba, lindera, ...) | any PG text search configuration, one per index | PG text search configurations | PG Snowball dictionaries through `ts_lexize`, fixed per index | 18 Snowball languages (off by default); `unicode` and `whitespace` tokenizers; stop words only change scoring | 36 language analyzers, 15 tokenizers |
| S19 Joins with other tables | Yes (nested loop and semi joins, ranked) | Yes | Yes (join pushdown is beta) | Partly (the index is used in a join only with the explicit `to_bm25query()` form) | Yes (inferred) | Partly (in a join use the query-qualified `bm25_score(ctid, query)`) | Yes (a ranked side needs a text predicate) | Partly (ES\ | QL `LOOKUP JOIN`; no SQL joins in the query DSL) |
| S20 License | PostgreSQL License | PostgreSQL License | AGPL-3.0 (Enterprise edition: commercial) | PostgreSQL License | PostgreSQL License | PostgreSQL License | ? | AGPL-3.0, SSPL-1.0 or Elastic-2.0 |
| S21 Source available, self-hostable | Yes | Yes | Yes (Community) | Yes | Yes | Yes | ? (offered on PlanetScale; no download or source found) | Yes |
| S22 PostgreSQL versions | 17, 18 (19 experimental) | 14-18 supported by the project (docs read: 18) | 15-18 | 17, 18 | 12 or later (1.3.15 supports 18) | 17, 18 | 17, 18 | n/a |

### Notes on the search rows

- **S1.** PG FTS and rum rank by frequency or distance and use no inverse document frequency (rum lists "TF/IDF" as to do). pg_textsearch scores are negative (lower ranks first).
- **S3.** pg_search has regex (`pdb.regex('key.*')`), fuzzy prefix, phrase-prefix with `max_expansions` and an edge-n-gram tokenizer, but no wildcard or `LIKE` operator in its full-text reference. Elasticsearch warns that a leading wildcard is slow. pg_sift scores a pattern that expands to more than `sift.max_expansions` (512) terms as one term.
- **S5.** PG FTS has no edit distance; the separate `pg_trgm` extension offers trigram similarity and trigram-indexed `LIKE`/regex, which are not counted here. pg_sift clamps the distance by term length (as Elasticsearch AUTO does).
- **S6.** pg_sift `fields` builds one key from sections separated by `chr(29)` and needs the option at `CREATE INDEX`; without it a colon is plain text ([USAGE.md](USAGE.md#analyzers-phrases-several-columns)). pg_search has `k1`/`b` per field but no per-field weight setting.
- **S7.** pg_sift facets cost linear in the distinct values and win on broad queries over low-cardinality columns; `GROUP BY` wins on selective queries or thousands of values ([USAGE.md](USAGE.md#keyword-facets)). TIN's docs list no facet or `GROUP BY` support.
- **S8, S9.** pg_sift pushes the bitmap of other quals (btree, BRIN, GIN) into the ranked scan when the planner's cost says it pays; a prepared statement builds it only when the bound query matches enough documents. For the sort-by-date top-N of a match, pg_sift needs a `sift_col` index next to the BM25 index (COVERAGE W07).
  pg_textsearch documents that "other conditions are post-filters applied after BM25 scoring" and that a boolean filter with BM25 ranking "is not yet optimized as a single index scan".
- **S10.** pg_search pushes the count down only when a ParadeDB predicate is present (`id @@@ pdb.all()` forces it). TIN falls back to heap checks until the next VACUUM. pg_sift counts from the index; with another filter in the `WHERE` a `SiftScan` feeds PostgreSQL's count (COVERAGE S08).
- **S11.** pg_sift tests its own uncommitted and rolled-back changes (`count_scan`, `tbl_bm25`). Where a cell says "inferred" the documentation is silent and the index is an ordinary access method that hands TIDs to the executor, which applies heap visibility; no own-write test was found. BM25 statistics in pg_sift, pg_textsearch and TIN include rows that are dead but not yet vacuumed.
  Elasticsearch: a get-by-id is real time by default; a search sees a change after the next refresh.
- **S12.** pg_search Community: "The ParadeDB index does not get physically replicated and won't be available on other nodes"; the Enterprise edition supports physical replication. pg_sift needs the same version installed on every node that queries, and `hot_standby_feedback = on` (or `sift.standby_conflicts`) to avoid retryable errors on a standby.
- **S13.** pg_sift logical replication carries rows, never indexes; publishing a `sift_table` needs `pg_sift` in `shared_preload_libraries` on the publisher and its standbys ([USAGE.md](USAGE.md#replication)). pg_search needs the tables to exist on the subscriber and builds indexes after the initial copy.
- **S15, S16.** pg_search needs `pgvector` since 0.25.0.
- **S17.** pg_textsearch updates fragment pages and may need a `REINDEX` after update-heavy loads (ARCHITECTURE.md). pg_sift writes to small write buffers through generic WAL; a background worker (`sift.async_maintenance`, on by default) seals and merges them and VACUUM clears dead postings.
- **S18.** Weakest row for pg_sift: one stemmer, no stop words, no n-gram or CJK tokenizer, analyzer fixed per index (change it with `CREATE INDEX CONCURRENTLY` and a swap). PG FTS count comes from the `snowball` object list of the PG 18 source tree, not from a docs page. Elasticsearch counts 35 language analyzers plus `cjk`.
- **S19.** pg_sift needs a sift index on every partition a ranked query reads, and BM25 statistics are per partition. TIN refuses a ranked join with a side that has no text predicate.
- **S20.** pg_search: AGPL-3.0 text in `LICENSE`, source headers say "or any later version"; GitHub reports `AGPL-3.0`. Elasticsearch: the repository default is the triple; `x-pack` is Elastic-2.0 only.
- No source says TIN is managed-only, and none offers a download.
- **Platform.** pg_sift is tested on x86-64 (other architectures untested; SIMD levels `scalar`, `avx2`, `avx512`). TIN's blog describes vectorized kernels for "any x86 CPU with AVX2 or higher" and says ARM64 also runs; behavior without AVX2 is not stated.

## Analytics

| Feature | pg_sift `sift_col` | pg_sift `sift_table` | PostgreSQL (heap) | Citus columnar | ClickHouse | Elasticsearch | DuckDB | TimescaleDB columnstore |
|---|---|---|---|---|---|---|---|---|
| A1 Columnar storage | Partly (column-format copy inside an index; the table stays a heap) | Yes | No (row storage) | Yes (`USING columnar`) | Yes | Yes (`doc_values`) | Yes | Partly (hybrid: rowstore for new data, columnstore for older chunks) |
| A2 Aggregates on column data: count, sum, avg, min, max | Yes (`Custom Scan (SiftAgg)`) | Partly (shapes the aggregate scan does not read keep a plain `Seq Scan`) | No (row executor, no batch path) | No (tuple at a time; projection pushdown and chunk skipping by min/max) | Yes (vectorized) | Yes (aggregations over `doc_values`) | Yes (vectorized) | Yes (vectorized aggregation on batches; which aggregates: ?) |
| A3 Exact `count(DISTINCT)` | Yes, exact | Yes, exact | Yes, exact | Yes, exact | Yes (exact by default, `uniq*` are approximate) | No (approximate only: HyperLogLog++, no exact mode) | Yes (exact; `approx_count_distinct` available) | Yes (exact; HyperLogLog in the Toolkit extension) |
| A4 `GROUP BY` expressions | Yes (an expression of up to 3 columns; up to 5 keys) | Partly (column keys; an expression keeps the normal plan) | Yes | Yes | Yes | Yes (`terms`, `date_histogram`, runtime fields; ES\ | QL `STATS ... BY` any expression) | ? | Yes |
| A5 Joins | Yes (small tables are joined inside the scan; other joins keep the normal plan) | Yes (normal plan; not pushed into the scan) | Yes | Yes | Yes (hash join by default keeps the right table in memory) | Partly (ES\ | QL `LOOKUP JOIN`; no general joins) | Yes | Yes |
| A6 Pre-aggregates | Yes (per-unit sums and one-key histograms) | No (not implemented) | Partly (materialized views, full refresh only) | Partly (PostgreSQL materialized views) | Yes (incremental and refreshable materialized views, projections, `AggregatingMergeTree`) | Partly (transforms, downsampling for time series; rollups deprecated) | ? | Yes (continuous aggregates) |
| A7 MVCC, transactions | Yes | Yes | Yes | Partly (no serializable isolation) | Partly (experimental transactions) | No (per-document; optimistic concurrency) | Yes (one writing process) | Yes |
| A8 `UPDATE` and `DELETE` | Yes (heap statements; the index follows) | Partly (no HOT; shared row locks become exclusive) | Yes | No (append-only) | Partly (lightweight `DELETE`; lightweight `UPDATE` was Beta until 26.10; `ALTER ... UPDATE` mutations are heavy) | Yes (`update`, `update_by_query`, `delete_by_query`) | Yes | Yes (decompresses the affected batches) |
| A9 Works on an existing heap table through an index | Yes | No (a table access method; `ALTER TABLE ... SET ACCESS METHOD` rewrites) | n/a | No (conversion rewrites the table and drops indexes) | No | No | No (the `pg_duckdb` extension reads heap tables in place) | No (a chunk is converted) |
| A10 PostgreSQL SQL | Yes | Yes | Yes | Yes | No (own SQL-based dialect) | No (query DSL, ES\ | QL, SQL-like) | Partly (follows PostgreSQL conventions, own engine) | Yes |
| A11 Runs inside PostgreSQL, no extra server | Yes | Yes | Yes | Yes | No | No | No (embedded in the host process) | Yes |
| A12 Replication | Yes (generic WAL for physical; rows replicate logically; publishing a `sift_table` needs the preload) | Yes (same) | Yes (physical and logical) | Yes (physical; no logical decoding) | Yes (`ReplicatedMergeTree` with Keeper) | Yes (shard replicas) | ? | Yes (streaming; columnstore chunks on a standby not stated) |
| A13 Compression | Yes (frame-of-reference, delta, RLE, dictionary, scaled integers, ALP; LZ4 or zstd when linked) | Yes (same codec) | Partly (TOAST, per value above about 2 kB: `pglz`, optional `lz4`) | Yes (zstd default if compiled in; lz4, lz4hc, pglz) | Yes (LZ4 default, ZSTD, per-column codecs) | Yes (`index.codec`: LZ4 default, `best_compression`) | Yes (RLE, bit packing, dictionary, FSST, ALP, ...) | Yes (delta, delta-of-delta, Simple-8b RLE, XOR, dictionary) |
| A14 License | PostgreSQL License | PostgreSQL License | PostgreSQL License | AGPL-3.0 | Apache-2.0 | AGPL-3.0, SSPL-1.0 or Elastic-2.0 | MIT | Apache-2.0 core; the columnstore is under the Tiger Data (Timescale) License |

### Notes on the analytics rows

- **A1.** `sift_col` is an index over an ordinary table: the heap stays and the query chooses between the heap plan and the column scan by cost. `sift_table` stores the table itself in column segments. Elasticsearch `doc_values` are columnar structures used for sorting and aggregations; documents are not stored column by column as a whole.
- **A2.** `sift_table` benchmark: 5 of the 10 questions ran as a plain `Seq Scan` and took 1.2x to 7.3x the time of PostgreSQL's heap ([README](../README.md#analytics-1), [USAGE.md](USAGE.md#limits) lists what `SiftAgg` does not read). Citus columnar: the columnar custom scan "pushes down the projections into the table access methods"; no vectorized execution appears in its README, docs or `src/backend/columnar` (a `No` that rests on that absence).
  TimescaleDB: "Vectorized execution evaluates aggregate functions directly on columnstore batches"; the documentation does not list which functions.
- **A3.** pg_sift float sums add in a fixed lane order, so they are identical at any worker count but can differ from the heap-order sum of the normal plan in the last digits (relative 1e-9); numeric sums up to 18 digits are exact in 128 bits. ClickHouse `count(DISTINCT)` runs `uniqExact` by default (setting `count_distinct_implementation`). The benchmark records the Elasticsearch approximation error.
- **A4.** pg_sift evaluates an expression of the group key once per distinct value or tuple; volatile expressions, a column outside the index and more than 5 keys keep the normal plan (COVERAGE K02-K12).
- **A5.** pg_sift lookup joins read the joined table whole at executor start, which suits dimension tables (COVERAGE J01-J09: up to 4 tables; right and full joins, non-equality joins and a large joined table keep the normal plan). Elasticsearch: the `join` field type exists for parent/child in one index, and its documentation says it should not be used like a relational join. TimescaleDB continuous aggregates restrict joins (inner, left, lateral only).
- **A6.** pg_sift answers a unit from stored sums and histograms only when its rows are all visible and clean; churn scans such units again until a merge or `REINDEX` rewrites them. One group key, and `count`, `sum`, `avg` of integers and numerics (COVERAGE AP01-AP06). ClickHouse incremental views fire only on inserts into the left-most source table.
- **A7.** pg_sift under `SERIALIZABLE` can fail with `40001` and needs a retry. DuckDB: "optimistic concurrency control"; one process can write. ClickHouse transactions are experimental and need Keeper, a non-replicated MergeTree and are not supported in Cloud. Elasticsearch: optimistic concurrency per document through `if_seq_no` and `if_primary_term`.
- TimescaleDB: "supports INSERT, UPDATE, DELETE, and UPSERT directly on the columnstore"; statements without a filter can decompress a lot of data.
- **A9.** The point of `sift_col`: add an index, change no table, keep the heap plan available (`SET sift.enable_agg = off`). `sift_table` and the other column stores need the data in their own format.
- **A11, A12.** pg_sift needs no `shared_preload_libraries` entry for `sift_col`; only publishing a `sift_table` under `wal_level = logical` does. TimescaleDB needs `shared_preload_libraries = 'timescaledb'`. Citus installs with `shared_preload_libraries = 'citus'` in its README; since 13.2 columnar is a separate `citus_columnar` extension and whether it alone needs the preload is unverified.
- **A14.** TimescaleDB: all columnstore functions are Community edition (source-available, bars selling it as a service). Citus: single open-source edition since 11.0.

## Where pg_sift is weaker

- **Search:** one key per index (concatenate columns, no multicolumn, unique or `INCLUDE`); English stemming only, no stopword list, no n-gram, ICU or CJK tokenizer; analyzer fixed per index; facets only on `keyword`-analyzed text keys; field boosts need the `fields` option; `sift.score()` cannot be used in `WHERE`; a role under row-level security gets no ranked scan; x86-64 only tested; prefix ranking agrees less with a BM25-over-n-grams oracle than pg_search's.
- **Analytics:** `sift_col` accelerates aggregates and top-N only (not row streams, `NOT IN`, right/full joins, partitioned-table parents, `numeric` wider than 18 digits, `uuid`, `interval`, arrays); `sift_table` is young (127 columns at most, no HOT, some shapes keep a `Seq Scan`, loads over one transaction's write-segment limit must be split). ClickHouse and Elasticsearch are faster on several of the benchmark's questions: the full list is in the [README](../README.md#analytics-1) ("Where pg_sift is not the fastest").

## Sources

Every cell above was checked against the pages listed for its column on 2026-10-18; versions are the latest release on that date unless noted. Most pages were read through a summarizing fetcher, so quoted fragments are its excerpts; the pages that decide a contested cell (pg_search replication, Citus columnar limits, pgx-bm25, TIN limits and SQL shapes, ClickHouse `UPDATE`, DuckDB `CREATE VIEW` and `GROUP BY`) were fetched a second time.

**PostgreSQL built-in FTS**, docs of PostgreSQL 18 (18.6, 2026-08-13; 19 is at release candidate):
[ranking](https://www.postgresql.org/docs/18/textsearch-controls.html), [indexes](https://www.postgresql.org/docs/18/textsearch-indexes.html), [GIN](https://www.postgresql.org/docs/18/gin.html),
[tsvector and tsquery types](https://www.postgresql.org/docs/18/datatype-textsearch.html), [dictionaries](https://www.postgresql.org/docs/18/textsearch-dictionaries.html), [pg_trgm](https://www.postgresql.org/docs/18/pgtrgm.html),
[btree_gin](https://www.postgresql.org/docs/18/btree-gin.html), [hot standby](https://www.postgresql.org/docs/18/hot-standby.html), [logical replication](https://www.postgresql.org/docs/18/logical-replication-architecture.html),
[versioning](https://www.postgresql.org/support/versioning/), [licence](https://www.postgresql.org/about/licence/); source tree `REL_18_STABLE`: `src/backend/utils/adt/tsginidx.c`, `src/backend/access/gin/ginutil.c`, `src/backend/snowball/Makefile`.
For `GROUP BY` expressions and the row executor see [SELECT](https://www.postgresql.org/docs/18/sql-select.html) and [query configuration](https://www.postgresql.org/docs/18/runtime-config-query.html).

**pg_search** 0.26.2 (2026-10-16), Community edition: [docs](https://www.paradedb.com/docs) (scoring `reference/full-text/score`, `phrase`, `regex`, `fuzzy`, `relevance-tuning`, `top-k`, `reference/aggregates/facets`, `reference/aggregates/limitations`, `reference/filtering/indexed`, `reference/joins/overview`, `reference/token-filters/stemming`, [concepts/architecture](https://www.paradedb.com/docs/concepts/architecture), [concepts/guarantees](https://www.paradedb.com/docs/concepts/guarantees), [concepts/limitations](https://www.paradedb.com/docs/concepts/limitations), `operate/deploy/logical-replication/bootstrapping`, [llms.txt](https://www.paradedb.com/docs/llms.txt)),
[repository and LICENSE](https://github.com/paradedb/paradedb), [release v0.26.2](https://github.com/paradedb/paradedb/releases/tag/v0.26.2), [extension README](https://github.com/paradedb/paradedb/blob/main/pg_search/README.md).

**pg_textsearch** 1.5.1 (2026-10-02): [README](https://github.com/timescale/pg_textsearch#readme), [ARCHITECTURE.md](https://github.com/timescale/pg_textsearch/blob/main/ARCHITECTURE.md), [LICENSE](https://github.com/timescale/pg_textsearch/blob/main/LICENSE), [releases](https://github.com/timescale/pg_textsearch/releases), `sql/pg_textsearch--1.6.0-dev.sql`, `src/access/handler.c`, `test/sql/unsupported.sql`.

**rum** 1.3.15 (2025-10-23): [README](https://github.com/postgrespro/rum), [releases](https://github.com/postgrespro/rum/releases), [LICENSE](https://raw.githubusercontent.com/postgrespro/rum/master/LICENSE), `src/rumutil.c`.

**pgx-bm25** v1.0.0 (2026-10-06; the extension is `bm25_native`): [repository, README, ARCHITECTURE.md, ROADMAP, LICENSE](https://github.com/pgexperts/pgx-bm25), [releases](https://github.com/pgexperts/pgx-bm25/releases).

**TIN** (PlanetScale; v1.0.2 benchmarked at launch, v1.0.6 announced 2026-10-08, 1.0.4 on the extensions page; the docs carry no version or date), docs read 2026-10-18:
[Introducing TIN](https://planetscale.com/blog/introducing-tin) (2026-09-16), [TIN v1.0.6](https://planetscale.com/blog/tin-v106), [Anatomy of a Postgres search engine](https://planetscale.com/blog/anatomy-of-a-postgres-search-engine),
[extensions](https://planetscale.com/docs/postgres/extensions), docs under `https://planetscale.com/docs/postgres/search/`: `get-started`, `tinql`, `scoring`, `operations`, `reference/indexes`, `reference/sql-shapes`, `reference/limitations`, `reference/functions`, `reference/settings`;
[Lead](https://github.com/planetscale/lead) (the stand-in). Vendor benchmark numbers are not used.

**Elasticsearch** 9.5.5 (2026-10-06): [similarity](https://www.elastic.co/docs/reference/elasticsearch/index-settings/similarity), query DSL pages under `https://www.elastic.co/docs/reference/query-languages/query-dsl/` (`match-query-phrase`, `wildcard-query`, `regexp-query`, `fuzzy-query`, `multi-match-query`, `bool-query`),
[near real time](https://www.elastic.co/docs/manage-data/data-store/near-real-time-search), [reading and writing documents](https://www.elastic.co/docs/deploy-manage/distributed-architecture/reading-and-writing-documents), [translog](https://www.elastic.co/docs/reference/elasticsearch/index-settings/translog),
[merge](https://www.elastic.co/docs/reference/elasticsearch/index-settings/merge), [language analyzers](https://www.elastic.co/docs/reference/text-analysis/analysis-lang-analyzer), [`LOOKUP JOIN`](https://www.elastic.co/docs/reference/query-languages/esql/commands/lookup-join), [parent-join](https://www.elastic.co/docs/reference/elasticsearch/mapping-reference/parent-join),
[doc values](https://www.elastic.co/docs/reference/elasticsearch/mapping-reference/doc-values), [cardinality](https://www.elastic.co/docs/reference/aggregations/search-aggregations-metrics-cardinality-aggregation), [ES|QL `STATS ... BY`](https://www.elastic.co/docs/reference/query-languages/esql/commands/stats-by),
[rollup](https://www.elastic.co/docs/manage-data/lifecycle/rollup), [downsampling](https://www.elastic.co/docs/manage-data/data-store/data-streams/downsampling-time-series-data-stream), [transforms](https://www.elastic.co/docs/explore-analyze/transforms),
[optimistic concurrency](https://www.elastic.co/docs/reference/elasticsearch/rest-apis/optimistic-concurrency-control), [update API](https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-update), [`index.codec`](https://www.elastic.co/docs/reference/elasticsearch/index-settings/index-modules), [LICENSE.txt](https://github.com/elastic/elasticsearch/blob/main/LICENSE.txt).

**Citus columnar**: Citus v14.2.0 (2026-08-06); the columnar [README](https://github.com/citusdata/citus/blob/main/src/backend/columnar/README.md) and sources on `main` (extension version `15.0-1`, in development), [table management docs](https://docs.citusdata.com/en/stable/admin_guide/table_management.html) (the published docs are 13.0.1), [LICENSE](https://raw.githubusercontent.com/citusdata/citus/main/LICENSE), [Citus 11.0](https://www.citusdata.com/updates/v11-0/).

**ClickHouse**: docs read 2026-10-18; the benchmarks ran 26.9.19.9, and GitHub lists 26.7, 26.8 (LTS) and 26.9 releases at once:
[intro](https://clickhouse.com/docs/intro), [why it is fast](https://clickhouse.com/docs/concepts/why-clickhouse-is-so-fast), [`uniq`](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniq), [`GROUP BY`](https://clickhouse.com/docs/sql-reference/statements/select/group-by), [`JOIN`](https://clickhouse.com/docs/sql-reference/statements/select/join),
[incremental materialized views](https://clickhouse.com/docs/materialized-view/incremental-materialized-view), [transactions](https://clickhouse.com/docs/guides/developer/transactional), [`UPDATE`](https://clickhouse.com/docs/sql-reference/statements/update), [](https://github.com/ClickHouse/ClickHouse/pull/125719),
[replication](https://clickhouse.com/docs/engines/table-engines/mergetree-family/replication), [`CREATE TABLE` codecs](https://clickhouse.com/docs/sql-reference/statements/create/table), [syntax](https://clickhouse.com/docs/sql-reference/syntax), [LICENSE](https://raw.githubusercontent.com/ClickHouse/ClickHouse/master/LICENSE), `src/Core/Settings.cpp` (`count_distinct_implementation`).

**DuckDB** v1.5.6 (2026-09-28): [why DuckDB](https://duckdb.org/why_duckdb), [internals](https://duckdb.org/docs/current/internals/overview.html), [aggregates](https://duckdb.org/docs/current/sql/functions/aggregates.html), [`FROM` and joins](https://duckdb.org/docs/current/sql/query_syntax/from.html),
[concurrency](https://duckdb.org/docs/current/connect/concurrency.html), [`DELETE`](https://duckdb.org/docs/current/sql/statements/delete.html), [PostgreSQL compatibility](https://duckdb.org/docs/current/sql/dialect/postgresql_compatibility.html), [storage](https://duckdb.org/docs/current/internals/storage.html), [`CREATE VIEW`](https://duckdb.org/docs/current/sql/statements/create_view.html),
[`GROUP BY`](https://duckdb.org/docs/current/sql/query_syntax/groupby.html), [pg_duckdb](https://github.com/duckdb/pg_duckdb) (v1.1.1, 2025-12-18), [LICENSE](https://raw.githubusercontent.com/duckdb/duckdb/main/LICENSE).

**TimescaleDB columnstore (Hypercore)** 2.30.2 (2026-09-29), documentation now at Tiger Data:
[Hypercore](https://www.tigerdata.com/docs/learn/columnar-storage/understand-hypercore), [reference](https://www.tigerdata.com/docs/reference/timescaledb/hypercore), [compression methods](https://www.tigerdata.com/docs/use-timescale/latest/hypercore/compression-methods),
[continuous aggregates](https://www.tigerdata.com/docs/use-timescale/latest/continuous-aggregates), [editions and licenses](https://www.tigerdata.com/docs/about/latest/timescaledb-editions), [installation](https://www.tigerdata.com/docs/self-hosted/latest/install/installation-linux), [high availability](https://www.tigerdata.com/docs/deploy/self-hosted/replication-and-ha/about-ha).

## Unverified

Cells written `?` above, and why:

- **pg_textsearch, rum, pgx-bm25, TIN, S13 (logical replication of the indexed table):** no page of their documentation says anything about it.
- **TIN, S20 and S21 (license, source, self-hosting):** no license is stated for TIN; no source or download was found; the documentation says it is available on PlanetScale. Behavior on a CPU without AVX2 is not stated either.
- **DuckDB, A4 (`GROUP BY` expressions):** the `GROUP BY` page speaks of grouping columns and shows no expression; **A6** (pre-aggregates): the `CREATE VIEW` page says the view "is not physically materialized" and does not mention materialized views; a GitHub discussion says they are not supported, which is not documentation; **A12** (replication): none documented.
- **TimescaleDB, A2:** which aggregates are vectorized is not in the documentation read. **A12:** whether columnstore chunks replicate to a standby is not stated (streaming replication is documented for the database).
- **Citus columnar:** whether the columnar code alone is under the repository's AGPL-3.0 (the source files carry no license text), whether `citus_columnar` alone needs `shared_preload_libraries`, and any native pre-aggregate feature. The published Citus docs (13.0.1) predate the 13.2 split.
- **Elasticsearch:** no official page states that there are no multi-document transactions; the `No` in S11 and A7 rests on the documented refresh model and per-document optimistic concurrency. No official API page was found that says an update is a delete plus an add.
- **pg_search:** the license before 2023-09-13 and PostgreSQL 14 support were not checked.
- **ClickHouse:** which release is "current stable" is ambiguous (26.7, 26.8 LTS and 26.9 are all listed); the codecs DoubleDelta, Gorilla, T64 and FPC were not read.

### Absence-based `No` and inferred `Yes` cells

A `No` here means the documentation never lists the feature (the README or SQL object list is the complete surface), not that a page says it is unsupported:

- pg_textsearch: S4, S5, S7, S10 (the access method has `amgetbitmap = NULL` and `amcanreturn = NULL` in `src/access/handler.c`). rum: S4, S5, S7, S10. pgx-bm25: S4, S5, S7, S8, S10. TIN: S7. PG FTS: S7. PostgreSQL (A2) and Citus columnar (A2): no vectorized or batch execution appears in the documentation or, for Citus, `src/backend/columnar`.
- pg_search S3 `Partly` and S6 `Partly` are built from documented parts (regex, fuzzy prefix, phrase-prefix, edge n-grams, boosts) because no wildcard operator or index-level field weight is documented.

A `Yes` marked "(inferred)", or Yes cells for PostgreSQL features of an extension (Citus columnar and TimescaleDB `GROUP BY` and joins, PG FTS S13, rum S19), follow from the system being an index access method, table access method or extension of PostgreSQL executed by its planner and executor; the pages read do not state it for that system.

## Keeping this file current

The tables are written by hand, from the pages above; no tool generates them. When a column changes, re-read its pages, update the cells, the notes, the version and the date in Sources and the "Checked on" line at the top, and move any cell between `?` and the tables. The README tables are a subset of these rows with the same values; keep them equal.
