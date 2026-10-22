# SQL reference

Generated from `sql/pg_sift--3.3.sql`; do not edit by hand. Descriptions are the `COMMENT ON` of the install scripts (`\df+`).

Schema `sift`: 46 functions, 3 views, 1 table, 2 operator classes, 1 operator. The first column is the signature with the defaults; Since is the first release that has the name; the section is the one of [USAGE.md](USAGE.md) that uses it most.

Settings (`sift.*`) are in the table of [Maintenance and operations](USAGE.md#maintenance-and-operations); the fixed limits are in [LIMITS.md](LIMITS.md).

## Search and ranking

The `sift` index (BM25): the operator, ranking, query helpers, facets and the inspection of the index.

| Object | Kind | Returns | Since | Description |
|---|---|---|---|---|
| `text @@> text` | operator | `bool` | 1.0 | Full-text match of a text against a SiftQL query (words, phrases, prefixes, fuzzy terms, NOT, fields); a sift index answers it, with BM25 ranking through sift.score. See [Ranking, filters and joins](USAGE.md#ranking-filters-and-joins). |
| `sift.facet_counts(tbl regclass, key text, query text, facets jsonb, filter text DEFAULT NULL, hits bool DEFAULT false)` | function | `setof (facet text, kind text, value text, lo text, hi text, count int8)` | 3.1 | Facet counts over a search: terms, range, histogram and date_histogram facets of any column type, several in one call. See [Facets](USAGE.md#facets). |
| `sift.facet_sql(tbl regclass, key text, query text, facets jsonb, filter text DEFAULT NULL, hits bool DEFAULT false)` | function | `text` | 3.1 | The SQL statement that facet_counts runs, one scan per facet. See [Facets](USAGE.md#facets). |
| `sift.facets(index regclass, facet_index regclass, query text, lim int DEFAULT 10, nulls bool DEFAULT false)` | function | `setof (value text, count int8)` | 1.1 | Counts of the matches of a query per value of a keyword sift index, most frequent first. See [Facets](USAGE.md#facets). |
| `sift.field_query(query text, fields text, index regclass DEFAULT NULL)` | function | `text` | 3.1 | The query with the weights of its fields: each term searched in every field with its boost times the field weight. See [Analyzers, phrases, several columns](USAGE.md#analyzers-phrases-several-columns). |
| `sift.highlight(doc text, query text, start_tag text DEFAULT '<b>', stop_tag text DEFAULT '</b>', index regclass DEFAULT NULL)` | function | `text` | 1.0 | The text with the matched terms of a query wrapped in start_tag and stop_tag. |
| `sift.index_info(index regclass)` | function | `(relation_blocks int8, high_water int8, free_blocks int8, reusable_blocks int8, free_extents int8, segments int4, max_segments int4, ndocs int8, sum_dl int8, maintenance_busy bool)` | 1.2 | Space and directory state of a sift index: blocks, free list, segments, documents and whether maintenance runs. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.match(doc text, query text)` | function | `bool` | 1.0 | The function behind @@> with the default analyzer; the planner rebinds it to the index analyzer. |
| `sift.match_idx(doc text, query text, index oid)` | function | `bool` | 1.0 | The function behind @@> for a column whose sift index uses another analyzer. See [Analyzers, phrases, several columns](USAGE.md#analyzers-phrases-several-columns). |
| `sift.max_score(tid)` | function | `real` | 1.0 | The largest BM25 score the query can give, for a scan (error anywhere else). See [Ranking, filters and joins](USAGE.md#ranking-filters-and-joins). |
| `sift.parse_query(query text, index regclass DEFAULT NULL)` | function | `text` | 1.0 | The parsed form of a SiftQL query under an index analyzer, as text. |
| `sift.score(tid)` | function | `real` | 1.0 | BM25 score of the row of a sift scan, in the same query (error anywhere else). See [Ranking, filters and joins](USAGE.md#ranking-filters-and-joins). |
| `sift.sections(VARIADIC parts text[])` | function | `text` | 3.1 | Joins the fields of a document into the text a fields index is built on. See [Analyzers, phrases, several columns](USAGE.md#analyzers-phrases-several-columns). |
| `sift.segment_postings(index regclass)` | function | `setof (gen int8, df_class text, terms int8, postings int8, inline_terms int8, list_terms int8, grouped_terms int8, codec_bytes int8, sidecar_bytes int8, bits_per_posting float8)` | 1.0 | Postings of a sift index by segment and document-frequency class: representation counts, codec bytes and bits per posting. |
| `sift.segments(index regclass)` | function | `setof (gen int8, kind text, ndocs int8, sum_dl int8, avgdl float8, bytes int8, sb_lo int4, sb_hi int4, dirty bool, nterms int8, avgdl_ref float8, docmap_bytes int8, postings_bytes int8, dict_bytes int8, live_bytes int8, extents int4, dead_docs int8)` | 1.0 | One row per segment of a sift index: documents, terms, bytes by section, dirty and dead documents. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.tokenize(doc text, index regclass DEFAULT NULL, stemmer text DEFAULT NULL, case_folding bool DEFAULT NULL, accent_folding bool DEFAULT NULL)` | function | `setof (token text, pos int4, start_offset int4, end_offset int4)` | 1.0 | The tokens of a text under an index analyzer or given options: token, position and character offsets. See [Analyzers, phrases, several columns](USAGE.md#analyzers-phrases-several-columns). |
| `sift.verify(index regclass, heapallindexed bool DEFAULT false)` | function | `void` | 1.0 | Checks the structure of a sift index (and with heapallindexed, that every visible row is in it); raises on a problem. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.text_ops FOR TYPE text USING sift (default)` | operator class |  | 1.0 | Default operator class of the sift index for text: the @@> operator. |

## Column index

The `sift_col` index: what it stores, and the operator classes that let the planner scan it.

| Object | Kind | Returns | Since | Description |
|---|---|---|---|---|
| `sift.col_clustering(index regclass, width_pct float8 DEFAULT 1)` | function | `setof (leaf int4, path text, units int8, zoned_units int8, useful_pct float8, point_units float8, range_units float8, range_ideal float8, correlation float8, verdict text)` | 3.3 | How well the rows of a sift_col index are ordered for its zone maps, one row per leaf: the units an equality or a range touches, the correlation and a verdict (clustered, partial or scattered). See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.col_index_info(index regclass)` | function | `(relation_blocks int8, high_water int8, free_blocks int8, reusable_blocks int8, free_extents int8, segments int4, max_segments int4, rows int8, bytes int8, units int8, buffers int4, leaves int4, maintenance_busy bool, simd text, preagg_bytes int8, preagg_units int8)` | 3.0 | Space and directory state of a sift_col index: blocks, free list, segments, rows and the kernel level of the scans. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.col_leaves(index regclass)` | function | `setof (leaf int4, path text, kind text, type text, const_chunks int8, for_chunks int8, pack_chunks int8, codec_chunks int8, inline_chunks int8, chunk_bytes int8, dict_bytes int8)` | 3.0 | One row per leaf of a sift_col index: its chunks by encoding, their bytes and the bytes of its dictionaries. |
| `sift.col_sections(rel regclass)` | function | `setof (section text, leaf text, bytes int8)` | 3.0 | Where the bytes of a sift_col index or a sift_table go, one row per kind; they add up to pg_relation_size. |
| `sift.col_segments(index regclass)` | function | `setof (gen int8, kind text, rows int8, bytes int8, sb_lo int4, sb_hi int4, units int4, dirty bool, dead_rows int8, extents int4, format int4, preagg_bytes int8)` | 3.0 | One row per segment of a sift_col index: rows, units, bytes, column format and the bytes of its pre-aggregates. |
| `sift.col_unit_agg(index regclass, leaf text)` | function | `setof (segment int8, sb int4, rows int4, exact bool, nvals int8, nnull int8, min text, max text, sum numeric, sumsq numeric, sum_float float8)` | 3.0 | Pre-aggregates of every stored unit of a leaf of a sift_col index (debug and test hook). |
| `sift.col_unit_hist(index regclass, dim text, measure text DEFAULT NULL)` | function | `setof (segment int8, sb int4, exact bool, key text, n int8, nn int8, sum numeric, sum_float float8)` | 3.0 | Histogram of the groups of a dimension of a sift_col index per unit, with the count and sum of one measure (debug and test hook). |
| `sift.col_verify(index regclass, heapallindexed bool DEFAULT false)` | function | `void` | 3.0 | Checks the structure of a sift_col index (and with heapallindexed, its rows against the heap); raises on a problem. |
| `sift.col_*_ops` | operator class |  | 3.3 | Operator classes per type (int2, int4, int8, float4, float8, date, timestamp, timestamptz, bool, numeric, text, bpchar, uuid) that let the planner scan a sift_col index; opt-in per column. |
| `sift.col_ops FOR TYPE anyelement USING sift_col (default)` | operator class |  | 3.0 | Default operator class of the sift_col index: it has no operators, the index is read by the column scans. |

## Tables

The `sift_table` access method.

| Object | Kind | Returns | Since | Description |
|---|---|---|---|---|
| `sift.table_paths` | table |  | 3.2 | The declared JSON paths of the jsonb columns of sift_table tables (one row per table and column), dumped with the extension. |
| `sift.table_segments(tbl regclass)` | function | `setof (gen int8, kind text, rows int8, bytes int8, sb_lo int4, sb_hi int4, units int4, dead_rows int8, format int4, unfrozen bool, min_xid xid, max_xid xid)` | 3.0 | One row per segment of a sift_table: write buffers, sealed buffers, column segments (rows, units, dead rows, format, unfrozen) and tombstones. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.table_set_paths(tbl regclass, col name, spec text)` | function | `void` | 3.2 | Declares the JSON paths of a jsonb column of a sift_table, in the grammar of the paths option of sift_col; an empty spec removes it. |
| `sift.table_space(tbl regclass)` | function | `(relation_blocks int8, high_water int8, free_blocks int8, reusable_blocks int8, free_extents int8, vblocks_left int8)` | 3.0 | Space of a sift_table: file blocks, free list, reusable blocks and the virtual TID blocks left. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |

## Maintenance and workers

Promotion, merging, rewriting and the background workers, of all three.

| Object | Kind | Returns | Since | Description |
|---|---|---|---|---|
| `sift.col_maintain(index regclass)` | function | `bool` | 3.2 | The rewrite of sift.col_auto_merge now, in this session: seals the write buffer and merges every segment when churn has scattered the index; false when nothing is due. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.col_maintenance_due(index regclass, automatic bool DEFAULT true)` | function | `text` | 3.2 | Why col_maintain would rewrite the index now (dirty units, fragmented units, write buffer rows), NULL when it is not due. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.col_merge(index regclass, force bool DEFAULT false)` | function | `int4` | 3.0 | Merges the segments of a sift_col index into fewer; force merges even when no policy asks; returns the segments merged. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.col_promote(index regclass)` | function | `int4` | 3.0 | Turns the write buffer of a sift_col index into column segments; returns the segments written. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.col_refresh(index regclass, max_segments int4 DEFAULT NULL)` | function | `(refreshed int4, remaining int4)` | 3.3 | Rewrites the segments of a sift_col index whose pre-aggregates differ from its current options, at most max_segments per call; returns the segments rewritten and those left. |
| `sift.col_rollup_counters()` | function | `(requested int8, refreshed int8, skipped int8, dropped int8, queued int4, refreshing int4)` | 3.1 | Server counters of the on-demand refresh of unit rollups: requested, refreshed, skipped, dropped, queued, refreshing. |
| `sift.col_rollup_queue()` | function | `setof (index regclass, segment int8, units int4, requested_at timestamptz, state text)` | 3.1 | The queued requests to refresh the rollups of a segment, with their state. |
| `sift.col_rollup_refresh(index regclass)` | function | `int4` | 3.1 | Refreshes the queued rollup requests of a sift_col index now, in this session; returns the segments rewritten. |
| `sift.col_rollup_requests` | view |  | 3.1 | The rows of col_rollup_queue as a view. |
| `sift.col_rollup_state` | view |  | 3.1 | The columns of col_rollup_counters as a view. |
| `sift.merge(index regclass, force bool DEFAULT false)` | function | `int4` | 1.0 | Merges the segments of a sift index into fewer; force merges even when no policy asks; returns the segments merged. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.promote(index regclass)` | function | `int4` | 1.0 | Turns the write buffer of a sift index into an immutable segment; returns the buffers promoted. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |
| `sift.table_maintain(tbl regclass)` | function | `(busy bool, dead_rows int8, promoted int4, merged int4, blocks_before int8, blocks_after int8)` | 3.2 | VACUUM's work on the column segments and the file of a sift_table without its indexes: clears dead rows, promotes buffers, rewrites mostly dead segments, returns the free end of the file. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.worker_state(index regclass)` | function | `(segments int4, sealed int4, running bool, failures int4, failed_at timestamptz, next_retry timestamptz, error_code text, last_error text)` | 3.3 | Maintenance state of an index: segments and sealed write buffers waiting for promotion, whether a step runs now, and the last background failure with its retry time. |
| `sift.workers` | view |  | 3.3 | One row per sift and sift_col index the caller can read: its maintenance state (sift.worker_state) and, for a sift_col index, why it would be rewritten now. See [Maintenance and operations](USAGE.md#maintenance-and-operations). |

## Diagnostics and upgrade

Session statistics, why a query kept the normal plan, what an upgrade leaves to do.

| Object | Kind | Returns | Since | Description |
|---|---|---|---|---|
| `sift.col_rollup_misses()` | function | `setof (rel regclass, reason text, fixable bool, scans int8, units int8)` | 3.3 | The scans of this session that could not answer from the pre-aggregates of their sift_col index, by index and reason, and whether other index options would answer them. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.explain_why(query text)` | function | `setof (arm text, used bool, slug text, reason text, scan_cost float8, normal_cost float8)` | 3.3 | Why each candidate path of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run: one row per arm with the first reason it gave up. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.refusal_slugs()` | function | `setof (slug text, class text, reason text)` | 3.3 | The slugs that sift.explain_why and sift.stats() name, with their class (limit: the query has more of something than a fixed limit of the extension allows; semantic: otherwise) and the reason. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.reindex_needed()` | function | `setof (index regclass, detected text, advice text, command text)` | 3.3 | The sift_col indexes that a REINDEX would improve (an option that only the units written after it follow), with the statement to run; changes nothing. |
| `sift.stats()` | function | `setof (name text, value int8)` | 3.3 | What the aggregate pushdown (Custom Scan (SiftAgg)) scans of this session did: one row per counter and per refusal slug. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.stats_reset()` | function | `void` | 3.3 | Resets the session counters that sift.stats() reports. See [Column indexes, aggregates and sift_table](USAGE.md#column-indexes-aggregates-and-sift_table). |
| `sift.version()` | function | `text` | 1.2 | Version of the loaded library (the installed extension version is in pg_extension). See [Maintenance and operations](USAGE.md#maintenance-and-operations). |

## Internal

Not meant to be called: `sift.col_handler`, `sift.col_ops_unused`, `sift.facet_cap`, `sift.facet_part`, `sift.match_support`, `sift.matchsel`, `sift.publication_check`, `sift.sift_handler`, `sift.table_ddl_check`, `sift.table_handler`, `sift.table_paths_check`, `sift.table_paths_drop`. Names starting with `_` are for the tests.

