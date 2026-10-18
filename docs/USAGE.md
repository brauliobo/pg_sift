# pg_sift usage

Reference for everything beyond the [README](../README.md) quick start. The examples use the `docs` table created there.

## Queries

`AND`, `OR`, `NOT` are keywords in upper case only; juxtaposition is `AND`; precedence is `NOT` > `AND` > `OR`; a `NOT` needs a positive
term in its AND group. A wildcard, regex, fuzzy or prefix leaf expands to dictionary terms. It matches every word it matches, however many (`h*`,
`*ing`, `/.*tion/` or `~2` on a million names), and above `sift.max_expansions` (512) terms it scores as the one term whose documents
are those with such a word (for a prefix, the term `prefix_index` would hold for it), instead of as the best of its terms. A regex
cannot have back-references.

```sql
SELECT id FROM docs WHERE body @@> 'breakfast NOT noisy';             -- exclusion: 2
SELECT id FROM docs WHERE body @@> '{pool spa noisy}';                -- any of a set of words or patterns
SELECT id FROM docs WHERE body @@> '"friendly staff"';                -- phrase: 2, 5
SELECT id FROM docs WHERE body @@> 'brea* OR n?isy OR /no.*y/ OR frendly~1';  -- prefix, wildcard, regex, fuzzy
SELECT id FROM docs WHERE body @@> '(pool OR spa) AND friendly^3';    -- grouping and boost
SELECT id FROM docs WHERE body @@> ANY (ARRAY['pool', 'spa']);        -- OR of several queries, a bitmap scan (no sift.score)
```

## Ranking, filters and joins

`ORDER BY sift.score(ctid) DESC LIMIT k` runs a ranked scan that skips blocks that cannot reach the top k; ties break by ctid.
`sift.score()` is computed by the scan: use it in the select list or `ORDER BY` of the query level that scans the table, not in `WHERE`
(filter on a score from a subquery). A prepared `LIMIT $1 [OFFSET $2]` gets the same scan as a constant one, also in a generic plan.
It works on materialized views too. It is not available in `UPDATE`, `DELETE`, `MERGE` or `FOR UPDATE` (error when planned): rank in a
`SELECT` and change or lock the rows by key.

```sql
SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM docs WHERE body @@> 'friendly OR pool'
                   ORDER BY s DESC LIMIT 100) top WHERE s > 0.5;  -- filter on the score
SELECT d.id, c.name FROM docs d JOIN (VALUES (1, 'travel')) c(id, name) ON c.id = d.category
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

For autocomplete, separate sections with `chr(29)`, name the leading ones with `fields` and set `prefix_index = n` (up to 15) so prefixes of
up to n letters are one term lookup. The option is a speed and size trade-off, not a requirement: without it a broad prefix such as `h*` is read
from the postings of all its words, once per backend and then from a cache (`sift.prefix_cache_size`): 10-15 ms for a one-letter prefix of a million
names, under a millisecond with the option, which costs index size (a million names: 17 MB, 34 MB at 3, 73 MB at 15):

```sql
CREATE TABLE places (id int PRIMARY KEY, name text, admin1 text, country text);
CREATE INDEX places_sift ON places USING sift ((name || chr(29) || admin1 || ' ' || country))
  WITH (fields = 'name', prefix_index = 3);
SELECT id FROM places WHERE (name || chr(29) || admin1 || ' ' || country) @@> '(gr* ho*) AND (name:{gr* ho*}^3 OR (gr* ho*))'
ORDER BY sift.score(ctid) DESC LIMIT 20;                          -- every word a prefix, name matches boosted x3
```

`name:word`, `name:(a b)` and `name:{a b}` need the field in the index: a name it lacks is an error (`42601`, listing the fields) and
a query on one key means the same on every plan. Without `fields` (also with no sift index at all) a colon is text, so `name:word` is
the word `name:word` and `name:(a b)` the words `name:a` and `name:b`. With several sift indexes on one key, `@@>` means the default
analyzer when one of them has it, else that of the lowest OID; an index that answers differently (one with `fields` next to a plain
one) does not serve it, and `sift.match_idx(key, query, index)` names the analyzer. Partitions bind `@@>` to their own indexes.

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

## Keyword facets

`analyzer = keyword` indexes each whole value as one term, so `sift.facets(index, facet_index, query, lim, nulls)` counts the matches of a
query per value of another column from the bitmaps, equal to the `GROUP BY` of the same snapshot. It pays for broad queries on
low-cardinality columns (1.9-4.5x on 26 values); for selective queries or thousands of values `GROUP BY` wins ([DESIGN.md](../DESIGN.md) (l)).
A value of more than 255 bytes is indexed as its first bytes and a hash of all of it, so `@@>` still finds exactly the rows with that value
(`sift.facets` shows it as that start and the hash); it was an error before.

```sql
CREATE INDEX docs_category ON docs USING sift ((category::text)) WITH (analyzer = keyword);   -- text keys: cast integers
SELECT * FROM sift.facets('docs_body', 'docs_category', 'friendly OR pool', 10);      -- value, count: 1 | 3
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

Writes go to small mutable segments through generic WAL; a background worker (`sift.async_maintenance`) turns them into immutable segments
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

| GUC | default | meaning |
|---|---|---|
| `sift.max_expansions` | 512 | superuser setting: terms a wildcard, regex, fuzzy or prefix leaf scores individually; above it the leaf scores as one term |
| `sift.prefix_cache_size` | 8MB | superuser setting: memory per backend for the postings of broad leaves, `0` keeps none |
| `sift.filter_pushdown` | on | `off`, `on`, `force`: give a ranked scan a bitmap of the other quals |
| `sift.parallel_scan` | off | let the planner add parallel SiftScan / SiftCount plans |
| `sift.async_maintenance` | on | promote and merge in a background worker; off leaves it to the inserts and `VACUUM` (superuser) |
| `sift.maintenance_naptime` | 2000 | ms (200 to 60000, `postgresql.conf` or `ALTER SYSTEM`): a background worker is started at most this often for an index, and stays that long after it started, so the seals of a bulk load that fall in between are promoted too |
| `sift.log_min_duration` | 1s | log promotions, merges and maintenance worker runs of at least that long, and inserts that wait for the worker (`-1` none, superuser) |
| `sift.simd` | auto | `auto`, `scalar`, `avx2`, `avx512`: caps the SIMD level of every kernel (BM25, column, aggregation, shredder, codec); every level returns the same results bit for bit |
| `sift.enable_col` | on | use `sift_col` indexes for column scans |
| `sift.enable_agg` | on | push `GROUP BY` aggregates down to `sift_col` indexes and `sift_table` tables (`Custom Scan (SiftAgg)`); `off` gives the plan PostgreSQL has without sift |
| `sift.maintenance_cost_delay` | 0 | milliseconds (0 to 100) the maintenance worker sleeps after every 16 pages (128 kB) its merges read or write, like `vacuum_cost_delay`; `0` runs them at full speed (superuser) |

Reloptions (`WITH (...)`; all but `mutable_limit`, `vacuum_segments`, `merge_fanin`, `dead_rewrite_pct` and `vacuum_truncate` need REINDEX): `k1` (1.2), `b` (0.75), `analyzer` (`text` or `keyword`), `stemmer`
(`none` or `en`), `case_folding` and `accent_folding` (true), `positions` (true), `prefix_index` (0-15), `fields`,
`mutable_limit` (kB, 4096), `build_segments` (0; forces N segments and disables the parallel build), `vacuum_segments` (2; 1 to 128),
`merge_fanin` (8; 2 to 32), `dead_rewrite_pct` (50; 1 to 100), `vacuum_truncate` (true).

`vacuum_segments` is the number of directory entries (the mutable one included) that `VACUUM`, autovacuum and the maintenance worker's
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

`merge_fanin` is the number of segments of one size tier (a tier is a factor of `merge_fanin` in size, starting at `mutable_limit`) that
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
are the worker's promotions of mutable segments, which the writers wait for.

`dead_rewrite_pct` is the share of dead documents above which maintenance rewrites a lone segment to drop them and their terms (a segment
that no merge picks; the dead postings are cleared by `VACUUM` either way). `ALTER INDEX ... SET (dead_rewrite_pct = N)` applies to the next
`VACUUM` or worker step, with no `REINDEX`; 100 never rewrites a segment on its own. The default 50 was measured against 5 on 400,000
documents of 40 words, 10 rounds of updating or deleting 6% of the rows, each followed by `VACUUM` (mean of 5 runs); 5 rewrites the segment
at every round and costs more everywhere except the queries after the deletes:

| load | `dead_rewrite_pct` | `VACUUM` total | WAL | index file | `count(*)` | ranked top-10 |
|---|---|---|---|---|---|---|
| update | 50 (default) | 20.2 s | 1,340 MB | 92 MB | 1.06 ms | 3.9 ms |
| update | 5 | 37.8 s | 1,428 MB | 133 MB | 1.40 ms | 5.0 ms |
| delete | 50 (default) | 2.3 s | 86 MB | 67 MB | 3.5 ms | 4.1 ms |
| delete | 5 | 15.8 s | 455 MB | 89 MB | 0.15 ms | 1.5 ms |

`mutable_limit` (kB, 4096) is the size at which the active mutable segment is sealed for promotion; it is also where the merge tiers start.
`ALTER INDEX ... SET (mutable_limit = N)` applies from the next seal. 1024 was measured against 4096 on
100,000 documents of 40 words (mean of 5 runs): it promotes and merges faster per step and leaves queries on the grown index faster, but it
does that 3.7 times as often and the writers pay for it, so the default stays 4096:

| `mutable_limit` | 16 clients, `INSERT` | `INSERT .. SELECT` | `COPY` | promotion / merge | index, segments | `count(*)` / top-10 OR / top-10 AND |
|---|---|---|---|---|---|---|
| 4096 (default) | 107k rows/s, p99 0.69 ms | 113k rows/s | 261k rows/s | 37 ms / 1.4 s | 72 MB, 17 | 11.5 / 12.5 / 6.1 ms |
| 1024 | 87k rows/s, p99 0.81 ms | 94k rows/s | 137k rows/s | 13 ms / 0.43 s | 98 MB, 78 | 10.6 / 4.1 / 2.6 ms |

`VACUUM` also gives the free blocks at the end of the index file back to the operating system, as it does for a heap (`sift.index_info(index)`:
`relation_blocks` falls to `high_water`). It tries when they are 1000 blocks or a 16th of the index, takes `AccessExclusiveLock` on the
index only if that is free right now, and lets go of it as soon as the file is cut; if any query, insert or maintenance step holds the index it
does nothing, and the next `VACUUM` tries again (it says so at `DEBUG2`; `VACUUM VERBOSE` shows the new size of the index). A hot standby
cancels the queries that hold the index when it replays that lock (or delays replay up to `max_standby_streaming_delay`), as for a heap
(`hot_standby_feedback` does not prevent it; `vacuum_truncate = false` does). A
`VACUUM` that runs with parallel workers (it vacuums several large indexes at once) cannot take the lock, which needs a transaction id, and
leaves it to the next one: `VACUUM (PARALLEL 0)` and autovacuum do truncate. Only space that is free and past the point where an old
snapshot could still read it goes, so the end of the file is what the merges released, once the snapshots older than them are gone.
Freed space in the middle of the file is not moved, it is reused by the next merge or promotion. `ALTER INDEX ... SET (vacuum_truncate = false)`
turns the truncation off for an index (no `REINDEX`), like `vacuum_truncate` of a heap table.

## Column indexes, aggregates and sift_table

Three pieces share the column engine ([docs/COL.md](COL.md)); none needs `shared_preload_libraries`, and `CREATE EXTENSION pg_sift` (3.0) installs all of them.

- **`sift_col` index** over a heap table or a `sift_table`: `CREATE INDEX ON orders USING sift_col (store_id, status, total, (placed_at::date))` keeps those
  columns once more, column by column, with zone maps; filters and aggregates read it instead of the heap rows, and the heap stays authoritative (MVCC
  is the table's). Types: integers, `bool`, dates and timestamps, `float4`/`float8`, `numeric(p, s)` with p up to 18, and `text` up to 255 bytes. Reloptions: `mutable_limit` (kB, 4096, like the
  `sift` index) and `paths` for `jsonb` attributes (`WITH (paths = 'metrics.* float4, items[]{sku text, qty int4}')`, [docs/COL-SHRED.md](COL-SHRED.md)) and `compression` (`auto`, `none`, `lz4[:N]`, `zstd[:N]`; per column `'auto; col=zstd:9'`; needs `REINDEX`).
  Maintenance is the same as for `sift` indexes (background worker, VACUUM); `sift.col_index_info`, `col_segments`, `col_leaves`, `col_verify`, `col_promote` and
  `col_merge` inspect and drive one.
- **Aggregate pushdown:** a `GROUP BY` over a table with a `sift_col` index (or over a lateral unnest of an indexed `jsonb` array) with `count`, `count(DISTINCT)`, `sum`,
  `avg`, `min`, `max`, filters and `IN` lists runs as `Custom Scan (SiftAgg)` when the planner can prove the result equals the plain plan's; anything else keeps
  its plan ([docs/COL-PLAN.md](COL-PLAN.md), [docs/AGG.md](AGG.md)). `SET sift.enable_agg = off` compares the two; `scripts/coldiff.sh` does it for a whole corpus.
- **`sift_table`:** `CREATE TABLE t (...) USING sift_table` is a table access method with native MVCC, btree and `sift` indexes, VACUUM, generic WAL and `pg_dump`
  support ([docs/COL-TAM.md](COL-TAM.md)). New rows go to row-format write pages; when a write segment fills, or VACUUM or autovacuum runs, the settled rows become compressed column segments with the same TIDs (`sift.table_segments('t')` lists them, `sift.table_space('t')` the file and its free blocks; settings: `sift.table_delta_limit`, `sift.table_inline_seal`, `sift.table_compression`, `sift.table_dead_rewrite_pct`, README, Configuration). VACUUM clears deleted rows out of the column segments and rewrites a segment once more than `sift.table_dead_rewrite_pct` percent of it is dead; `VACUUM FULL` and `CLUSTER` write the table anew as column segments. Every type is kept exactly. Its limits are listed once, under [Limits](#limits); publishing one under `wal_level = logical` is under [Replication](#replication).

The chunk codec (encodings plus LZ4 or zstd, [docs/COL-CODEC.md](COL-CODEC.md)) compresses the chunks of a `sift_col` index (option `compression`: `auto`, `none`, `lz4`, `zstd`, or per column `'auto; col=zstd:9'`). It is built into the library; `make SIFT_NO_LZ4=1` or `SIFT_NO_ZSTD=1` build without one.

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
  - `0A000` for backward scans (a `SCROLL` cursor fetching backward; use `FETCH ABSOLUTE` over a `NO SCROLL` cursor), `TABLESAMPLE`, the system columns `xmin`, `xmax`, `cmin` and `cmax`, a DML on a published table without the preload (see [Replication](#replication)), and an aggregate scan that reads a column holding a string over 255 bytes or a float `-0` or non-canonical NaN (`SET sift.enable_agg = off` for that query);
  - `numeric` wider than 18 digits and other types stored as exact images are not read by `SiftAgg`; no HOT, and shared row locks degrade to exclusive;
- One key per index: multicolumn, `INCLUDE` and unique indexes and non-text keys are rejected. No plain or index-only scans.
- Queries: at most 256 leaves, nesting depth 32, phrases of 128 tokens, boosts up to 10000.
- Terms are truncated at 255 bytes, `tf` saturates at 255 and document length at 65535; at most 128 segments per index. A merge reads at most 16 GB of segments: a segment of that size is not merged again (VACUUM still clears its dead postings through the liveness bits).
- Analyzers: English Snowball stemmer only, no stopword list, fixed per index (REINDEX to change).
- `positions = on` is the default and makes the index about 2.6x larger (`positions = off` restores the small index). Phrases are
  rechecked in the heap for documents without positions: indexes built by earlier versions until `REINDEX`, `positions = off`, rows
  inserted since the build, and phrases under `NOT`.
- `sift.score()` cannot be used in `WHERE`; read it from a subquery to filter on it. It is not available with `@@> ANY (array)` (write
  the OR in the query text), nor in `UPDATE`, `DELETE`, `MERGE` and `FOR UPDATE`.
- Under `SERIALIZABLE` a sift scan locks the whole relation (the index access method has no finer predicate locks), so concurrent writers
  can make a serializable transaction fail with `40001`: retry. Ranked scans and statistics see rows that row-level security hides from the
  role ([SIZING.md](SIZING.md#features-and-their-costs)).
- Partitions: `sift.score()` needs a sift index on every partition the query reads (otherwise an error; unranked matching and `count(*)`
  still work).
- Partitions: BM25 statistics are per partition, so scores can differ across partitions.
- Partitions: `sift.max_score()` is not available through a parent (call it on a partition).
- Partitions: the score cannot be used inside an aggregate or a non-nested-loop join; compute it in a subquery with `OFFSET 0` or `LIMIT`.
- Ranked partition scans are never parallel, and neither are scans with a pushed-down filter.
- Facets: text keys only (use an expression index for integers), cost linear in the distinct values, tables with row-level security are
  refused.
- pg_upgrade is tested within one major version only. `pg_sift_test` (built with `SIFT_TESTING=1`) is for the test suite only.
