# pg_sift usage

Reference for everything beyond the [README](../README.md) quick start. The examples use the `docs` table created there.

## Install

Requires PostgreSQL 17 or 18 (19 is experimental), server headers with `pg_config` on `PATH`, and a C compiler. Tested on x86-64 (the whole suite); on macOS arm64 the Homebrew formula was built and tried by hand (CI runs on Linux only). The chunk codec of the column engine links `liblz4` and `libzstd` when their
development headers are installed (`liblz4-dev`, `libzstd-dev`); `make SIFT_NO_LZ4=1` or `SIFT_NO_ZSTD=1` build without one, and the packages depend on both.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
```

- Arch Linux: AUR `pg_sift` (recipe in [packaging/](../packaging/)).
- Debian 13, Ubuntu 24.04 with PGDG `postgresql-18`: download the `.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest)
  and `sudo apt install ./postgresql-18-pg-sift_3.2.2-1_amd64.deb`.
- RHEL, Rocky, Alma 9 or 10 with PGDG `postgresql18-server`:
  `sudo dnf install https://github.com/brauliobo/pg_sift/releases/download/v3.2.2/pg_sift_18-3.2.2-1.el9.x86_64.rpm`.

Then, as a superuser (after installing a newer version run `ALTER EXTENSION pg_sift UPDATE`): `CREATE EXTENSION pg_sift;`. No `shared_preload_libraries` entry is needed for the `sift` index, the `sift_col` index or the `sift_table` table access method; only publishing a `sift_table` under `wal_level = logical` does ([Replication](#replication)).

### Install with Homebrew

```sh
brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git
brew install --HEAD brauliobo/pg_sift/pg_sift
brew test brauliobo/pg_sift/pg_sift
```

The formula in this repository builds for Homebrew PostgreSQL 17 and 18, with LZ4 and zstd, and links extension files through Homebrew's versioned directories.
The HEAD-only formula builds `main` and supports macOS arm64.
The test creates temporary clusters, checks BM25 queries, an extension upgrade and a `sift_col` aggregate, then stops the clusters.
For aggregate pushdown in a new session, load the planner hooks with `LOAD 'pg_sift';` before querying.
Remove the extension package with `brew uninstall pg_sift && brew untap brauliobo/pg_sift`; PostgreSQL installations and databases are managed separately.

## Queries

`AND`, `OR`, `NOT` are keywords in upper case only; juxtaposition is `AND`; precedence is `NOT` > `AND` > `OR`; a `NOT` needs a positive
term in its AND group. A wildcard, regex, fuzzy or prefix leaf expands to dictionary terms. It matches every word it matches, however many (`h*`,
`*ing`, `/.*tion/` or `~2` on a million names), and above `sift.max_expansions` (512) terms it scores as the one term whose documents
are those with such a word (for a prefix, the term `prefix_index` would hold for it), instead of as the best of its terms. A regex
cannot have back-references.

```sql
SELECT id FROM docs WHERE body @@> 'battery NOT squeaky';             -- exclusion: 2
SELECT id FROM docs WHERE body @@> '{case clip squeaky}';             -- any of a set of words or patterns
SELECT id FROM docs WHERE body @@> '"friendly support"';              -- phrase: 2, 5
SELECT id FROM docs WHERE body @@> 'batt* OR sq?eaky OR /sq.*y/ OR frendly~1';  -- prefix, wildcard, regex, fuzzy
SELECT id FROM docs WHERE body @@> '(case OR clip) AND friendly^3';   -- grouping and boost
SELECT id FROM docs WHERE body @@> ANY (ARRAY['case', 'clip']);       -- OR of several queries, same as 'case OR clip' (scored, counted)
```

## Ranking, filters and joins

`ORDER BY sift.score(ctid) DESC LIMIT k` runs a ranked scan that skips blocks that cannot reach the top k; ties break by ctid.
`sift.score()` is computed by the scan: use it in the select list or `ORDER BY` of the query level that scans the table. In `WHERE` it
may only be compared with a constant, a parameter or a stable expression of them (`>`, `>=`, `<`, `<=`, either side): a minimum or maximum
score, tested by the scan on the score it computed, so the rows are those of a subquery that filters on the score (several cutoffs must all
hold; `EXPLAIN` shows `Score Filter`). A cutoff from below keeps the `LIMIT` hint of the ranked scan; one from above reads every match.
`BETWEEN` and `NOT (score > c)` are such comparisons once PostgreSQL has rewritten them. Any other use in `WHERE` (`OR`, `=`, `<>`, an
expression of the score, `sift.max_score()`) is an error: filter on the score from a subquery. A prepared `LIMIT $1 [OFFSET $2]` gets the same scan as a constant one, also in a generic plan.
`ORDER BY name, sift.score(ctid) DESC LIMIT k` (columns of the scanned table first, then the score) scores only the rows tied with the
k-th best `name` instead of every match, and gives the answer of ranking every match and sorting after; `EXPLAIN` shows
`Order: name, score DESC` and `Key Passes`. A query with no `LIMIT` ranks every match in one search.
It works on materialized views too. It is not available in `UPDATE`, `DELETE`, `MERGE` or `FOR UPDATE` (error when planned): rank in a
`SELECT` and change or lock the rows by key.

```sql
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'friendly OR case' AND sift.score(ctid) > 0.5
ORDER BY sift.score(ctid) DESC LIMIT 100;                                          -- a minimum score
SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM docs WHERE body @@> 'friendly OR case'
                   ORDER BY s DESC LIMIT 100) top WHERE s > 0.5;  -- filter on the score of the top 100
SELECT d.id, c.name FROM docs d JOIN (VALUES (1, 'audio')) c(id, name) ON c.id = d.category
WHERE d.body @@> 'friendly' ORDER BY sift.score(d.ctid) DESC LIMIT 10;              -- joins work too
```

### Filter pushdown

With `sift.filter_pushdown` (`on` by default) the planner builds a bitmap of the other quals from their indexes (btree, BRIN, GIN) and
gives it to the ranked scan, which then scores only the matches that pass, so the work follows the filter and not a very common term;
results equal the post-filter plan. A 0.1% filter on 218,058 articles went from 23.6 ms to 0.22 ms (clustered) and 15.0 ms to 0.86 ms
(scattered). `force` pushes down whenever a bitmap path exists; with `on` the planner costs both plans. The post-filter plan is priced
from the filter column's statistics: the ranker visits the heap in order and verifies every match until it holds `k` rows that pass,
so a filter whose rows lie at the end of a correlated column (recent ids) costs it the whole heap before them. Both plans are also charged for the blocks the ranker scores, and an id list (`= ANY(array)`) is priced by the btree leaf pages it touches, so a long list of scattered ids is pushed down.
A prepared statement (or any generic plan: `plan_cache_mode`, a driver that prepares) is planned without its query, so no plan is right for
every query: the plan keeps the filter's bitmap as a child that the scan builds only when the bound query matches enough documents for it
to pay (`Filter Mask From Matches` in `EXPLAIN VERBOSE`; the planner finds the number on the costs of both plans, and a loose filter, one that keeps
a tenth of the rows or more on the benchmark corpus, never gets one). A rare query is
answered by checking the filter after the ranker, a common one by the mask, as in the plan of the literal query (a prepared 2.3% filter
query on common words went from 2.72M to 1.51M cycles, a rare one from 772k to 658k; a filter of 10% or more is left after the ranker).
A filter on a value that the table stores in one stretch of the heap (`country_code = 'US'` on rows loaded by country) is placed from the
btree at planning time (the entries of a key are in heap order, so a few of them tell where the rows that pass start), and is pushed down
when that stretch is late: the filtered one-letter prefix of the places went from 8.74M to 1.26M user cycles. A stretch at the start of the
heap keeps the scan that checks the filter after the ranker.

```sql
CREATE INDEX docs_published ON docs (published);
SET sift.filter_pushdown = force;
EXPLAIN (analyze, costs off, timing off, summary off, buffers off)
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'friendly' AND published >= '2026-02-01'
ORDER BY sift.score(ctid) DESC LIMIT 10;      -- Bitmap Index Scan child; Filter Superblocks / Pages Exact / Pages Lossy
RESET sift.filter_pushdown;
```

## Analyzers, phrases, several columns

Text is split into words, case folded, accent folded (`résumé` is `resume`) and optionally stemmed (Snowball English only); there is no
stopword list. The analyzer is fixed per index: build the replacement with `CREATE INDEX CONCURRENTLY` and swap. The index
stores word positions (`positions = on`, the default), so phrases are decided from the index; `positions = off` gives a much smaller
index that rechecks phrases in the heap. An index has one
key, a `text` column or an immutable expression, so concatenate columns (`coalesce` nullable ones, `NULL` keys are not indexed).

```sql
SELECT token, pos FROM sift.tokenize('Running Cafés, RÉSUMÉ', stemmer => 'en');             -- run, cafe, resum
CREATE INDEX CONCURRENTLY docs_body_en ON docs USING sift (body) WITH (stemmer = 'en');
DROP INDEX docs_body;
ALTER INDEX docs_body_en RENAME TO docs_body;
SELECT id FROM docs WHERE body @@> 'run';                         -- 4: runners, running, runs
CREATE INDEX docs_title_body ON docs USING sift ((coalesce(title, '') || ' ' || coalesce(body, '')));   -- query with the same expression
```

For autocomplete and for weighted fields, build the key from sections with `sift.sections(a, b, ...)` (they are joined with `chr(29)`; a NULL
section is empty), name the leading ones with `fields` and set `prefix_index = n` (up to 15) so prefixes of
up to n letters are one term lookup. The option is a speed and size trade-off, not a requirement: without it a broad prefix such as `h*` is read
from the postings of all its words, once per backend and then from a cache (`sift.prefix_cache_size`): 10-15 ms for a one-letter prefix of a million
names, under a millisecond with the option, which costs index size (a million names: 17 MB, 34 MB at 3, 73 MB at 15):

```sql
CREATE TABLE places (id int PRIMARY KEY, name text, admin1 text, country text);
CREATE INDEX places_sift ON places USING sift (sift.sections(name, admin1 || ' ' || country))
  WITH (fields = 'name', prefix_index = 3);
SELECT id FROM places WHERE sift.sections(name, admin1 || ' ' || country) @@> '(gr* ho*) AND (name:{gr* ho*}^3 OR (gr* ho*))'
ORDER BY sift.score(ctid) DESC LIMIT 20;                          -- every word a prefix, name matches boosted x3
```

`name:word`, `name:(a b)` and `name:{a b}` need the field in the index: a name it lacks is an error (`42601`, listing the fields) and
a query on one key means the same on every plan. Without `fields` (also with no sift index at all) a colon is text, so `name:word` is
the word `name:word` and `name:(a b)` the words `name:a` and `name:b`. With several sift indexes on one key, `@@>` means the default
analyzer when one of them has it, else that of the lowest OID; an index that answers differently (one with `fields` next to a plain
one) does not serve it, and `sift.match_idx(key, query, index)` names the analyzer. Partitions bind `@@>` to their own indexes.

### Field weights

`sift.field_query(query, fields [, index])` searches each word, prefix, wildcard and fuzzy term of a query in every listed field, with the
weight of the field times the term's own boost. A row scores the sum of its fields' scores (the sum of per-field BM25, each with its own
`idf` and `tf` and the length of the whole key); the rows that match are those of the unweighted query on those fields. A weight is above 0 and
up to 10000, and no weight is 1. With `index` the field names are checked against the `fields` of that index. Phrases, regular expressions and
the terms of a `NOT` are not scoped to a field; there is no BM25F, no analyzer per field and no highlight per field
([FIELDS-FACETS.md](FIELDS-FACETS.md)).

```sql
CREATE INDEX items_sift ON items USING sift (sift.sections(title, body, tags)) WITH (fields = 'title,body,tags');
SELECT id FROM items
WHERE sift.sections(title, body, tags) @@> sift.field_query('wireless headphones', 'title^3, body, tags^0.5', 'items_sift')
ORDER BY sift.score(ctid) DESC LIMIT 10;
```

## Partitioned tables

Works with declarative partitioning (also multi-level) and inheritance parents. `CREATE INDEX ... USING sift` on the parent builds one
sift index per leaf partition, also for partitions attached later. Matching and `count(*)` span partitions, and ranking works through the
parent: each partition runs its own ranked scan and a MergeAppend returns the global top-k.

```sql
CREATE TABLE logs (id int, region text, msg text) PARTITION BY LIST (region);
CREATE TABLE logs_eu PARTITION OF logs FOR VALUES IN ('eu');
CREATE TABLE logs_us PARTITION OF logs FOR VALUES IN ('us');
INSERT INTO logs SELECT i, CASE WHEN i % 2 = 0 THEN 'eu' ELSE 'us' END, 'disk full on node ' || i % 10 FROM generate_series(1, 1000) i;
CREATE INDEX logs_msg ON logs USING sift (msg);
SELECT region, count(*) FROM logs WHERE msg @@> 'disk "node 7"' GROUP BY region;
SELECT id, region, sift.score(ctid) FROM logs WHERE msg @@> 'disk "node 7"' ORDER BY sift.score(ctid) DESC LIMIT 3;
```

## Facets

`sift.facet_counts(table, key, query, facets [, filter [, hits]])` returns the counts of several facets of one search in one call: terms with a
top N and the count of the other values, range, histogram and date buckets, on columns of any type (also arrays), and the facets of a
multi-select (a facet ignores its own selection). Every count is exact, the `count(*)` of the same snapshot. The design, the options and the
limits are in [FIELDS-FACETS.md](FIELDS-FACETS.md).

```sql
SELECT * FROM sift.facet_counts('items', 'title', 'wireless',
  '[{"expr":"brand","size":10,"missing":true},
    {"expr":"price","edges":[100,300]},
    {"expr":"added","unit":"month","fill":true},
    {"expr":"tags","array":true}]', 'active', hits => true);   -- facet, kind, value, lo, hi, count
SELECT * FROM sift.facet_counts('items', 'title', 'wireless',
  '[{"expr":"brand","selected":["acme"]}, "category"]');       -- category counts only brand acme; brand counts all brands
```

`kind` is `value` (terms), `bucket` (range, histogram, date), `other` (`lo` values past `size`, `count` their sum), `missing` (NULL) or `hits`
(the rows left after every selection). `sift.facet_sql(...)` returns the statement that `facet_counts` runs, for `EXPLAIN`: with a `sift_col`
index on the columns each facet is a `Custom Scan (SiftAgg)` with the search as the row selection (a million rows, four facets: 3-13 ms against
140-310 ms for the ordinary plan on broad queries); an array facet or a filter on a column outside the index runs as the ordinary plan, with
the same counts. The statement runs as the calling role, so permissions and row-level security apply.

### Keyword facets

`analyzer = keyword` indexes each whole value as one term, so `sift.facets(index, facet_index, query, lim, nulls)` counts the matches of a
query per value of another column from the bitmaps, equal to the `GROUP BY` of the same snapshot. It pays for broad queries on
low-cardinality columns (1.9-4.5x on 26 values); for selective queries or thousands of values `GROUP BY` wins.
A value of more than 255 bytes is indexed as its first bytes and a hash of all of it, so `@@>` still finds exactly the rows with that value
(`sift.facets` shows it as that start and the hash); it was an error before.

```sql
CREATE INDEX docs_category ON docs USING sift ((category::text)) WITH (analyzer = keyword);   -- text keys: cast integers
SELECT * FROM sift.facets('docs_body', 'docs_category', 'friendly OR case', 10);      -- value, count: 1 | 3
```

## Parallel scan

`sift.parallel_scan` (`off` by default) lets the planner add a Gather over a parallel SiftScan or SiftCount; a ranked one needs a `LIMIT`
and a single table, and rows, scores and ties equal the serial plan. It is off because a Gather costs milliseconds before any work: on
218,058 articles only an unordered scan with a heavy residual qual gained (1.8x). Try it on much larger tables or expensive quals.

```sql
SET sift.parallel_scan = on;
EXPLAIN (costs off) SELECT count(*) FROM docs WHERE body @@> 'friendly';      -- Gather over a parallel SiftCount when it pays
RESET sift.parallel_scan;
```

## Maintenance and operations

Writes go to small write buffers through generic WAL; a background worker (`sift.async_maintenance`) turns them into immutable segments
and merges; VACUUM promotes, merges and clears dead postings.
`sift.segments(index)` and `sift.verify(index, true)` inspect and check an index. `sift.index_info(index)` shows its size, high-water mark,
free and reusable blocks, directory entries and whether a maintenance run holds the lock; `sift.version()` is the version of the loaded
library (compare with `pg_extension.extversion`). `sift.promote`, `merge`, `verify`, `segments`, `segment_postings` and `index_info`
need the owner of the table or a role with `MAINTAIN` on it, like `VACUUM`. Disk, memory and time of a large index: [SIZING.md](SIZING.md).

- `REINDEX [CONCURRENTLY]` works as usual; `CREATE INDEX` is parallel on large tables. Changing `k1`, `b` or an analyzer option needs it.
- `pg_stat_progress_create_index` shows a build's phase (`scanning table`, `spilling runs`, `merging runs`, `writing segments`) and the documents done.
- A build writes its postings to temporary files (`temp_tablespaces`) before the segments: about 2.3 times the index size at the default
  `maintenance_work_mem`, about 4.3 times at `1MB` (a second merge pass), measured on 100,000 articles (64 MB index). Memory stays within the budget at any table size.
- pg_dump restores rebuild the index. Replication: see [Replication](#replication).
- `EXPLAIN` shows `Custom Scan (SiftScan)`, or `SiftCount` for the `count(*)` pushdown.

<!-- gen:gucs (generated by `scripts/gen-docs.py` from the settings the library defines in src/; do not edit by hand) -->
| setting | default | range | set by | meaning |
|---|---|---|---|---|
| `sift.agg_combo_cache` | 1,048,576 | 16 to 16,777,216 | any user | Distinct value combinations of an expression PostgreSQL evaluates for the aggregate scan kept before the cache starts again. |
| `sift.agg_dense` | on |  | any user | Aggregates the chunks of counts, sums and averages from the bitmap of their rows, without a list of tuples. |
| `sift.agg_fuse` | on |  | any user | Runs the UNION ALL arms that read the same rows in one pass over the index, as the grouping sets of one scan. |
| `sift.agg_join_filter` | on |  | any user | Tests the integer key leaf of an inner or semi lookup join against the keys of the loaded table before tuples are made (zone pruning, source filters, row bitmaps). |
| `sift.agg_key_skew` | on |  | any user | Prices the probes of the normal plan for the keys of a lookup join by the rows a key owns by the column's most common values, not by the mean rows of a key. |
| `sift.agg_lookup_cache_kb` | 256 MB | at least 0 kB | any user | Memory per backend that keeps the rows of the joined tables a scan loaded for the next query, while the table does not change (0: off). |
| `sift.agg_lookup_max_rows` | 1,000,000 | at least 0 | any user | Rows a sub-select, VALUES list or function joined to Custom Scan (SiftAgg) may return, as the planner estimates them: the rows go into a lookup table in memory when the scan starts; a larger one keeps the normal join. |
| `sift.agg_lookup_pushdown` | on |  | any user | Loads the rows a lookup join can match only: an index scan or scan keys on the table's own `column = value` conditions, and the other own conditions tested while loading. |
| `sift.agg_param_set_size` | 1000 | 0 to 100,000,000 | any user | Elements assumed for an array parameter of a set filter (col = ANY($1)) in a generic plan, to cost the index and bitmap scans against Custom Scan (SiftAgg); 0 keeps the planner's own guess. |
| `sift.agg_preagg` | on |  | any user | Answers the units of an aggregate scan whose rows are all visible from the pre-aggregates the sift_col index stored with them. |
| `sift.agg_rollup_cost` | 1.0 |  | any user | Scale of the cost of a tuple of a unit's rollup against the scan of an element: a unit is answered from its rollup only when that costs less (0: always). |
| `sift.agg_rows` | on |  | any user | Groups the rows of a plan that reads Custom Scan (SiftAgg) nodes with exact count(DISTINCT) in Custom Scan (SiftAggRows), without a sort of the rows. |
| `sift.agg_semi_lut` | on |  | any user | Answers a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer leaf from a byte table over the keys of the joined table. |
| `sift.agg_semi_lut_size` | 4 MB | 0 kB to 1024 MB | any user | Memory of the byte table of a semi or anti join, per scan and per worker; keys that span more keep the generic join. |
| `sift.agg_share_lookups` | on |  | any user | Lets the Custom Scan (SiftAgg) nodes of one statement that join the same table share the rows they load (UNION ALL arms). |
| `sift.agg_two_level` | on |  | any user | Aggregates an aggregate over per-row aggregates (UNION ALL arms grouped by a row id) in one scan of the index: the cells of a row are kept in the scan. |
| `sift.async_maintenance` | on |  | superuser | Promotes and merges segments in a dynamic background worker. |
| `sift.col_auto_merge` | off |  | superuser | After a VACUUM that left a sift_col index scattered by churn, a background worker seals its write buffer and merges all its segments into one. The rule is sift.col_auto_merge_dirty_pct, _fragmentation and _buffer_pct; the rewrite sleeps as sift.maintenance_cost_delay says and is skipped when the maintenance lock is held. Needs sift.async_maintenance; sift.col_maintain(index) applies the same rule from a job. Set it for the server or the database: the worker reads no session setting. |
| `sift.col_auto_merge_buffer_pct` | 1.0 |  | superuser | Percent of its rows the write buffer of a sift_col index holds above which the automatic rewrite runs. The rows of the buffer are read as one in-memory unit per superblock they touch, on every query, until a promotion. |
| `sift.col_auto_merge_dirty_pct` | 30 | 1 to 100 | superuser | Percent of the units of a sift_col index holding a dead row above which the automatic rewrite runs. Such a unit answers from its rows, not from its pre-aggregates, however few rows are dead. |
| `sift.col_auto_merge_fragmentation` | 2.0 |  | superuser | Units listed per superblock they cover from which the automatic rewrite of a sift_col index runs. Each promotion of the write buffer adds a segment over the superblocks the churn touched. |
| `sift.col_format_minor` | 6 | 3 to 6 | superuser | Column format minor of the segments written: 6, 4 to keep them readable by pg_sift 3.1, or 3 for pg_sift 3.0. Segments already written keep their minor; a 3.2 library reads them all. A segment of a schema with wide leaves is minor 5 at least, and the minor 6 of the complete kinds is written only for a schema with jsonb paths, from rows whose kinds are known. |
| `sift.col_rollup_on_demand` | on |  | any user | A scan that reads units for lack of their rollups asks the background worker to rebuild them. Nothing is written by the query; the next one answers from the rollups. Off: the units stay as they are until a merge or REINDEX. |
| `sift.col_rollup_worker` | on |  | any user | A request for a rollup refresh starts the background worker that makes it. Off: the requests wait in the queue until sift.col_rollup_refresh(index) is called. |
| `sift.enable_agg` | on |  | any user | Enables aggregate pushdown into sift_col column indexes (Custom Scan (SiftAgg)). |
| `sift.enable_col` | on |  | any user | Enables the column scans over sift_col indexes. |
| `sift.enable_count` | on |  | any user | Enables count(*) pushdown into the sift index. |
| `sift.enable_scan` | on |  | any user | Enables the sift custom scan (ranked and unordered). |
| `sift.enable_topn` | on |  | any user | Enables ORDER BY .. LIMIT from the column units (Custom Scan (SiftAgg) with Top-N), with sift.enable_agg. |
| `sift.filter_pushdown` | on | `off`, `on`, `force` | any user | Ranked scans take the tuples a bitmap of the other quals keeps: off, on (by cost) or force. |
| `sift.gpu` | off | `off`, `auto`, `on` | superuser | Runs the integer aggregates of a SiftAgg scan on a Vulkan device (docs/GPU.md): off, auto (above sift.gpu_min_rows) or on. A build without Vulkan support (make VULKAN=1) runs on the CPU whatever this says. |
| `sift.gpu_cache_mb` | 256 | 16 to 1,048,576 | superuser | Device memory, in MB, for the column chunks that stay on the Vulkan device between queries (per backend). |
| `sift.gpu_device` | empty |  | superuser | The Vulkan device sift.gpu uses: its index, or a part of its name. Empty: the best one. |
| `sift.gpu_min_rows` | 5,000,000 | at least 0 | any user | Rows of the index above which sift.gpu = auto uses the device for a grouped aggregate; an ungrouped one runs there only with sift.gpu = on. |
| `sift.grouped_min_df` | 8192 | at least 3 | any user | Testing: df from which a term's postings are GROUPED rather than LIST. |
| `sift.log_min_duration` | 1 s | at least -1 ms | superuser | Logs the promotions, merges and maintenance worker runs of an index that take at least this long (ms), -1 to log none. |
| `sift.maintenance_cost_delay` | 0 ms | 0 ms to 100 ms | superuser | Milliseconds the maintenance worker sleeps after every 16 pages (128 kB) its merges read or write, 0 to run them at full speed. |
| `sift.maintenance_naptime` | 2 s | 200 ms to 60 s | reload | Time between background maintenance workers of an index; a worker stays this long after it starts. |
| `sift.max_expansions` | 512 | 1 to 1,000,000 | superuser | Maximum terms a wildcard, regex or fuzzy leaf may expand to, and a prefix may be scored over. A prefix with more terms matches all of them and scores as the one term prefix_index would index. |
| `sift.parallel_scan` | off |  | any user | Lets the planner use parallel sift scans and counts (they pay off on large, heap-bound scans only). |
| `sift.prefetch` | on |  | any user | Reads ahead the heap pages an unordered scan is about to visit. |
| `sift.prefix_cache_size` | 8 MB | 0 kB to 1024 MB | superuser | Memory a backend keeps the postings of broad prefixes in (kB), 0 to keep none. |
| `sift.simd` | auto | `scalar`, `avx2`, `avx512`, `auto` | any user | Caps the SIMD level used by sift kernels. auto uses the best level the CPU supports. |
| `sift.standby_conflicts` | off |  | superuser | Logs the reuse of released blocks as a recovery conflict, so a standby without hot_standby_feedback waits for or cancels the queries that may still read them. Off, such a query fails with "segment recycled during scan, retry" when the blocks it reads are reused. |
| `sift.table_auto_defrag` | off |  | any user | A backend that closes a write buffer of a sift_table asks a background worker to rewrite its mostly dead column segments and give back its free space when it is worth it, without VACUUM. Needs sift.async_maintenance. The worker clears no index entry: VACUUM still has to run for those. |
| `sift.table_auto_defrag_delay` | 10 ms | 0 ms to 10 s | any user | Milliseconds the background pass of a sift_table waits after each segment it rewrites. 0 never waits. |
| `sift.table_auto_defrag_free_pct` | 25 | 1 to 100 | any user | Percent of the file that is free blocks from which the background pass of a sift_table gathers and returns them. |
| `sift.table_auto_defrag_indexes` | on |  | any user | The background pass of a sift_table whose dead rows reach sift.table_dead_rewrite_pct is a whole VACUUM, index cleanup and tombstone rewrite included. Off: it only clears the dead rows of the column segments and returns space, and the tombstone log and the index entries wait for a VACUUM. |
| `sift.table_compression` | empty |  | any user | Compression of the column segments a sift_table writes: auto, none, lz4, zstd or encodings; per column: 'auto; col=zstd:9'. |
| `sift.table_dead_rewrite_pct` | 30 | 1 to 100 | any user | Percent of dead rows above which VACUUM rewrites a column segment of a sift_table alone to drop them. 100 never rewrites. |
| `sift.table_init_gate` | 0 | at least 0 | any user | Test only: the first write of a sift_table waits for the advisory lock of this number before it creates the tombstone segment (0 = no gate). |
| `sift.table_inline_promote` | on |  | any user | Promotes the closed write buffers of a sift_table into columns while rows are inserted (VACUUM always does). |
| `sift.table_max_segments` | 128 | 16 to 128 | any user | Test only: the directory entries a sift_table may use. |
| `sift.table_seal_gate` | 0 | at least 0 | any user | Test only: the writer that closed a write buffer of a sift_table waits for the advisory lock of this number before it flags the pages sealed (0 = no gate). |
| `sift.table_vblock_floor` | 0 | 0 to 1,073,741,824 | any user | Test only: the first virtual block a new write buffer of a sift_table may take. |
| `sift.table_write_buffer_limit` | 4 MB | 2 MB to 8 MB | any user | Size of a sift_table write buffer (row format) before it is closed, to be promoted into columns. Rounded down to whole superblocks of 2 MB; a row has to fit one segment. |
| `sift.topn_walk` | on |  | any user | Lets a top-N scan visit the units of a sort column that does not follow the table by the best value of their zones, and stop at the first that cannot beat the n-th. |
| `sift.vacuum_rewrite_limit` | 128 | 0 to 1,048,576 | any user | Megabytes of segments a column VACUUM may rewrite to merge (past the first merge step), with a separate equal allowance to copy the end of the file into free blocks below it, which holds the table or index under an exclusive lock; 0 keeps the one merge step and moves nothing. |
<!-- /gen:gucs -->

Index options go in `WITH (...)` at `CREATE INDEX`; `write_buffer_limit`, `vacuum_segments`, `merge_fanin`, `dead_rewrite_pct` and `vacuum_truncate` can be changed afterwards (`ALTER INDEX ... SET (...)`), the rest need `REINDEX`.

| Index option | default | range | meaning |
|---|---|---|---|
| `k1` | 1.2 | 0-1000 | BM25 term-frequency saturation |
| `b` | 0.75 | 0-1 | BM25 length normalization |
| `analyzer` | `text` | `text`, `keyword` | tokenize words, or index the whole value as one term |
| `stemmer` | none | `none`, `en` | Snowball English stemming |
| `case_folding`, `accent_folding` | true |  | fold case / accents |
| `positions` | true |  | store positions (needed for phrase queries) |
| `prefix_index` | 0 | 0-15 | prefix lengths indexed for fast autocomplete |
| `fields` |  |  | columns weighted as separate fields ([several columns](#analyzers-phrases-several-columns)) |
| `write_buffer_limit` | 4096 | 64-1048576 kB | size of the write buffer before it is sealed into a segment; also the base size of the merge tiers; `mutable_limit`, the name of 1.x and 2.0, is a deprecated alias that is still accepted |
| `merge_fanin` | 8 | 2-32 | segments of one size tier that trigger a merge; a lower value merges sooner and keeps fewer segments, a higher one merges less |
| `dead_rewrite_pct` | 50 (`sift_col`: 10) | 1-100 | percent of dead documents above which maintenance rewrites a lone segment to drop them; 100 never rewrites |
| `vacuum_truncate` | true |  | VACUUM gives the free blocks at the end of the index file back to the operating system (takes an exclusive lock on the index only if it is free right away) |
| `build_segments` | 0 | 0-64 | force N segments at build and disable the parallel build |
| `vacuum_segments` | 2 | 1-128 | segments VACUUM leaves the index with; more keeps VACUUM cheaper and queries slightly slower |

`vacuum_segments` is the number of directory entries (the write buffer included) that `VACUUM`, autovacuum and the maintenance worker's
merges leave an index with. `ALTER INDEX ... SET (vacuum_segments = N)` takes effect on the next `VACUUM`; no `REINDEX`, and an index built
before the option keeps working with the default. A value out of 1 to 128 is an error; an index that already has N segments or fewer is left as it is.
A segment more than `merge_fanin` times the size of the others that would merge with it stays: its rewrite would cost more than the count gains (a 0.1 MB segment next to one of 11 MB would rewrite the 11 MB at every `VACUUM`), so an index may keep more than N segments when sizes differ that much; `sift.merge(index, true)` merges them all. A segment of which more than `dead_rewrite_pct` percent of the documents are dead is rewritten at any N.
The trade-off, measured on 60 segments of 50,000 documents: a lower N is more merging work per `VACUUM` and faster queries, a higher N the opposite.

| `vacuum_segments` | `VACUUM` | `count(*)` | ranked top-10 |
|---|---|---|---|
| 2 (default) | 0.19 s | 0.08 ms | 0.18 ms |
| 4 | 0.15 s | 0.14 ms | 0.21 ms |
| 8 | 0.12 s | 0.22 ms | 0.28 ms |
| 16 | 0.10 s | 0.35 ms | 0.33 ms |

`merge_fanin` is the number of segments of one size tier (a tier is a factor of `merge_fanin` in size, starting at `write_buffer_limit`) that
makes the background worker, the inserters that need room and `sift.merge()` merge them, and the most one merge takes is twice that. A
lower value merges sooner and more often, which is more write work and fewer segments to query; a higher one merges less and keeps up to
`merge_fanin` - 1 segments per tier. It is read when a merge is chosen: `ALTER INDEX ... SET (merge_fanin = N)` applies to the next one, with no
`REINDEX`, and an index built before the option keeps working with the default. A value out of 2 to 32 is an error. The limits the
policy keeps on the directory follow it: more than `greatest(32, 2 * merge_fanin)` entries merge the smallest segments whatever their tier, and inserters
still merge themselves from 112 of the 128 entries, so no value in range can fill a directory. Merging 39 segments of similar size leaves 1
segment at 2 to 8, 8 at 16, 6 at 17 and 1 at 31 and 32 (`test/sql/merge_fanin.sql`).

`sift.maintenance_cost_delay` slows the merges of the maintenance worker so that they leave I/O to the queries and inserts: after every 16 pages
(128 kB) the merge reads (the segments it merges) or writes (the new segment) the worker sleeps that many milliseconds. A worker merge of a
10 MB index took 0.27 s at the default 0 and 5.3 s at 20, at most 6.4 MB/s of I/O (128 kB per 20 ms); set it per database (`ALTER DATABASE ... SET`) or in `postgresql.conf`, as the worker reads the
settings of its database. It applies to the worker only: `VACUUM`, `sift.merge()` and the merges inserters do for room are never delayed, and neither
are the worker's promotions of write buffers, which the writers wait for.

`VACUUM` rewrites a segment alone only above `dead_rewrite_pct` percent of dead rows (default 10), but a `DELETE` or `UPDATE` spread over the table puts a dead
row in every unit long before that, and a unit with a dead row answers from its rows and not from its pre-aggregates. `sift.col_auto_merge` (above) is the answer: the
`VACUUM` that sees the rule satisfied starts a worker that does what `sift.col_promote` followed by `sift.col_merge(index, true)` does, without blocking readers or writers.
`sift.col_maintain(index)` applies the same rule in a session (a `pg_cron` job after the nightly `VACUUM`: `SELECT sift.col_maintain('idx')`; false when nothing is due or the
maintenance lock is held, never in a read-only transaction or on a standby) and `sift.col_maintenance_due(index)` tells why it would (`dirty units`, `fragmented units`, `write buffer rows`).
The rewrite writes the new segment before it frees the old ones, so the index file is about twice its live size until the `VACUUM`s that follow reuse and give back the free end; that and the unthrottled default of `sift.maintenance_cost_delay` (0) are why the setting is off by default: turn it on for a database with scattered churn, with a delay of a few milliseconds.

`dead_rewrite_pct` is the share of dead documents above which maintenance rewrites a lone segment to drop them and their terms (a segment
that no merge picks; the dead postings are cleared by `VACUUM` either way). `ALTER INDEX ... SET (dead_rewrite_pct = N)` applies to the next
`VACUUM` or worker step, with no `REINDEX`; 100 never rewrites a segment on its own. A `sift_col` index has the same option with the default 10: a unit with a dead row answers from its rows, not from its pre-aggregates ([docs/COL-PREAGG.md](COL-PREAGG.md)), so a lone segment is rewritten sooner. The BM25 default 50 was measured against 5 on 400,000
documents of 40 words, 10 rounds of updating or deleting 6% of the rows, each followed by `VACUUM` (mean of 5 runs); 5 rewrites the segment
at every round and costs more everywhere except the queries after the deletes:

| load | `dead_rewrite_pct` | `VACUUM` total | WAL | index file | `count(*)` | ranked top-10 |
|---|---|---|---|---|---|---|
| update | 50 (default) | 20.2 s | 1,340 MB | 92 MB | 1.06 ms | 3.9 ms |
| update | 5 | 37.8 s | 1,428 MB | 133 MB | 1.40 ms | 5.0 ms |
| delete | 50 (default) | 2.3 s | 86 MB | 67 MB | 3.5 ms | 4.1 ms |
| delete | 5 | 15.8 s | 455 MB | 89 MB | 0.15 ms | 1.5 ms |

`write_buffer_limit` (kB, 4096) is the size at which the open write buffer is sealed for promotion; it is also where the merge tiers start.
`ALTER INDEX ... SET (write_buffer_limit = N)` applies from the next seal. 1024 was measured against 4096 on
100,000 documents of 40 words (mean of 5 runs): it promotes and merges faster per step and leaves queries on the grown index faster, but it
does that 3.7 times as often and the writers pay for it, so the default stays 4096:

| `write_buffer_limit` | 16 clients, `INSERT` | `INSERT .. SELECT` | `COPY` | promotion / merge | index, segments | `count(*)` / top-10 OR / top-10 AND |
|---|---|---|---|---|---|---|
| 4096 (default) | 107k rows/s, p99 0.69 ms | 113k rows/s | 261k rows/s | 37 ms / 1.4 s | 72 MB, 17 | 11.5 / 12.5 / 6.1 ms |
| 1024 | 87k rows/s, p99 0.81 ms | 94k rows/s | 137k rows/s | 13 ms / 0.43 s | 98 MB, 78 | 10.6 / 4.1 / 2.6 ms |

`VACUUM` also gives the free blocks at the end of the index file back to the operating system, as it does for a heap (`sift.index_info(index)`:
`relation_blocks` falls to `high_water`). It tries when free space amounts to 1000 blocks or a 16th of the index, takes `AccessExclusiveLock` on the
index only if that is free right now, and lets go of it as soon as the file is cut; if any query, insert or maintenance step holds the index it
does nothing, and the next `VACUUM` tries again (it says so at `DEBUG2`; `VACUUM VERBOSE` shows the new size of the index). A hot standby
cancels the queries that hold the index when it replays that lock (or delays replay up to `max_standby_streaming_delay`), as for a heap
(`hot_standby_feedback` does not prevent it; `vacuum_truncate = false` does). A
`VACUUM` that runs with parallel workers (it vacuums several large indexes at once) cannot take the lock, which needs a transaction id, and
leaves it to the next one: `VACUUM (PARALLEL 0)` and autovacuum do truncate. Under the exclusive lock, newly released space can be reused
immediately and tail segments can be relocated into interior free runs before truncation, all in the same `VACUUM`. See COL §2.7
for the write allowance and conditions that defer relocation. Free pages left inside the file remain reusable by later writes. `ALTER INDEX ... SET (vacuum_truncate = false)`
turns the truncation off for an index (no `REINDEX`), like `vacuum_truncate` of a heap table.

**Merging is automatic.** Sealed segments are merged `merge_fanin` (8) at a time per size tier, the directory (128 entries) is kept under `greatest(32, 2 * merge_fanin)` by merging the
`merge_fanin` smallest, inserts wait for the worker at 112 entries, and an insert seals the lane itself when 4 segments are waiting. VACUUM merges
down to `vacuum_segments`, except that a segment more than `merge_fanin` times larger than the ones before it is left alone (`sift.merge(index, true)` merges
it anyway). `write_buffer_limit`, `merge_fanin` and `dead_rewrite_pct` scale and tune these thresholds; the rest are constants.

**Cost of one library (3.0).** `pg_sift` 3.0 carries the BM25 index and the column engine in one library, so the library is about twice the size of 2.0's and each new backend that loads it links more symbols. Against 2.0.x (`bench/steps/cpu_gate.sh`, user instructions per operation) three BM25 cells are accepted above 1.02: the first query of a session (`session`, about 1.04 to 1.05), a `VACUUM` with nothing to free (`vacuum_nofree`, about 1.04, on 3.9M instructions) and a ranked query on a one-segment index (`idle1_rank`, 1.02 to 1.03). Every other cell (queries, inserts, `COPY`, `CREATE INDEX`, `sift.merge`, `sift.promote`, `VACUUM` with work) is within 1.02. Connection pooling amortizes the session cost; a deployment that needs neither `sift_col` nor `sift_table` pays it for nothing.

## Column indexes, aggregates and sift_table

Three pieces share the column engine; none needs `shared_preload_libraries`, and `CREATE EXTENSION pg_sift` (3.0) installs all of them.

- **`sift_col` index** over a heap table (`CREATE INDEX ... USING sift_col` on a `sift_table` is refused: it is already in column format and `SiftAgg` reads it as it is): `CREATE INDEX ON orders USING sift_col (tenant_id, status, total, (placed_at::date))` keeps those
  columns once more, column by column, with zone maps; filters and aggregates read it instead of the heap rows, and the heap stays authoritative (MVCC
  is the table's). Types: integers, `bool`, dates and timestamps, `float4`/`float8`, `numeric(p, s)` of any precision, unconstrained `numeric`, `money`, `interval`, `time`, `timetz`, enums, `uuid`, `bytea`, `inet`, `cidr`, `macaddr`, `macaddr8`, `bit`, `varbit`, arrays, ranges and multiranges, `char(n)`, `name`, and `text` of any length (up to 1 GB; a unit with strings over 255 bytes stores them inline). Reloptions: `write_buffer_limit` (kB, 4096, like the
  `sift` index) and `paths` for `jsonb` attributes (`WITH (paths = 'metrics.* float4, items[]{sku text, qty int4}')`, [docs/COL-SHRED.md](COL-SHRED.md)) and `compression` (`auto`, `none`, `lz4[:N]`, `zstd[:N]`; per column `'auto; col=zstd:9'`; needs `REINDEX`) and `preagg` (`default`, `off` or `cube(dim: measure, ...)`, the pre-aggregates written with every unit; a change applies to the segments written afterwards, [docs/COL-PREAGG.md](COL-PREAGG.md)) and `dead_rewrite_pct` (default 10).
  Maintenance is the same as for `sift` indexes (background worker, VACUUM); `sift.col_index_info`, `col_segments`, `col_leaves`, `col_verify`, `col_promote`,
  `col_merge`, `col_maintain` and `col_maintenance_due` inspect and drive one (`col_merge(index, true)` seals the open write buffer and merges every segment into one: `col_promote` and the merge in a call, without blocking readers or writers).
  `CREATE INDEX` and `REINDEX` (also `CONCURRENTLY`) are parallel on large tables with the workers PostgreSQL plans (`max_parallel_maintenance_workers`, `min_parallel_table_scan_size`,
  the table's `parallel_workers`, 32 MB of `maintenance_work_mem` for each process): each process writes the segments of the superblocks it took, so the index has a segment per process until a merge,
  and its file is longer by the unused end of each process's last run (free space, up to 64 MB each); the rows it serves are those of a serial build.
- **Aggregate pushdown:** a `GROUP BY` over a table with a `sift_col` index (or over a lateral unnest of an indexed `jsonb` array) with `count`, `count(DISTINCT)`, `sum`,
  `avg`, `min`, `max`, filters and `IN` lists runs as `Custom Scan (SiftAgg)` when the planner can prove the result equals the plain plan's; anything else keeps
  its plan ([docs/COL-PLAN.md](COL-PLAN.md), [docs/AGG.md](AGG.md)). `SET sift.enable_agg = off` compares the two; `scripts/coldiff.sh` does it for a whole corpus.
  A partitioned table works the same when every partition the query reads has a `sift_col` index of the same columns and options: create it per partition or
  once on the parent (`CREATE INDEX ON parent USING sift_col (...)` builds one per partition, also for later ones); `EXPLAIN` lists the `Partitions:` ([AGG.md](AGG.md), "Partitioned tables").
- **`sift_table`:** `CREATE TABLE t (...) USING sift_table` is a table access method with native MVCC, btree and `sift` indexes, VACUUM, generic WAL and `pg_dump`
  support ([docs/COL-TAM.md](COL-TAM.md)). New rows go to row-format write pages; when a write buffer fills, or VACUUM or autovacuum runs, the settled rows become compressed column segments with the same TIDs (`sift.table_segments('t')` lists them with their column format minor, `sift.table_space('t')` the file and its free blocks, `sift.table_maintain('t')` does VACUUM's work on the segments and the file without its index cleanup: [COL-TAM §7](COL-TAM.md); settings: `sift.table_write_buffer_limit`, `sift.table_inline_promote`, `sift.table_compression`, `sift.table_dead_rewrite_pct`, in the settings table of [Maintenance and operations](#maintenance-and-operations)). VACUUM clears deleted rows out of the column segments, rewrites a segment once more than `sift.table_dead_rewrite_pct` percent of it is dead, and, when it can take the exclusive lock, can reuse newly released space and relocate tail segments into interior free runs before returning the free tail to the operating system in the same VACUUM (enabled by default; `vacuum_truncate = false` or `VACUUM (TRUNCATE FALSE)` disables this exclusive-lock reuse, relocation and tail-truncation step while other ordinary maintenance continues; allowance and deferral conditions in COL §2.7); `VACUUM FULL` and `CLUSTER` write the table anew as column segments. Every type is kept exactly. Its limits are listed once, under [Limits](#limits); publishing one under `wal_level = logical` is under [Replication](#replication).

```sql
CREATE TABLE orders (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), created_at timestamp);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, created_at, (date_trunc('month', created_at)::date));

SELECT category, count(*), sum(amount) FROM orders
WHERE country = 'DE' AND created_at >= '2025-06-01' GROUP BY category;          -- Custom Scan (SiftAgg) on orders_col
SELECT date_trunc('month', created_at)::date AS month, count(DISTINCT customer_id), avg(amount) FROM orders
WHERE status = 'shipped' GROUP BY 1 ORDER BY 1;                                  -- the month is an indexed expression of its own

SET sift.enable_agg = off;      -- the plan PostgreSQL has without sift, to compare results and times
CREATE TABLE events (id bigint, kind text, price numeric(10,2), at timestamp) USING sift_table;   -- the table itself in column format
```

Index what you filter and group by; an expression such as the month above is a column of its own and a query must write it the same way.

PostgreSQL 17-19 table access methods cannot define their own `WITH (...)` options (a patch targets PostgreSQL 20), so a `sift_table` is tuned with PostgreSQL settings (`SET`, `ALTER DATABASE ... SET`, `postgresql.conf`), not per table; a `sift_col` index has real options stored with the index (`CREATE INDEX ... WITH`, `ALTER INDEX ... SET`). Background promotion by `VACUUM`, autovacuum and the maintenance worker uses the server or database value of the setting, not your session's; the exact rule is under [Limits](#limits).

| option | `sift_col` (per index) | `sift_table` (setting) |
|---|---|---|
| compression of the column chunks | `compression` (`auto` default, `none`, `lz4[:N]`, `zstd[:N]`, per column `'auto; col=zstd:9'`; needs `REINDEX`) | `sift.table_compression` (empty = `auto`; same grammar) |
| size of the write buffer before it is sealed | `write_buffer_limit` (kB, `ALTER INDEX ... SET`) | `sift.table_write_buffer_limit` (kB) |
| promotion while rows are inserted | not applicable (the index follows the heap) | `sift.table_inline_promote` (`on`; `off` leaves it to `VACUUM`) |
| dead rows | `dead_rewrite_pct` (per index) | `sift.table_dead_rewrite_pct` (percent of a segment) |

A `sift_table` has no `paths` or rows-per-unit setting: a `jsonb` column is stored as its exact bytes and units follow the 2 MB superblocks.

The chunk codec compresses the chunks of a `sift_col` index (option `compression`: `auto`, `none`, `lz4`, `zstd`, or per column `'auto; col=zstd:9'`). It is built into the library; `make SIFT_NO_LZ4=1` or `SIFT_NO_ZSTD=1` build without one.

### Set filters

A filter on a set of ids (the members of a group, the entities that have an attribute) runs in `Custom Scan (SiftAgg)` as an array parameter, a literal `IN` list, or a
join to the table that holds the set. [docs/SET-FILTERS.md](SET-FILTERS.md) has the measurements and the settings; the array parameter is the fast one: send the ids as one
`int4[]` of a prepared statement on a generic plan (`SET LOCAL plan_cache_mode = force_generic_plan`), and keep a small set (about 100 ids) on a custom plan.

The array form pushes down for these key types (the element type of the array is the type of the column; cast the array, `ANY($1::int8[])`, `ANY($1::text[])`, `ANY($1::date[])`):

| key column | array | how `SiftAgg` tests it |
|---|---|---|
| `int2`, `int4`, `date`, `int8`, `timestamp`, `timestamptz` | the same type of array (an `int4[]` for an `int8` column too) | a byte table over the range of the array (up to 4 MB), else a sorted array (32-bit types) or a hash table (64-bit types) |
| `text` | `text[]` | PostgreSQL evaluates `col = ANY($1)` once per distinct value of the column, with a hash table of the array built on the first value |
| `uuid`, `bytea` |  | not supported: a `sift_col` index refuses these columns, so the query keeps the normal plan |

A `NULL` element matches nothing (`= ANY`) and makes `<> ALL` empty, an empty array keeps no row (`= ANY`) or every row (`<> ALL`), as in SQL. The timings by type are in
[docs/SET-FILTERS.md](SET-FILTERS.md), section 2.

```c
/* libpq: one text parameter of type int4[] (OID 1007) */
Oid         types[1] = {1007};
const char *vals[1] = {"{101,102,103}"};
PGresult   *r = PQexecParams(conn, "SELECT channel_id, count(*) FROM events WHERE dim_a_id = ANY($1::int4[]) GROUP BY 1", 1, types, vals, NULL, NULL, 0);
```

```ruby

# Sequel: the pg_array extension binds the array as one parameter of a server-side prepared statement
DB.extension :pg_array
ps = DB[:events].where(Sequel.lit('dim_a_id = ANY(?::int4[])', :$ids)).select_group(:channel_id).select_append { count.function.* }.prepare(:select, :by_set)
DB.transaction { DB.run 'SET LOCAL plan_cache_mode = force_generic_plan'; ps.call(ids: Sequel.pg_array(ids, :int4)) }
```

## Replication

**Physical:** the index goes through generic WAL, so a hot standby (`pg_basebackup -R`) replays builds, inserts, VACUUM,
`CREATE INDEX CONCURRENTLY` and `REINDEX`, and serves `@@>`, ranked `sift.score()`, `count(*)` and pushed-down filters read-only with the
primary's rows and scores; a promoted standby takes writes at once. No `shared_preload_libraries`, but install the same pg_sift version on
every node that queries. `sift.*` settings are per node; `hot_standby_feedback = on` avoids retryable "segment recycled during scan" errors. Without feedback, `sift.standby_conflicts = on` on the primary logs recovery conflicts for reused space instead: standby scans are waited for (`max_standby_streaming_delay`) or cancelled like any standby query, never served recycled pages; it also delays standby replay for other long snapshots, so it is off by default.

**Logical:** only table rows replicate, never indexes or `CREATE EXTENSION`. On the subscriber install pg_sift, create the table and its own
sift index (before or after the initial sync) and the apply worker maintains it, with a primary key or `REPLICA IDENTITY FULL` (a sift index
is never a replica identity). Matches and counts equal the publisher's, and so do scores while both indexes saw the same changes: BM25 counts
postings that are not yet vacuumed, so an index rebuilt on one side only (`REINDEX`) scores slightly differently.

```sql
CREATE PUBLICATION docs_pub FOR TABLE docs;                                                -- publisher
CREATE EXTENSION pg_sift;                                                                  -- subscriber
CREATE TABLE docs (id int PRIMARY KEY, title text, body text, published date, category int);
CREATE INDEX docs_body ON docs USING sift (body);
CREATE SUBSCRIPTION docs_sub CONNECTION 'host=primary dbname=app' PUBLICATION docs_pub;
```

**sift_table:** a table created `USING sift_table` replicates physically like any table (generic WAL, no preload). To *publish* one under
`wal_level = logical`, `pg_sift` must be in `shared_preload_libraries` on the publisher and on every standby of it: the logical change records
use a custom WAL resource manager, which a server can only register at preload time. Without the preload a DML on the published table fails
with `0A000` (`sift_table table "t" is published ... but pg_sift is not in shared_preload_libraries`, hint: preload or publish a heap staging
table); unpublished sift_table tables never need it. INSERT, UPDATE, DELETE and TRUNCATE reach a heap or sift_table subscriber, UPDATE and
DELETE with a primary key or `REPLICA IDENTITY FULL` as for heap.

`scripts/replication.sh` checks all of it on a primary, a standby and a subscriber in temporary clusters; `--container` puts every node in
its own docker container.

## Limits

- PostgreSQL 17 and 18, 19 experimental; x86-64 tested (other architectures untested); heap and sift_table tables; BLCKSZ 8192; database encoding UTF8 or SQL_ASCII.
- `sift_table` (full list in [docs/COL-TAM.md](COL-TAM.md), section 9):
  - at most 127 columns (`CREATE TABLE` fails with `54000`; an `ADD COLUMN` past it succeeds and the next write fails until a column is dropped), 128 directory entries, `BLCKSZ` 8192;
  - a single `COPY` or `INSERT ... SELECT` writes any number of rows in one transaction (the write buffers of one command become column groups that carry their writer while it runs, and `VACUUM` freezes them once it is committed or drops them if it aborted; a 10,000,000-row `COPY` takes about 180 MB in the backend). A transaction that writes its rows with many commands (a loop of single-row `INSERT`s, or several sessions into the same write buffers) cannot do that: its rows are promoted only once it has ended, so it is limited to about 126 write buffers of `sift.table_write_buffer_limit` (504 MB of row format at the default, 1 GB at 8192 kB), and a larger one fails with `54000` (the message says so: load with `COPY` or `INSERT ... SELECT`, or in several transactions); writers merge the column segments as they go;
  - `0A000` for a DML on a published table without the preload (see [Replication](#replication)), and an aggregate scan that reads a column holding a non-canonical float NaN (`SET sift.enable_agg = off` for that query);
  - `sift_table` has no pre-aggregate rollups of array elements, and a jsonb path is read only once it is declared and every segment holds its leaf (`VACUUM FULL`); no HOT, and shared row locks degrade to exclusive;
  - `xmax` of a row locked by several transactions is the last locker's, not a MultiXactId;
  - no per-table options: `sift.table_write_buffer_limit`, `sift.table_inline_promote`, `sift.table_compression` and `sift.table_dead_rewrite_pct` are ordinary settings that the backend doing the work reads when it does it (PostgreSQL 17 through 19 give a table access method no reloptions of its own: `WITH` knows heap's and TOAST's only and `amoptions` is for indexes; the patch that adds them, commitfest entry 5631, targets PostgreSQL 20). Inline promotion and the closing of write buffers follow the settings of the inserting session (`SET`, `SET LOCAL`, the role's or the database's default); a manual `VACUUM`, those of the session that runs it; autovacuum has no session, so it applies the server settings (`postgresql.conf`, `ALTER SYSTEM`), `ALTER DATABASE ... SET`, and the `ALTER ROLE ... SET` defaults of the bootstrap superuser (the role autovacuum runs as), never the writer's session or the owner's role; the maintenance worker does not touch a `sift_table`. The settings of the session that wrote the rows are not kept for the VACUUM that promotes them later. So set what background promotion should use at the server or database level; a `SET` in a session shapes only that session's inserts and its own VACUUM (the segments already written keep their codec until a merge or rewrite). `sift_col` indexes do have per-index options (`compression` and the rest, see above), so a column index on a table needs no setting for it. Tested by `test/t/010_tbl_settings.pl`;
- One key per index: multicolumn, `INCLUDE` and unique indexes and non-text keys are rejected (build a key of several columns with `sift.sections`). No plain or index-only scans.
- Queries: at most 256 leaves, nesting depth 32, phrases of 128 tokens, boosts up to 10000.
- `fields`: at most 14 field names, 128 bytes of names in all; `prefix_index` up to 15.
- Terms are truncated at 255 bytes, `tf` saturates at 255 and document length at 65535; at most 128 segments per index. A merge reads at most 16 GB of segments: a segment of that size is not merged again (VACUUM still clears its dead postings through the liveness bits).
- Analyzers: English Snowball stemmer only, no stopword list, fixed per index (REINDEX to change).
- `positions = on` is the default and makes the index about 2.5x larger (`positions = off` restores the small index). Phrases are
  rechecked in the heap for documents without positions: indexes built by earlier versions until `REINDEX`, `positions = off`, rows
  inserted since the build, and phrases under `NOT`.
- `sift.score()` in `WHERE` is only a comparison with a constant (`>`, `>=`, `<`, `<=`); any other use is an error, read the score from a subquery
  to filter on it. It works with `@@> ANY (array)` (the OR of its
  queries, scored like the same OR in one query text), but not in `UPDATE`, `DELETE`, `MERGE` and `FOR UPDATE`.
- Under `SERIALIZABLE` a sift scan locks the whole relation (the index access method has no finer predicate locks), so concurrent writers
  can make a serializable transaction fail with `40001`: retry.
- Row-level security and security barrier views: a role under a policy gets no ranked scan, no `count(*)` pushdown and no index estimate
  (the document frequencies would include the rows the policy hides), so `sift.score()` raises an error for it and its matches come from
  a sequential scan; the table owner and roles that bypass row-level security are not affected ([SIZING.md](SIZING.md#features-and-their-costs)).
- Partitions: `sift.score()` needs a sift index on every partition the query reads (otherwise an error; unranked matching and `count(*)`
  still work).
- Partitions: BM25 statistics are per partition, so scores can differ across partitions.
- Partitions: `sift.max_score()` is not available through a parent (call it on a partition).
- Partitions: the score cannot be used inside an aggregate or a non-nested-loop join; compute it in a subquery with `OFFSET 0` or `LIMIT`.
- Ranked partition scans are never parallel, and neither are scans with a pushed-down filter.
- Keyword facets (`sift.facets`): text keys only (use an expression index for integers), cost linear in the distinct values, tables with row-level
  security are refused. `sift.facet_counts` has none of these limits; its array facets and filters on columns outside the `sift_col` index use the
  ordinary plan.
- Fields: weights are set at query time over the sections of one key; no BM25F, no analyzer per field, phrases and regular expressions are not
  scoped to a field, no highlight per field ([FIELDS-FACETS.md](FIELDS-FACETS.md)).
- pg_upgrade is tested within one major version only. `pg_sift_test` (built with `SIFT_TESTING=1`) is for the test suite only.
