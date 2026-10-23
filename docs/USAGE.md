# pg_sift usage

Reference for everything beyond the [README](../README.md) quick start. The examples use the `docs` table created there.

## Install

Requires PostgreSQL 17, 18 or 19, server headers with `pg_config` on `PATH`, and a C compiler. Tested on x86-64 (the whole suite), Linux arm64 (PostgreSQL 18, the fast tier of every labeled pull request in CI) and macOS arm64 (the Homebrew build and its test, and a few regression tests, in CI). The chunk codec of the column engine links `liblz4` and `libzstd` when their
development headers are installed (`liblz4-dev`, `libzstd-dev`); `make SIFT_NO_LZ4=1` or `SIFT_NO_ZSTD=1` build without one, and the packages depend on both.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
```

- Arch Linux: AUR `pg_sift` (recipe in [packaging/](../packaging/)).
- Docker, Docker Compose, CloudNativePG: [CONTAINERS.md](CONTAINERS.md) (the image recipes are in [packaging/docker/](../packaging/docker/)); a five-minute tour: [QUICKSTART.md](QUICKSTART.md). Supported versions: [SUPPORT.md](SUPPORT.md).
- Debian 13, Ubuntu 24.04 with PGDG `postgresql-18`: download the `.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest)
  and `sudo apt install ./postgresql-18-pg-sift_3.2.2-1_amd64.deb`.
- RHEL, Rocky, Alma 9 or 10 with PGDG `postgresql18-server`:
  `sudo dnf install https://github.com/brauliobo/pg_sift/releases/download/v3.2.2/pg_sift_18-3.2.2-1.el9.x86_64.rpm`.

Then, as a superuser (after installing a newer version run `ALTER EXTENSION pg_sift UPDATE`): `CREATE EXTENSION pg_sift;`. No `shared_preload_libraries` entry is needed for the `sift` index, the `sift_col` index or the `sift_table` table access method; only publishing a `sift_table` under `wal_level = logical` does ([Replication](#replication)).

A backend loads the library when a statement first opens one of its objects. For a query on a heap table with a `sift_col` index that is while the planner opens the index, after `planner()` had chosen to run without the planner hook that decides the CTE and lateral-join plans. The executor start therefore plans that first statement again through the hook when it is a plain `SELECT` (the parameters of the extended protocol or of `EXECUTE` are planned with their own types and values, as a custom plan is; and it resets the plan cache, so a prepared statement does not keep the first plan), and the statement runs in the scan like the ones after it. Any other first statement (`EXPLAIN`, `CREATE TABLE AS`, a cursor, a query inside a function) keeps the plan it was given, which is the ordinary plan for the aggregates that need the hook. Loading the library at session start also saves the second planning and the few milliseconds of the load: `ALTER DATABASE mydb SET session_preload_libraries = 'pg_sift'` (new sessions only, no restart), or list it in `shared_preload_libraries`.

Since 3.3 the install gives `USAGE` on the schema `sift` to `PUBLIC`, so any role can call `sift.score(ctid)` and the read-only diagnostics without a `GRANT`; each function still checks the table or index it reads, and the tables of the extension stay unreadable. `REVOKE USAGE ON SCHEMA sift FROM PUBLIC` takes it back (then `GRANT USAGE ON SCHEMA sift TO` the roles that need it; the `@@>` operator and the access methods need none). `ALTER EXTENSION pg_sift UPDATE` grants it again.

### Install with Homebrew

```sh
brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git
brew install --HEAD brauliobo/pg_sift/pg_sift
brew test brauliobo/pg_sift/pg_sift
```

The formula in this repository builds for Homebrew PostgreSQL 17 and 18, with LZ4 and zstd, and links extension files through Homebrew's versioned directories.
The HEAD-only formula builds `main` and supports macOS arm64; CI builds it and runs `brew test` on PostgreSQL 17 and 18.
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
It works on materialized views too, in `INSERT ... SELECT`, and in a `HAVING` that reads the score of an aggregate. `UPDATE`, `DELETE`,
`MERGE` and `FOR UPDATE` re-check the rows they read, which a scan cannot do: a subquery, view or `WITH` query of such a statement that
reads the score is planned apart (as with `OFFSET 0` or `MATERIALIZED`) and the statement uses its rows by key (`UPDATE t SET s = q.s FROM
(SELECT id, sift.score(ctid) AS s FROM docs WHERE body @@> 'case') q WHERE t.id = q.id`); a score read straight from a table the statement
joins is an error (planned, with a hint).

### Row-level security

A role under a row-level security policy gets no ranked scan by default: the match is not leakproof and the index statistics
(document count, document frequencies, field lengths, the vocabulary a wildcard expands over) include the rows the policy hides.
`sift.rls_scores = on` (a superuser setting: `ALTER ROLE app SET sift.rls_scores = on`, or `GRANT SET ON PARAMETER` to a role that may
change it) lets the ranked scan serve such a role:

```sql
ALTER TABLE docs ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON docs USING (tenant = current_user);
SET sift.rls_scores = on;       -- as a superuser, or per role with ALTER ROLE ... SET
SET ROLE app;
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'friendly' ORDER BY sift.score(ctid) DESC LIMIT 10;
```

The policy's quals run on every row the ranker offers before any qual of the query (the order PostgreSQL gives a sequential scan, so a
function or an expression of the query that raises for a row is never evaluated on a hidden one), the ranker is asked for more rows until
`k` pass the policy, and no hidden row is ever returned. Policies that call functions or read other tables work. `EXPLAIN` does not
estimate the matches from the index (on a partition or an inheritance child either) and `EXPLAIN ANALYZE` does not print its counters
(`Candidates`, `Superblocks`, `Heap Checks`, ...), as those count the hidden rows; the plan's estimates are the same whatever the hidden
rows hold. `BUFFERS`, `pg_statio_*` and the time still show the pages the scan read, which grow with the hidden rows the ranker offered
before `k` visible ones passed (a sequential scan under a policy shows its hidden rows in `Rows Removed by Filter` the same way). Without a ranking (`WHERE body @@> ...` with
no score), `count(*)`, `sift.facets` and the column index behave as before: a sequential scan, no pushdown. `sift.max_score()` stays an
error (it is the best score of all the matches, hidden ones included). A table owner and a role with `BYPASSRLS` are not under the policy
and score as always; `FORCE ROW LEVEL SECURITY` puts the owner under it.

**The caveat, in full.** The score of a visible row is computed from the statistics of the whole index. It is the score the table owner
sees for the same row, and it depends on the hidden rows: a term that the hidden rows repeat has a lower weight, and a row's score changes
when a hidden row is added. No hidden row is returned and none is counted in a result, but the numbers are information about the corpus,
readable from the scores of rows the role sees (a row of known length and term counts fixes the weight of each term of the query, and so
the number of documents that hold it in the whole index): the number of documents, their average length, how common a term is in all of
them, and whether a wildcard expands past `sift.max_expansions`. A role that can insert documents can probe any word it chooses. Time and
the pages read (`BUFFERS`, `pg_statio_*`) are side channels as for any query. Enable the setting for roles whose
tenants may see that much (a shared corpus with per-user visibility), not for tenants whose documents must be unobservable by each other;
for those, index each tenant's rows separately (a partition per tenant, a sift index on each) or leave the setting off. Because the
setting is read when the query is planned, a prepared statement planned with it on keeps its plan for the session.

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
low-cardinality columns (1.9-4.5x on 26 values); for selective queries or thousands of values `GROUP BY` wins ([DESIGN.md](../DESIGN.md) (l)).
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
`sift.workers` lists every `sift` and `sift_col` index you can read with its directory entries, the write buffers waiting for promotion, whether maintenance is running and, when the last background step failed, `failures` in a row, `failed_at`, `next_retry` (the delay doubles from `sift.maintenance_naptime` up to an hour), the SQLSTATE and the `last_error`; the next successful step clears them. For a `sift_col` index `due` is the answer of `sift.col_maintenance_due`; a worker of a `sift_col` index that fails records its error the same way, and the requests for a new worker wait out the same delay.

`sift.col_index_status` is the one row per `sift_col` index you can read that a DBA of a large table looks at first: the size of the index, of its segments, of the free and reusable blocks, the segments against the 128 the directory holds and the largest one, the rows, the dead rows (`dead_pct`) and the segments VACUUM marked dirty, the rows in the write buffer and the buffers sealed, whether maintenance runs or fails, and what `sift.col_maintenance_status` says (the last VACUUM, the rows changed since, why a rewrite is `due`, what it is `blocked_by`).
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

<!-- sift-settings:start -->
| Setting | Applies to | Default | Values | Context | Description | Cost |
|---|---|---|---|---|---|---|
| `sift.max_expansions` | BM25 | 512 | int 1..1000000 | superuser | Maximum terms a wildcard, regex or fuzzy leaf may expand to, and a prefix may be scored over. A prefix with more terms matches all of them and scores as the one term prefix_index would index. | above it a prefix matches all its terms but scores as one term [USAGE](USAGE.md#queries) |
| `sift.prefix_cache_size` | BM25 | 8192 kB | int 0..1048576 kB | superuser | Memory a backend keeps the postings of broad prefixes in (kB), 0 to keep none. | up to this much memory per backend; 0 keeps none [USAGE](USAGE.md#queries) |
| `sift.enable_scan` | BM25 | on | bool | session | Enables the sift custom scan (ranked and unordered). | none; off gives the plan PostgreSQL has without them [USAGE](USAGE.md#filter-pushdown) |
| `sift.enable_count` | BM25 | on | bool | session | Enables count(*) pushdown into the sift index. | none; off gives the plan PostgreSQL has without them [USAGE](USAGE.md#filter-pushdown) |
| `sift.filter_pushdown` | BM25 | on | enum: off, on, force | session | Ranked scans take the tuples a bitmap of the other quals keeps: off, on (by cost) or force. | none; off gives the plan PostgreSQL has without them [USAGE](USAGE.md#filter-pushdown) |
| `sift.prefetch` | BM25 | on | bool | session | Reads ahead the heap pages an unordered scan is about to visit. | none; off gives the plan PostgreSQL has without them [USAGE](USAGE.md#filter-pushdown) |
| `sift.parallel_scan` | BM25 | off | bool | session | Lets the planner use parallel sift scans and counts (they pay off on large, heap-bound scans only). | off because a Gather costs milliseconds: only an unordered scan with a heavy residual qual gained (1.8x on 218,058 articles) [USAGE](USAGE.md#parallel-scan) |
| `sift.rls_scores` | BM25 | off (new in 3.3) | bool | superuser | Lets a ranked scan serve a role under a row-level security policy. The scores count the rows the policy hides: document frequencies and field lengths come from the whole index. Off, such a role gets no ranked scan and sift.score() raises an error. | scores use the statistics of the whole index, hidden rows included [USAGE](USAGE.md#row-level-security) |
| `sift.standby_conflicts` | all | on (was off in 3.2) | bool | superuser | Logs the reuse of released blocks as a recovery conflict, so a standby without hot_standby_feedback waits for or cancels the queries that may still read them. Off, such a query fails with "segment recycled during scan, retry" when the blocks it reads are reused. | replay can wait up to max_standby_streaming_delay [SIZING](SIZING.md#settings-that-matter) |
| `sift.log_min_duration` | all | 1000 ms | int -1..max ms | superuser | Logs the promotions, merges and maintenance worker runs of an index that take at least this long (ms), -1 to log none. | none; a log line [SIZING](SIZING.md#settings-that-matter) |
| `sift.simd` | all | avx512 | enum: scalar, avx2, avx512, auto | session | Caps the SIMD level used by sift kernels. auto uses the best level the CPU supports; avx2 and avx512 are the levels of x86-64, neon the level of AArch64, and a level of the other architecture caps nothing. | none; a lower level only slows the kernels (auto is the best level of the CPU, shown as avx512 on x86-64 and as neon on AArch64, whose levels are scalar and neon; the level names of the other architecture cap nothing, so one configuration file serves a mixed fleet) [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.async_maintenance` | all | on | bool | superuser | Promotes and merges segments in a dynamic background worker. | a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O) [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.maintenance_naptime` | all | 2000 ms | int 200..60000 ms | reload | Time between background maintenance workers of an index; a worker stays this long after it starts. | a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O) [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.maintenance_cost_delay` | all | 0 | int 0..100 ms | superuser | Milliseconds the maintenance worker sleeps after every 16 pages (128 kB) its merges read or write, 0 to run them at full speed. | a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O) [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.maintenance_wal_rate` | all | 0 (new in 3.3) | int 0..1048576 kB | superuser | Kilobytes of WAL per second the merges of the maintenance worker may write, 0 for no limit. A merge waits before the page that would pass the rate, so that its WAL leaves room for the writes of the queries and inserts and for the standbys to replay it; it adds to sift.maintenance_cost_delay. | a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O) [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.vacuum_rewrite_limit` | sift_col, sift_table | 128 MB | int 0..1048576 MB | session | Megabytes of segments a column VACUUM may rewrite to merge (past the first merge step), with a separate equal allowance to copy the end of the file into free blocks below it, which holds the table or index under an exclusive lock; 0 keeps the one merge step and moves nothing. | the VACUUM that moves the end of the file holds the table or index under an exclusive lock [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.enable_col` | sift_col | on | bool | session | Enables the column scans over sift_col indexes. | none; off gives the plan PostgreSQL has without them [USAGE](USAGE.md#column-indexes-aggregates-and-sift_table) |
| `sift.enable_col_scan` | sift_col | off (new in 3.3) | bool | session | Lets the planner use a sift_col index with the sift.col_*_ops operator classes as a bitmap index scan. Off by default: the bitmap scan returns rows (the column scans of SiftAgg aggregate them), see docs/COL-SCAN.md. | off because the bitmap scan is 40-2000x slower than a btree for point lookups on random, text and uuid keys [COL-SCAN](COL-SCAN.md#how-it-works) |
| `sift.col_rollup_on_demand` | sift_col | on | bool | session | A scan that reads units for lack of their rollups asks the background worker to rebuild them. Nothing is written by the query; the next one answers from the rollups. Off: the units stay as they are until a merge or REINDEX. | background rewrites of segments; off leaves the units as they are until a merge or REINDEX [COL-PREAGG 7](COL-PREAGG.md#7-on-demand-refresh-of-the-rollups-srccolrollupc) |
| `sift.col_rollup_worker` | sift_col | on | bool | session | A request for a rollup refresh starts the background worker that makes it. Off: the requests wait in the queue until sift.col_rollup_refresh(index) is called. | background rewrites of segments; off leaves the units as they are until a merge or REINDEX [COL-PREAGG 7](COL-PREAGG.md#7-on-demand-refresh-of-the-rollups-srccolrollupc) |
| `sift.cluster_advice` | sift_col | on (new in 3.3) | bool | session | sift.cluster_advice() recommends a physical order for the tables of the sift_col indexes. Off: it answers 'not recommended' for every index. The index option cluster_advice switches one index off. | none; maintenance only [USAGE](USAGE.md#is-the-index-clustered-and-what-did-the-scans-do) |
| `sift.col_format_minor` | sift_col, sift_table | 16 (was 6 in 3.2) | int 3..16 | superuser | Column format minor of the segments written (15, the newest 3.3 writes, by default): 6 keeps them readable by pg_sift 3.2, 4 by 3.1, 3 by 3.0. Segments already written keep their minor; a 3.2 library refuses the ones above 6. A segment of a schema with wide leaves is minor 5 at least, and the minor 6 of the complete kinds is written only for a schema with jsonb paths, from rows whose kinds are known. The minor 7 is written only for an index with the option cube_minmax, the minor 8 only for a sift_table with a date or timestamp column, the minor 9 only for an index with the option rollup_dims, the minor 10 only for one with the option key_columns, the minor 12 only for one with the option superunit, the minor 13 only for one with the option order_columns, the minor 14 only for one with a dictionary of 1,024 strings or more (the option dict_lookup), the minor 15 only for one whose key_columns keep a key list or a bitmap. | the options that write a minor above the setting are ignored; a 3.2 library refuses segments of minor 7 and above [FORMATS 4](FORMATS.md#4-column-segments-siftsegentryformat-of-access-methods-sift_col-and-sift_table) |
| `sift.col_auto_merge` | sift_col | on (was off in 3.2) | bool | superuser | Keeps the sift_col indexes answerable after churn: a background worker vacuums the heap table, and after a VACUUM that left an index scattered it seals the write buffer and merges all its segments into one. Rows on a page the visibility map does not call all-visible (any page with an UPDATE or DELETE since the last VACUUM) are checked one by one in the heap, and a unit holding a dead row answers from its rows; the VACUUM sets the map, and the merge after it makes the units answer from their pre-aggregates again. A backend that inserts a row version, or reads many rows in the heap, looks at the statistics of the table once per sift.maintenance_naptime and starts the worker when the rows changed since the last VACUUM are a thousandth of the table (50 rows at least, 100,000 at most). It vacuums as autovacuum does (its reloptions, cost delay, skipped when the table is locked), at most once per five sift.maintenance_naptime (ten times the average VACUUM of the table on PostgreSQL 18), and not a table with autovacuum_enabled = false. The merge rule is sift.col_auto_merge_dirty_pct, _fragmentation and _buffer_pct, and it stays within sift.col_auto_merge_wal_pct of the WAL the server writes; the rewrite sleeps as sift.maintenance_cost_delay says and is skipped when the maintenance lock is held. An index over sift.vacuum_rewrite_limit is rewritten by partitions of that size (sift.col_auto_merge_partition_kb sets another), one step each, so that the lock is let go between them. Needs sift.async_maintenance; sift.col_maintain(index) applies the same rule from a job. A DELETE alone calls nothing and waits for a scan or an insert; a scan of a partitioned table does not ask, an insert into a partition does. Set it for the server or the database: the worker reads no session setting. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.col_auto_merge_dirty_pct` | sift_col | 30 | int 1..100 | superuser | Percent of the units of a sift_col index holding a dead row above which the automatic rewrite runs. Such a unit answers from its rows, not from its pre-aggregates, however few rows are dead. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.col_auto_merge_fragmentation` | sift_col | 2 | real 1.1..1000 | superuser | Units listed per superblock they cover from which the automatic rewrite of a sift_col index runs. Each promotion of the write buffer adds a segment over the superblocks the churn touched. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.col_auto_merge_buffer_pct` | sift_col | 1 | real 0.01..100 | superuser | Percent of its rows the write buffer of a sift_col index holds above which the automatic rewrite runs. The rows of the buffer are read as one in-memory unit per superblock they touch, on every query, until a promotion. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.col_auto_merge_wal_pct` | sift_col | 5 (new in 3.3) | real 0..100 | superuser | Percent of the WAL the server writes that the automatic rewrite of a sift_col index may add. The rewrite writes the whole index, so it waits until the server has written 100 / this percent times the bytes of the index since the index was last written (its build or its last rewrite): 5 bounds the rewrites at a twentieth of the WAL, and the churn that makes one worth it is the churn that wrote that WAL. 0 does not wait. sift.col_maintain(index) ignores it. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.col_auto_merge_max_active` | sift_col | 0 (new in 3.3) | int 0..10000 | superuser | Client backends of the database running a statement above which the automatic rewrite of a sift_col index waits for a quieter moment. The worker looks again after sift.maintenance_naptime, then twice as late each time, 8 times in all; the next VACUUM asks again. 0: never waits. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.col_auto_merge_partition_kb` | sift_col | 0 (new in 3.3) | int 0..max kB | superuser | Size of the segments the automatic rewrite of a sift_col index writes, which then rewrites only the ones the churn touched; 0 rewrites the whole index into one. A step rewrites the partition with most dirty units (over sift.col_auto_merge_dirty_pct of its own), or the overlays the promotions left over the partitions, and holds the maintenance lock and needs the free space of one partition, not of the index. A segment under half of this size is an overlay; the merge tiers leave the partitions alone. An index under half of it is rewritten whole. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.col_auto_merge_max_workers` | sift_col | 0 (new in 3.3) | int 0..64 | superuser | Rewrite workers of sift_col indexes that may run at once in a database, 0 for no limit. A worker above the limit looks again after sift.maintenance_naptime, then twice as late each time, 8 times in all; the next VACUUM asks again. | background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed [USAGE](USAGE.md#maintenance-and-operations) |
| `sift.enable_agg` | sift_col, sift_table | on | bool | session | Enables aggregate pushdown into sift_col column indexes (Custom Scan (SiftAgg)). | none; off gives the plan PostgreSQL has without sift [USAGE](USAGE.md#column-indexes-aggregates-and-sift_table) |
| `sift.agg_left_lateral` | sift_col, sift_table | on (new in 3.3) | bool | session | Pushes down LEFT JOIN LATERAL over a jsonb_to_recordset or jsonb_array_elements list of the scanned row: a row none of whose elements passes is one tuple with NULL element columns. | none; off keeps the normal plan of such a query (the way to compare it and to isolate a wrong result) [AGG](AGG.md) |
| `sift.agg_preagg` | sift_col, sift_table | on | bool | session | Answers the units of an aggregate scan whose rows are all visible from the pre-aggregates the sift_col index stored with them. | none on the index; off scans every unit [COL-PREAGG 4](COL-PREAGG.md#4-when-a-units-pre-aggregate-may-answer) |
| `sift.agg_preagg_distinct` | sift_col, sift_table | on (new in 3.3) | bool | session | Answers count(DISTINCT col) of a column the cube stores a histogram of from the groups of the histograms of the units whose rows are all visible, with sift.agg_preagg. | none on the index; off scans every unit [COL-PREAGG 4](COL-PREAGG.md#4-when-a-units-pre-aggregate-may-answer) |
| `sift.agg_super` | sift_col, sift_table | on (new in 3.3) | bool | session | Answers a group of exact units from the summary the sift_col index stored for the group (reloption superunit) instead of from each unit. | none on the index; off scans every unit [COL-PREAGG 10](COL-PREAGG.md#10-superunits-one-summary-for-a-group-of-units-column-format-minor-12-reloption-superunit-default-16) |
| `sift.enable_topn` | sift_col, sift_table | on | bool | session | Enables ORDER BY .. LIMIT from the column units (Custom Scan (SiftAgg) with Top-N), with sift.enable_agg. | none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns [COL-SEARCH](COL-SEARCH.md#facets-and-top-n) |
| `sift.topn_walk` | sift_col, sift_table | on | bool | session | Lets a top-N scan visit the units of a sort column that does not follow the table by the best value of their zones, and stop at the first that cannot beat the n-th. | none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns [COL-SEARCH](COL-SEARCH.md#facets-and-top-n) |
| `sift.topn_zone_keys` | sift_col, sift_table | on (new in 3.3) | bool | session | Lets the walk of a top-N bound the units by the zones of the sort keys after the first too: a first key that ties between units is decided by the next. | none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns [COL-SEARCH](COL-SEARCH.md#facets-and-top-n) |
| `sift.topn_runs` | sift_col, sift_table | on (new in 3.3) | bool | session | Lets a top-N scan read a unit through the sorted run of its first sort column (an index with the option order_columns) instead of testing every row of the unit. | none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns [COL 2.14](COL.md#214-sorted-run-of-a-leaf-in-a-unit-column-format-minor-13-33-opt-in) |
| `sift.agg_lookup_pushdown` | sift_col, sift_table | on | bool | session | Loads the rows a lookup join can match only: an index scan or scan keys on the table's own `column = value` conditions, and the other own conditions tested while loading. | memory: up to 256 MB a backend, and shared memory of 5% of shared_buffers by default; the memory limits stop a larger table from being loaded [SET-FILTERS 4](SET-FILTERS.md#4-the-lookup-table-loads-only-the-rows-it-can-match-siftagg_lookup_pushdown) |
| `sift.agg_lookup_cache_kb` | sift_col, sift_table | 262144 kB | int 0..max kB | session | Memory per backend that keeps the rows of the joined tables a scan loaded for the next query, while the table does not change (0: off). | up to 256 MB a backend while the table is unchanged and fully vacuumed; a table that needs over half of it is loaded per query [AGG](AGG.md#join-key-filters-and-shared-lookups) |
| `sift.agg_lookup_shared_kb` | sift_col, sift_table | -1 (new in 3.3) | int -1..max kB | superuser | Shared memory, for all backends together, that keeps the rows of the joined tables a scan loaded for the queries of other backends, while the table does not change (0: off, -1: the smaller of sift.agg_lookup_cache_kb and a twentieth of shared_buffers). Dynamic shared memory, taken as entries are kept and returned as they go, so it needs no restart; with posix shared memory it comes from /dev/shm, which it must stay below. Backends that run the same query at once wait for the one that loads the rows (30 s at most); a segment the system has no room for gives a notice and the backend loads its own tables. | dynamic shared memory taken as entries are kept (by default the smaller of the per-backend cache and 5% of shared_buffers); with posix shared memory keep it below what /dev/shm holds [AGG](AGG.md#join-key-filters-and-shared-lookups) |
| `sift.agg_lookup_max_kb` | sift_col, sift_table | 262144 kB (new in 3.3) | int 0..max kB | session | Memory of the rows a sub-select, VALUES list, function or spine joined to Custom Scan (SiftAgg) may return, as the planner estimates them: the rows go into a lookup table in memory when the scan starts; a larger one keeps the normal join. | a larger sub-select, VALUES list or function keeps the normal join [AGG](AGG.md#joins-to-plans) |
| `sift.agg_lookup_max_rows` | sift_col, sift_table | -1 (was 1000000 in 3.2) | int -1..max | session | The older form of sift.agg_lookup_max_kb: rows a sub-select, VALUES list or function joined to Custom Scan (SiftAgg) may return, whatever their width (-1: the memory limit decides). | the older form of agg_lookup_max_kb; -1 lets the memory decide [AGG](AGG.md#joins-to-plans) |
| `sift.agg_share_lookups` | sift_col, sift_table | on | bool | session | Lets the Custom Scan (SiftAgg) nodes of one statement that join the same table share the rows they load (UNION ALL arms). | memory: up to 256 MB a backend, and shared memory of 5% of shared_buffers by default; the memory limits stop a larger table from being loaded [AGG](AGG.md#join-key-filters-and-shared-lookups) |
| `sift.agg_semi_lut` | sift_col, sift_table | on | bool | session | Answers a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer leaf from a byte table over the keys of the joined table. | memory of that table, 4096 kB by default per scan and per worker; keys that span more keep the generic join [SET-FILTERS 3](SET-FILTERS.md#3-the-set-as-a-table-in-select--exists-not-exists-siftagg_semi_lut) |
| `sift.agg_semi_lut_size` | sift_col, sift_table | 4096 kB | int 0..1048576 kB | session | Memory of the byte table of a semi or anti join, per scan and per worker; keys that span more keep the generic join. | memory of that table, 4096 kB by default per scan and per worker; keys that span more keep the generic join [SET-FILTERS 3](SET-FILTERS.md#3-the-set-as-a-table-in-select--exists-not-exists-siftagg_semi_lut) |
| `sift.agg_param_set_size` | sift_col, sift_table | 1000 | int 0..100000000 | session | Elements assumed for an array parameter of a set filter (col = ANY($1)) in a generic plan, to cost the index and bitmap scans against Custom Scan (SiftAgg); 0 keeps the planner's own guess. | none; planning only (0 keeps the planner's guess of 10) [SET-FILTERS 2](SET-FILTERS.md#2-the-set-as-one-array-parameter-col--any1int4) |
| `sift.agg_key_skew` | sift_col, sift_table | on | bool | session | Prices the probes of the normal plan for the keys of a lookup join by the rows a key owns by the column's most common values, not by the mean rows of a key. | none; planning only [SET-FILTERS 4](SET-FILTERS.md#4-the-lookup-table-loads-only-the-rows-it-can-match-siftagg_lookup_pushdown) |
| `sift.agg_rollup_cost` | sift_col, sift_table | 1 | real 0..1000 | session | Scale of the cost of a tuple of a unit's rollup against the scan of an element: a unit is answered from its rollup only when that costs less (0: always). | none; planning only (0 always, large never) [COL-PREAGG 6](COL-PREAGG.md#6-consumption-in-siftagg-built) |
| `sift.agg_dense` | sift_col, sift_table | on | bool | session | Aggregates the chunks of counts, sums and averages from the bitmap of their rows, without a list of tuples. | none on the index; off gives the generic path with the same rows [COL-PLAN](COL-PLAN.md#executor) |
| `sift.agg_two_level` | sift_col, sift_table | on | bool | session | Aggregates an aggregate over per-row aggregates (UNION ALL arms grouped by a row id) in one scan of the index: the cells of a row are kept in the scan. | none on the index; off gives the generic path with the same rows [COL-PLAN](COL-PLAN.md#executor) |
| `sift.agg_dist_stream` | sift_col, sift_table | on (new in 3.3) | bool | session | Visits the superblocks of a count(DISTINCT) of an integer column in the order of the zone minimum of the column when that keeps its sets small: the values below the minimum of the superblocks to come are dropped. | none on the index; off gives the generic path with the same rows [AGG](AGG.md#counting-distinct-values-over-the-rows-of-a-scan) |
| `sift.agg_fuse` | sift_col, sift_table | on | bool | session | Runs the UNION ALL arms that read the same rows in one pass over the index, as the grouping sets of one scan. | none on the index; off gives the generic path with the same rows [COL-PLAN](COL-PLAN.md#one-pass-for-the-arms) |
| `sift.agg_join_filter` | sift_col, sift_table | on | bool | session | Tests the integer key leaf of an inner or semi lookup join against the keys of the loaded table before tuples are made (zone pruning, source filters, row bitmaps). | none on the index; off gives the generic path with the same rows [AGG](AGG.md#join-key-filters-and-shared-lookups) |
| `sift.agg_rows` | sift_col, sift_table | on | bool | session | Groups the rows of a plan that reads Custom Scan (SiftAgg) nodes with exact count(DISTINCT) in Custom Scan (SiftAggRows), without a sort of the rows. | none on the index; off gives the generic path with the same rows [AGG](AGG.md#counting-distinct-values-over-the-rows-of-a-scan) |
| `sift.agg_combo_cache` | sift_col, sift_table | 1048576 | int 16..16777216 | session | Distinct value combinations of an expression PostgreSQL evaluates for the aggregate scan kept before the cache starts again. | memory grows with the combinations kept before the cache starts again [COL-PLAN](COL-PLAN.md#expressions-postgresql-evaluates) |
| `sift.explain_refusals` | sift_col, sift_table | off (new in 3.3) | bool | session | Reports as a notice why a query kept the normal plan instead of Custom Scan (SiftAgg): the first reason each candidate path gave up for. | none when off; the plans are the same on or off [USAGE](USAGE.md#why-a-query-kept-the-normal-plan) |
| `sift.count_refusals` | sift_col, sift_table | off (new in 3.3) | bool | session | Counts in sift.stats() why queries kept the normal plan (refused.<slug>, limit.<id>), without the notice of sift.explain_refusals. | a few microseconds a query that reaches a candidate path [USAGE](USAGE.md#why-a-query-kept-the-normal-plan) |
| `sift.gpu` | sift_col, sift_table | off | enum: off, auto, on | superuser | Runs the integer aggregates of a SiftAgg scan on a Vulkan device (docs/GPU.md): off, auto (above sift.gpu_min_rows) or on. A build without Vulkan support (make VULKAN=1) runs on the CPU whatever this says. | needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says [GPU](GPU.md#settings) |
| `sift.gpu_min_rows` | sift_col, sift_table | 5000000 | int 0..max | session | Rows of the index above which sift.gpu = auto uses the device for a grouped aggregate; an ungrouped one runs there only with sift.gpu = on. | needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says [GPU](GPU.md#settings) |
| `sift.gpu_device` | sift_col, sift_table | '' | device index or part of its name; empty: the best | superuser | The Vulkan device sift.gpu uses: its index, or a part of its name. Empty: the best one. | needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says [GPU](GPU.md#settings) |
| `sift.gpu_cache_mb` | sift_col, sift_table | 256 MB | int 16..1048576 MB | superuser | Device memory, in MB, for the column chunks that stay on the Vulkan device between queries (per backend). | device memory per backend for chunks that stay on the device [GPU](GPU.md#settings) |
| `sift.table_write_buffer_limit` | sift_table | 4096 kB | int 2048..8192 kB | session | Size of a sift_table write buffer (row format) before it is closed, to be promoted into columns. Rounded down to whole superblocks of 2 MB; a row has to fit one segment. | rounded down to 2 MB; a row must fit one unless table_wide_rows is on [COL-TAM 2.3](COL-TAM.md#23-write-buffer-row-format-the-write-buffer) |
| `sift.table_wide_rows` | sift_table | on (new in 3.3) | bool | session | Accepts a row of a sift_table that does not fit one write buffer (up to PostgreSQL's 1 GB), in pages beyond the buffer's table (format 11). Off: such a row is refused. On (the default): the write buffer that takes it is marked format 11, which a build that does not know it cannot write to; rows that fit keep the layout of earlier versions. | none for rows that fit; the buffer that takes a wide row is directory format 11, which a build that does not know it cannot write to [COL-TAM 2.3](COL-TAM.md#23-write-buffer-row-format-the-write-buffer) |
| `sift.table_inline_promote` | sift_table | on | bool | session | Promotes the closed write buffers of a sift_table into columns while rows are inserted (VACUUM always does). | off leaves the promotion to VACUUM and autovacuum [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_dict_lookup` | sift_table | off (new in 3.3) | bool | session | Stores the order of the strings too in the dictionary of 1,024 strings or more of the column segments written from now on, so that a test for a string reads the dictionary in place (column format minor 14); the order is 4 bytes a string. | off by default: the order is +24% to +63% of a sift_table of a high-distinct text column and gains the first statement of a backend only; a row fetch reads the strings in place without it [COL 2.15](COL.md#215-dictionary-stored-in-blocks-read-in-place-column-format-minor-14-33) |
| `sift.table_async_promote` | sift_table | off (new in 3.3) | bool | session | A backend that closes a write buffer of a sift_table asks a background worker to promote it, where it promotes it itself. Needs sift.async_maintenance and sift.table_inline_promote. From half of the directory on the writer promotes and merges itself. | off by default: 4 clients of single-row INSERT, tps and p99 equal within noise [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_bulk_groups` | sift_table | on (new in 3.3) | bool | session | COPY, CREATE TABLE AS and REFRESH MATERIALIZED VIEW write the rows of a sift_table straight into column groups, with no write buffer to promote. On by default. Only a table without a unique or exclusion index, a trigger or a foreign key, and not published; other tables keep the write buffers. | a table with a unique or exclusion index, a trigger or foreign key, or a published table keeps the write buffers [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_backpressure` | sift_table | 0 (new in 3.3) | int 0..99 | session | Percent of the directory of a sift_table from which a writer that needs a new write buffer first waits for the maintenance lock and merges. 0 (off): a writer waits only when the directory is nearly full. Above 50, the percent of the entries. | the wait; 0 waits only when the directory is nearly full [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_compression` | sift_table | '' | auto, none, lz4[:N], zstd[:N], encodings; per column 'auto; col=zstd:9' | session | Compression of the column segments a sift_table writes: auto, none, lz4, zstd or encodings; per column: 'auto; col=zstd:9'. | the sift_col trade-off: zstd -4.0% size, build +30%, scans slower to decode [COL-TAM 8](COL-TAM.md#8-compression-options-defaults) |
| `sift.table_dead_rewrite_pct` | sift_table | 30 | int 1..100 | session | Percent of dead rows above which VACUUM rewrites a column segment of a sift_table alone to drop them. 100 never rewrites. | 100 never rewrites [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag` | sift_table | on (was off in 3.2) | bool | session | A backend that closes a write buffer of a sift_table, or deletes, updates or reads it, asks a background worker to rewrite its mostly dead column segments and give back its free space when it is worth it, without VACUUM. Needs sift.async_maintenance. With sift.table_auto_defrag_indexes the worker runs a whole VACUUM when the dead rows reach sift.table_auto_defrag_dead_pct; without it the worker clears no index entry and VACUUM still has to run for those. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_indexes` | sift_table | on | bool | session | The background pass of a sift_table whose dead rows reach sift.table_dead_rewrite_pct is a whole VACUUM, index cleanup and tombstone rewrite included. Off: it only clears the dead rows of the column segments and returns space, and the tombstone log and the index entries wait for a VACUUM. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_free_pct` | sift_table | 25 | int 1..100 | session | Percent of the file that is free blocks from which the background pass of a sift_table gathers and returns them. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_delay` | sift_table | 10 ms | int 0..100 ms | session | Cost delay of the background pass of a sift_table, in milliseconds: it sleeps this long (at most four times) each time its page accesses add up to vacuum_cost_limit, and after each segment it rewrites. 0 never waits; at most 100, the most vacuum_cost_delay allows. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_dead_pct` | sift_table | 5 (new in 3.3) | int 1..100 | session | Percent of the rows of a sift_table that are tombstones from which the background pass is asked for. The pass rewrites the segments that are more than sift.table_dead_rewrite_pct percent dead; 100 never asks for dead rows. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_max_active` | sift_table | 0 (new in 3.3) | int 0..10000 | session | Client backends of the database running a statement above which the background pass of a sift_table waits for a quieter moment. The worker looks again after sift.maintenance_naptime, then twice as late each time, 8 times in all, and between the merges of a pass; a later request asks again. 0: never waits. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_wal_rate` | sift_table | 0 (new in 3.3) | int 0..max kB | session | Kilobytes of WAL per second the background pass of a sift_table may write on average. After a step that put it ahead of that rate the worker sleeps until the rate is met. A rewritten segment makes every backend that reads it load its dictionaries again, so the rate also bounds how often that happens. 0: no limit. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_coalesce` | sift_table | on (new in 3.3) | bool | session | The background pass of a sift_table merges the small column groups next to the ones it rewrites into one group. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.table_auto_defrag_budget` | sift_table | 128 MB (new in 3.3) | int 0..1048576 MB | session | Megabytes of column segments one background pass of a sift_table rewrites by merging at most. The worker passes again while the budget stopped the last one. 0 is no limit. | background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh) [COL-TAM 7](COL-TAM.md#7-maintenance) |
| `sift.plan_scale` | sift_col, sift_table | 1 (new in 3.3) | real 1..1000000 | session | Plans as if the tables with a sift_col index had this many times the pages and rows (1: as they are). The tables without one, and the statistics of the columns, stay as they are. For the planner's choice at a larger size: compare the plans of EXPLAIN and sift.explain_why at 10 and 100 without the data. PostgreSQL up to 18. | none at 1; a diagnostic [USAGE](USAGE.md#column-indexes-aggregates-and-sift_table) |
| `sift.agg_scale_horizon` | sift_col, sift_table | 100 (new in 3.3) | real 1..1000000 | session | Size, as a multiple of the table, up to which an inline shared CTE that is a close call must stay the better plan (1: only the size it is). Used where the scans of the inline plan cost less than the work of the shared CTE but not by the margin: the plans are made again at the square root of the horizon and at the horizon times the rows (sift.plan_scale), and the inline one is kept if it wins at both. | none at 1; two more plans of the statement where the scans cost less than the shared work but not by the margin [USAGE](USAGE.md#column-indexes-aggregates-and-sift_table) |
<!-- sift-settings:end -->

Index options go in `WITH (...)` at `CREATE INDEX`; `write_buffer_limit`, `vacuum_segments`, `merge_fanin`, `dead_rewrite_pct` and `vacuum_truncate` can be changed afterwards (`ALTER INDEX ... SET (...)`), the rest need `REINDEX`.

<!-- sift-reloptions:start -->
| Index option | Index | Default | Values | Description | Cost |
|---|---|---|---|---|---|
| `k1` | `sift` | 1.2 | real 0..1000 | BM25 term frequency saturation | REINDEX to change; no size effect [SIZING](SIZING.md#features-and-their-costs) |
| `b` | `sift` | 0.75 | real 0..1 | BM25 document length normalization | REINDEX to change; no size effect [SIZING](SIZING.md#features-and-their-costs) |
| `analyzer` | `sift` | text | enum: text, keyword | Analyzer: text, or keyword to index each whole value as one term (facets, REINDEX to apply) | REINDEX to change [FIELDS-FACETS](FIELDS-FACETS.md#facets-siftfacet_counts) |
| `stemmer` | `sift` | '' | none or en | Stemmer: en (Snowball English) or none | REINDEX to change [SIZING](SIZING.md#features-and-their-costs) |
| `case_folding` | `sift` | true | bool | Fold case | REINDEX to change [SIZING](SIZING.md#features-and-their-costs) |
| `accent_folding` | `sift` | true | bool | Strip accents | REINDEX to change [SIZING](SIZING.md#features-and-their-costs) |
| `positions` | `sift` | true | bool | Store word positions, so phrase queries are decided by the index instead of rechecked in the heap (REINDEX to apply) | index about 2.5x the size of positions=false in the 3.0 series (130.9 against 52.5 MiB on 218,058 articles); REINDEX to change [SIZING](SIZING.md#disk) |
| `prefix_index` | `sift` | 0 | int 0..15 | Index the first 1..n characters of every word, so prefix queries of up to n characters need no expansion (0 = off, REINDEX to apply) | a larger index (more terms, growing with n); REINDEX to change [USAGE](USAGE.md#queries) |
| `fields` | `sift` | '' | names of the leading sections of the text, as name:word | Names of the first sections of the indexed text (split at chr(29)) that queries can search apart, as name:word | REINDEX to change [FIELDS-FACETS](FIELDS-FACETS.md#fields-siftsections-and-siftfield_query) |
| `write_buffer_limit` | `sift` | 4096 | int 64..1048576 | Write buffer size in kB before it is sealed for promotion | 1024 against 4096 kB (100,000 documents): promotion 37 to 13 ms but 3.7x as often, 16-client INSERT 107k to 87k rows/s, index 72 to 98 MB [USAGE](USAGE.md#maintenance-and-operations) |
| `vacuum_segments` | `sift` | 2 | int 1..128 | Directory entries (segments, the write buffer included) VACUUM merges the index down to; more keep VACUUM short and queries slower | 2 against 16 (60 segments of 50,000 documents): VACUUM 0.19 s against 0.10 s, count(*) 0.08 against 0.35 ms [USAGE](USAGE.md#maintenance-and-operations) |
| `merge_fanin` | `sift` | 8 | int 2..32 | Segments of a size tier that trigger a merge; a lower value merges more often and keeps fewer segments, a higher one merges less | lower merges more often (more write work), higher keeps up to merge_fanin - 1 segments a tier [USAGE](USAGE.md#maintenance-and-operations) |
| `build_segments` | `sift` | 0 | int 0..64 | Segments written by CREATE INDEX, 0 for one per build participant | a serial build [USAGE](USAGE.md#maintenance-and-operations) |
| `dead_rewrite_pct` | `sift` | 50 | int 1..100 | Percent of dead documents above which maintenance rewrites a lone segment to drop them (100 never) | 5 against 50 (400,000 documents, 10 update rounds): VACUUM 37.8 s against 20.2 s, index 133 against 92 MB; 100 never rewrites [USAGE](USAGE.md#maintenance-and-operations) |
| `vacuum_truncate` | `sift` | true | bool | VACUUM gives the free blocks at the end of the index file back to the operating system (takes a brief exclusive lock when it can) | a brief exclusive lock when it is free; a standby cancels queries that hold the index [USAGE](USAGE.md#maintenance-and-operations) |
| `write_buffer_limit` | `sift_col` | 4096 | int 64..1048576 | Write buffer size in kB before it is sealed for promotion | the trade-off measured for the sift index (1024 against 4096 kB: 3.7x as many promotions); ALTER INDEX applies from the next seal [USAGE](USAGE.md#maintenance-and-operations) |
| `dead_rewrite_pct` | `sift_col` | 10 | int 1..100 | Percent of dead rows above which maintenance rewrites a lone segment to drop them (100 never) | a rewrite writes the whole segment; 100 never rewrites [USAGE](USAGE.md#maintenance-and-operations) |
| `vacuum_truncate` | `sift_col` | true | bool | VACUUM gives the free blocks at the end of the index file back to the operating system | a brief exclusive lock when it is free; a standby cancels queries that hold the index [USAGE](USAGE.md#maintenance-and-operations) |
| `paths` | `sift_col` | '' | attname: a.b type, a.b[]{f type}, *:type; ... | jsonb paths shredded into columns, by index attribute name | the declared leaves are stored once more; REINDEX to change [COL-SHRED](COL-SHRED.md#declaring-paths-reloption-paths) |
| `compression` | `sift_col` | '' | auto, none, lz4[:N], zstd[:N], encodings; per column 'auto; col=zstd:9' | chunk compression: auto, none, lz4, zstd, encodings; per column: 'auto; col=zstd:9' | zstd builds 30% slower (13 columns, 200,000 rows); zstd decodes 3-10x slower on scanned streams; REINDEX to apply [COMPRESSION 5.3](perf/COMPRESSION.md#53-per-column-control-the-compression-option) |
| `preagg` | `sift_col` | '' | default, off, or cube(dim: measure, ...; ...) | pre-aggregates of every unit: default, off, or cube(dim: measure, ...; dim2: ...) | the default takes at most 2% of the encoded bytes of a unit (at least 2 kB); off keeps leaf sums only; applies to segments written afterwards [COL-PREAGG 2](COL-PREAGG.md#2-what-is-precomputed-written-when-a-unit-is-built-merged-promoted-rewritten-by-vacuums-merge-and-by-every-worker-of-a-parallel-build) |
| `rollup` | `sift_col` | '' | leaf, leaf, ... or off | the leaves of array elements and map values that the rollup of every unit groups by: 'leaf, leaf, ...', or off | explicit names hold up to 4096 tuples a unit with no byte budget; the default rule has a budget of 2% of the unit per list [COL-PREAGG 2](COL-PREAGG.md#2-what-is-precomputed-written-when-a-unit-is-built-merged-promoted-rewritten-by-vacuums-merge-and-by-every-worker-of-a-parallel-build) |
| `rollup_on_demand` | `sift_col` | true | bool | A scan that reads units for lack of their rollups asks the background worker to rebuild them (default on) | a background rewrite of the segment; the query itself writes nothing [COL-PREAGG 7](COL-PREAGG.md#7-on-demand-refresh-of-the-rollups-srccolrollupc) |
| `cluster_advice` | `sift_col` | true (new in 3.3) | bool | sift.cluster_advice() may recommend a physical order for the table of this index (default on; maintenance only, not part of the format) | none; maintenance only, not part of the format [USAGE](USAGE.md#is-the-index-clustered-and-what-did-the-scans-do) |
| `dict_lookup` | `sift_col` | auto (new in 3.3) | enum: auto, on, true, off, false | A dictionary of 1,024 strings or more of the segments written from now on is stored in blocks that are read in place, without loading it whole, where that adds at most 3% to the segment (ids, hashes, uuids and long strings; short strings that repeat words stay in the compact form); the option adds the order of the strings, so that an equality, an IN list or a prefix test reads it in place too: auto (default) for the columns of point_columns, key_columns and return_columns, on for every column, off for none (the segments are column format minor 14; the order is 4 bytes a string) | format minor 14; a 3.2 library cannot read it; the order is 4 bytes a string: +10% to +70% of an index of one high-distinct text or uuid column, none for a column that does not ask [COL 2.15](COL.md#215-dictionary-stored-in-blocks-read-in-place-column-format-minor-14-33) |
| `cube_minmax` | `sift_col` | true (new in 3.3) | bool | The cube of every unit written from now on also holds the minimum and maximum of each integer measure per group; the segments are column format minor 7 (default on) | format minor 7; a 3.2 library cannot read it; +0.17% of the index alone, about +1.5% with superunit 16; insert, promotion and merge CPU inside the noise [COL-PREAGG 2](COL-PREAGG.md#2-what-is-precomputed-written-when-a-unit-is-built-merged-promoted-rewritten-by-vacuums-merge-and-by-every-worker-of-a-parallel-build) |
| `rollup_dims` | `sift_col` | '' (new in 3.3) | col, col or col, col: measure, ... | the scalar columns that the rollup of the rows of every unit written from now on groups by: 'col, col' or 'col, col: measure, ...' (default off; the segments are column format minor 8) | format minor 9; a 3.2 library cannot read it; +5.3% of the index, build +25% (two columns) [COL-PREAGG 2](COL-PREAGG.md#2-what-is-precomputed-written-when-a-unit-is-built-merged-promoted-rewritten-by-vacuums-merge-and-by-every-worker-of-a-parallel-build) |
| `calendar_columns` | `sift_col` | '' (new in 3.3) | col, col, all or off; empty: every date, timestamp and timestamptz column | the date, timestamp and timestamptz columns whose calendar cells (days, else months, else years) every unit written from now on keeps, so that a GROUP BY of date_trunc, extract or a cast of them, and a range, are answered from the cells; 'col, col', 'all' or 'off' (default all, unless the index has JSON paths; the cube holds at most 16 dimensions with the default dims, a list that does not fit is refused and 'all' keeps the columns that fit; the segments are column format minor 8) | format minor 8; a 3.2 library cannot read it; +1.0% of the index for one column, +1.8% for two (superunits no longer merge the cells); not for an index with JSON paths; the cube holds at most 16 dimensions and the calendar cells come first: the default dims give way (the dates' own histograms, then the last columns; a NOTICE names them), a cube(...) of the user does not, so a list that does not fit is refused, `all` keeps the columns that fit and the build names the ones it left out [COL-PREAGG 2](COL-PREAGG.md#2-what-is-precomputed-written-when-a-unit-is-built-merged-promoted-rewritten-by-vacuums-merge-and-by-every-worker-of-a-parallel-build) |
| `key_columns` | `sift_col` | '' (new in 3.3) | auto (the default), off, or a list of columns: int2, int4 and date for a key list or bitmap, also integer, text and uuid for a filter | the columns whose keys every unit written from now on keeps, as a key list or a bitmap where that takes a few percent of the unit (a count(DISTINCT column) over whole units reads no row, an equality or IN test skips the units that lack the key) and, for a column named here, as a bloom filter where it does not; 'col, col', 'off' or 'auto' (the int2, int4 and date foreign key columns and the columns named *_id that are not unique and that the cube does not hold; chosen by CREATE INDEX and REINDEX when the option is not set); the segments are column format minor 10, and 15 for a key list or bitmap (default auto) | format minor 15; a 3.2 library cannot read it; auto keeps a key list or bitmap of the int2, int4 and date foreign keys and *_id columns that fit 3% of a unit: +1.1% to +1.4% of the index on the copies; a filter named in a list: +5.8% to +10.8% of the index on random keys, no gain on them, build +30% (three columns) [COL 2.13](COL.md#213-keys-of-a-leaf-in-a-unit-column-format-minors-10-and-15-33-on-for-foreign-keys) |
| `point_columns` | `sift_col` | '' (new in 3.3) | integer, text or uuid columns (int2, int4, int8, date, timestamp, timestamptz, numeric up to 18 digits and bool are integer leaves; float is not) | the plain integer, text and uuid columns whose rows every segment written from now on lists by value (a point directory that lets an equality or a short IN list of the access method find its rows with a probe, and the heap check them); the segments are column format minor 14 (default none) | format minor 14; a 3.2 library cannot read it; 4 bytes a row for keys that repeat (0.8 MB for 200,000 rows), 4.2 MB for a unique key; a leaf with overflow values or inline strings gets no table [COL 2.17](COL.md#217-point-directory-of-a-leaf-in-a-segment-column-format-minor-14-33-opt-in) |
| `point_tid_columns` | `sift_col` | '' (new in 3.3) | integer, text or uuid columns (the leaves of point_columns) | the plain integer, text and uuid columns whose point directory lists the heap addresses of the rows, not their numbers in the segment (an equality or a short IN list of the access method then returns the rows' addresses from the directory without reading a unit, and the heap checks them; a unit with a dead row is read instead); the segments are column format minor 16, which only 3.3 and later read, and the lists take 1 to 5 bytes per row (default none) | format minor 16; a 3.2 library cannot read it; the size of point_columns within 3% (a key of many rows takes 1 to 3 bytes a row, not 4); the build 65% longer, a promotion 5% longer, inserts unchanged; not for an index-only scan, which reads the units the addresses name [COL 2.17](COL.md#217-point-directory-of-a-leaf-in-a-segment-column-format-minor-14-33-opt-in) |
| `order_columns` | `sift_col` | '' (new in 3.3) | integer, date, timestamp, numeric or float columns | the integer, date, timestamp, numeric and float columns whose rows every unit written from now on keeps in the order of the column (a sorted run that lets ORDER BY .. LIMIT read the first rows of each unit only); the segments are column format minor 13 (default none) | format minor 13; a 3.2 library cannot read it; the row ordinals per column: 1 B a row (units up to 256 rows), 2 B (to 65,536), else 4 B; 16 B a unit for a column loaded in order [COL 2.14](COL.md#214-sorted-run-of-a-leaf-in-a-unit-column-format-minor-13-33-opt-in) |
| `return_columns` | `sift_col` | '' (new in 3.3) | text, varchar or uuid columns | the text, varchar and uuid columns an index-only scan returns from the index (their strings are read from the dictionary of the segment; default none: a query that needs one reads the heap) | no extra storage: the strings are the dictionary the index keeps [COL-SCAN](COL-SCAN.md#how-it-works) |
| `superunit` | `sift_col` | 16 (new in 3.3) | int 0..256 | Units per superunit summary: the sums and the cube of every group of this many consecutive units of a segment are stored once more, merged, and answer a query that no predicate cuts through from one read; the segments written from now on are column format minor 12 (0: none, else 2 to 256; default 16) | format minor 12; a 3.2 library cannot read it; +1.0% of the index (groups of 64: +0.8%, of 4 to 8: +1.3% to +1.5%); a range query on a 2-unit table 0.10 to 0.13-0.15 ms [COL-PREAGG 10](COL-PREAGG.md#10-superunits-one-summary-for-a-group-of-units-column-format-minor-12-reloption-superunit-default-16) |
<!-- sift-reloptions:end -->

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
With `sift.col_auto_merge_partition_kb` the rewrite is by range instead: the index is kept as segments of that size, a step rewrites the one the churn left most dirty (or merges the small segments over them), and the maintenance lock is held, and free space needed, for one step; where the churn is local (a retention window, a hot tail) the partitions it did not touch are not written at all. `sift.maintenance_wal_rate` and `sift.col_auto_merge_max_workers` bound what the workers take from the WAL and from the other sessions.

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
immediately and tail segments can be relocated into interior free runs before truncation, all in the same `VACUUM`. See [COL §2.7](COL.md#27-liveness-and-vacuum)
for the write allowance and conditions that defer relocation. Free pages left inside the file remain reusable by later writes. `ALTER INDEX ... SET (vacuum_truncate = false)`
turns the truncation off for an index (no `REINDEX`), like `vacuum_truncate` of a heap table.

**Merging is automatic.** Sealed segments are merged `merge_fanin` (8) at a time per size tier, the directory (128 entries) is kept under `greatest(32, 2 * merge_fanin)` by merging the
`merge_fanin` smallest, inserts wait for the worker at 112 entries, and an insert seals the lane itself when 4 segments are waiting. VACUUM merges
down to `vacuum_segments`, except that a segment more than `merge_fanin` times larger than the ones before it is left alone (`sift.merge(index, true)` merges
it anyway). `write_buffer_limit`, `merge_fanin` and `dead_rewrite_pct` scale and tune these thresholds; the rest are constants.

**Cost of one library (3.0).** `pg_sift` 3.0 carries the BM25 index and the column engine in one library, so the library is about twice the size of 2.0's and each new backend that loads it links more symbols. Against 2.0.x (`bench/steps/cpu_gate.sh`, user instructions per operation) three BM25 cells are accepted above 1.02: the first query of a session (`session`, about 1.04 to 1.05), a `VACUUM` with nothing to free (`vacuum_nofree`, about 1.04, on 3.9M instructions) and a ranked query on a one-segment index (`idle1_rank`, 1.02 to 1.03). Every other cell (queries, inserts, `COPY`, `CREATE INDEX`, `sift.merge`, `sift.promote`, `VACUUM` with work) is within 1.02. Connection pooling amortizes the session cost; a deployment that needs neither `sift_col` nor `sift_table` pays it for nothing.

## Column indexes, aggregates and sift_table

Three pieces share the column engine ([docs/COL.md](COL.md)); none needs `shared_preload_libraries`, and `CREATE EXTENSION pg_sift` (3.0) installs all of them.

- **`sift_col` index** over a heap table (`CREATE INDEX ... USING sift_col` on a `sift_table` is refused: it is already in column format and `SiftAgg` reads it as it is): `CREATE INDEX ON orders USING sift_col (store_id, status, total, (placed_at::date))` keeps those
  columns once more, column by column, with zone maps; filters and aggregates read it instead of the heap rows, and the heap stays authoritative (MVCC
  is the table's). Types: integers, `bool`, dates and timestamps, `float4`/`float8`, `numeric(p, s)` of any precision, unconstrained `numeric`, `money`, `interval`, `time`, `timetz`, enums, `uuid`, `bytea`, `inet`, `cidr`, `macaddr`, `macaddr8`, `bit`, `varbit`, arrays, ranges and multiranges (stored as lanes or images; keys, aggregates, `FILTER`, `OR`, `CASE` and expressions over them are PostgreSQL's own functions on the value and are pushed down, and a group of equal but differently spelled values shows the first spelling seen, [docs/COL-LEAFTYPES-32.md](COL-LEAFTYPES-32.md); `point`, `tsvector`, `xml` and `json` are refused), `char(n)`, `name`, and `text` of any length (up to 1 GB; a unit with strings over 255 bytes stores them inline). Reloptions: `write_buffer_limit` (kB, 4096, like the
  `sift` index) and `paths` for `jsonb` attributes (`WITH (paths = 'metrics.* float4, items[]{sku text, qty int4}')`, [docs/COL-SHRED.md](COL-SHRED.md)) and `compression` (`auto`, `none`, `lz4[:N]`, `zstd[:N]`; per column `'auto; col=zstd:9'`; needs `REINDEX`) and `preagg` (`default`, `off` or `cube(dim: measure, ...)`, the pre-aggregates written with every unit; a change applies to the segments written afterwards, [docs/COL-PREAGG.md](COL-PREAGG.md)) and `dead_rewrite_pct` (default 10) and `cluster_advice` (default on: `sift.cluster_advice()` may recommend an order for the table).
  `sift.index_advice(index)` is the profile of the columns of a `sift_col` index and what its options could still use, advice only: per column its kind (`dimension`: at most 256 distinct values in 90% of 32 evenly spaced units; `key`, `unique`, `date`, `measure`, `text`, `list`), what the index holds of it (the cube histogram in so many units, the key list or bitmap, the calendar cells, a rollup), and the advice when a rule holds (add the dimension to `preagg = 'cube(...)'`, `key_columns = 'auto'`); per pair of dimensions the share of units whose combinations fit a rollup of the rows, and `rollup_dims = 'a, b: <measure>'` when that is 90% of them for no more than 3% of the index. The same rows after a `REINDEX`; the options stay explicit.
  Maintenance is the same as for `sift` indexes (background worker, VACUUM); `sift.col_index_info`, `col_segments`, `col_leaves`, `col_verify`, `col_promote`,
  `col_merge`, `col_maintain` and `col_maintenance_due` inspect and drive one (`col_merge(index, true)` seals the open write buffer and merges every segment into one: `col_promote` and the merge in a call, without blocking readers or writers).
  `CREATE INDEX` and `REINDEX` (also `CONCURRENTLY`) are parallel on large tables with the workers PostgreSQL plans (`max_parallel_maintenance_workers`, `min_parallel_table_scan_size`,
  the table's `parallel_workers`, 32 MB of `maintenance_work_mem` for each process): each process writes the segments of the superblocks it took, so the index has a segment per process until a merge,
  and its file is longer by the unused end of each process's last run (free space, up to 64 MB each); the rows it serves are those of a serial build ([COL.md](COL.md) 2.6).
- **Aggregate pushdown:** a `GROUP BY` over a table with a `sift_col` index (or over a lateral unnest of an indexed `jsonb` array) with `count`, `count(DISTINCT)`, `sum`,
  `avg`, `min`, `max`, filters and `IN` lists runs as `Custom Scan (SiftAgg)` when the planner can prove the result equals the plain plan's; anything else keeps
  its plan ([docs/COL-PLAN.md](COL-PLAN.md), [docs/AGG.md](AGG.md)). `SET sift.enable_agg = off` compares the two; `scripts/coldiff.sh` does it for a whole corpus.
  A partitioned table works the same when every partition the query reads has a `sift_col` index of the same columns and options: create it per partition or
  once on the parent (`CREATE INDEX ON parent USING sift_col (...)` builds one per partition, also for later ones); `EXPLAIN` lists the `Partitions:` ([AGG.md](AGG.md), "Partitioned tables").
- **`sift_table`:** `CREATE TABLE t (...) USING sift_table` is a table access method with native MVCC, btree and `sift` indexes, VACUUM, generic WAL and `pg_dump` A `GROUP BY` of the calendar (`date_trunc('month', ts)::date`, `extract(isoyear FROM d)`, `ts::date`, a week or a quarter) and a date range are answered from the calendar cells each unit stores for its `date`, `timestamp` and `timestamptz` columns, so no generated month column or expression index is needed ([COL-PREAGG](COL-PREAGG.md) section 2); on a `timestamptz` write `AT TIME ZONE 'UTC'` in the key (the session time zone does not give the cells' UTC days); a `sift_col` index gets the same cells for the columns of its reloption `calendar_columns = 'ts, d'` (or `'all'`).
  support ([docs/COL-TAM.md](COL-TAM.md)). New rows go to row-format write pages; when a write buffer fills, or VACUUM or autovacuum runs, the settled rows become compressed column segments with the same TIDs (`sift.table_segments('t')` lists them with their column format minor, `sift.table_space('t')` the file and its free blocks, `sift.table_maintain('t')` does VACUUM's work on the segments and the file without its index cleanup: [COL-TAM §7](COL-TAM.md); settings: `sift.table_write_buffer_limit`, `sift.table_inline_promote`, `sift.table_compression`, `sift.table_dead_rewrite_pct`, in the settings table of [Maintenance and operations](#maintenance-and-operations)). VACUUM clears deleted rows out of the column segments, rewrites a segment once more than `sift.table_dead_rewrite_pct` percent of it is dead, and, when it can take the exclusive lock, can reuse newly released space and relocate tail segments into interior free runs before returning the free tail to the operating system in the same VACUUM (enabled by default; `vacuum_truncate = false` or `VACUUM (TRUNCATE FALSE)` disables this exclusive-lock reuse, relocation and tail-truncation step while other ordinary maintenance continues; allowance and deferral conditions in [COL §2.7](COL.md#27-liveness-and-vacuum)); `VACUUM FULL` and `CLUSTER` write the table anew as column segments. Every type is kept exactly. Its limits are listed once, under [Limits](#limits); publishing one under `wal_level = logical` is under [Replication](#replication).

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

### Memory of a backend

A backend that plans and runs statements of many aggregate arms keeps far more memory after them than its memory contexts hold: the planner and the scan free blocks of tens of megabytes, and the C library of Linux (glibc) keeps them in the heap of the process for the next statement, because its threshold for mapping a block by itself grows to the largest block it has freed. For a server with hundreds of pooled backends that is the number to watch. Two clients replaying 258 statement shapes of a production mix (40 to 60 aggregate arms for the largest, 195,000 rows, 75 seconds, the same seed, run twice) hold on average per backend:

| allocator setting | private memory of a backend | CPU per statement |
|---|---|---|
| default | 104 MB (116, 93) | 145 ms (154, 136) |
| `MALLOC_MMAP_THRESHOLD_=131072` | 70 MB | 145 ms |

(The same mix held 156 MB per backend before a statement kept the memory of one plan, not of the up to four it makes.) A fixed threshold of 128 kB gives every freed block above it back to the system, so a backend holds what its statements use. Set it for the server process, where the memory is the limit (a pool of hundreds of backends), not where the CPU is (the cost is the fresh pages the next block touches); `Environment=MALLOC_MMAP_THRESHOLD_=131072` in the systemd unit of PostgreSQL, or `GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072`. The library does not call `mallopt` or `malloc_trim` itself: both change the whole process, the server's own allocations too. Other allocators (musl, jemalloc) were not measured.

### Why a query kept the normal plan

The pushdown refuses most shapes by design, and the planner gives up without a word. `SET sift.explain_refusals = on` makes it say why: for each candidate path (arm: `single`, `distinct`,
`topn`, `two_level`, `rows`, and the decisions `lookup` and `cte_inline`) it reports the first reason the path gave up for, as a notice in the session that plans the query (also under `EXPLAIN`),
and compares a path that was built with the normal plan (`cost`). The setting changes no plan and costs nothing when off.

```sql
SET sift.explain_refusals = on;
EXPLAIN (COSTS OFF) SELECT kind, count(*) FROM plain_table GROUP BY 1;
-- NOTICE:  SiftAgg not used: no_source: no sift_col index or sift_table covers the table
-- DETAIL:  arm: single
```

A sub-select is planned in its own query level and its arms are named `L2:single`, and the plan of a statement with its shared CTE inlined, `inline:single`.
The notice names the arm in its detail; the detail of the reason names the leaf, the expression or the limit and its value (`the limit group_keys is 32`).
`sift.refusal_slugs()` lists the slugs below with their class: `limit` is a query with more of something than the scan holds (a fixed limit of [LIMITS.md](LIMITS.md), lowered by
`sift.test_limits` in a test build), `semantic` a shape the scan does not prove or does not need.

`sift.explain_why(query)` gives the same as rows without running the query or setting anything: it plans one `SELECT` (the privileges to read its tables are checked) and returns one row per arm with
`used` (the arm is in the plan), `slug` and `reason` (why it gave up, NULL when used) and `scan_cost` and `normal_cost` (the path that was built and the cheapest normal path it was compared with, both at the grouping stage: the total before an `ORDER BY` or a `LIMIT` above it, while PostgreSQL chooses by the cost of the top node; NULL without such a path).
A `cost` refusal that the heap check decided says so: `the scan costs 705, the normal plan 658; 100% of the heap pages are not all-visible, which adds 163 to the scan: VACUUM (ANALYZE) the table` (a rewrite by `CLUSTER`, `VACUUM FULL` or `pg_repack` leaves the visibility map empty).
A `cte_cost` refusal says the same when the heap check of the inlined scans is what keeps the shared CTE (`inline costs 1654 (the scans 1654), the shared CTE 1740 (the work the scans replace 1740); 100% of the heap pages are not all-visible, which adds 547 to the scans: VACUUM (ANALYZE) the table`): it is said only when the inlined plan would win without that charge in both plans.

```sql
SELECT arm, used, slug FROM sift.explain_why('SELECT kind, count(*) FROM plain_table GROUP BY 1');
```

<!-- refusals:start -->
| slug | class | by design | the query kept the normal plan because |
|---|---|---|---|
| `other` | semantic |  | the shape is not one the aggregate pushdown proves, and no more specific reason is recorded |
| `disabled` | semantic | yes | sift.enable_agg is off |
| `query_kind` | semantic | yes | the query has a set-returning function in the select list, row locks or a recursive CTE |
| `from_shape` | semantic |  | the FROM list is not one table with the inner, left, semi or anti joins and lateral functions the scan reads |
| `table_kind` | semantic | yes | the table is inherited, sampled, row-secured or not a plain, materialized or partitioned table |
| `no_source` | semantic | yes | no sift_col index or sift_table covers the table |
| `partition_source` | semantic |  | a partition has no column index the plan can use, or the partitions do not share a layout |
| `partition_count` | limit |  | the table has more partitions left by PostgreSQL's pruning than a scan reads |
| `join_child` | semantic |  | a joined sub-select, VALUES list, CTE or function is not one the scan can read or run as a child |
| `outer_join` | semantic |  | an outer join of the scanned table keeps rows the scan cannot reproduce |
| `unnest_leaf` | semantic |  | the array or object a lateral function unnests is not a leaf of the column index |
| `where` | semantic |  | a condition of the WHERE or ON clause is not one the scan can test |
| `join_condition` | semantic |  | a join condition is not an equality or filter the scan can look up |
| `group_key` | semantic |  | a group key is not a leaf, or an expression of one, that the scan reads |
| `too_many_keys` | limit |  | the query groups by more columns than the scan holds |
| `error_parity` | semantic |  | the scan could raise, or not raise, an error for a value where the normal plan does the other (a cast of a JSON null before PostgreSQL 18, a guard of jsonb_typeof around a read) |
| `output_expr` | semantic |  | the select list or HAVING reads something besides group keys and the aggregates the scan computes |
| `aggregate` | semantic |  | an aggregate, or its FILTER, DISTINCT or ORDER BY, is not one the scan computes |
| `distinct_on` | semantic | yes | DISTINCT ON has no total order for the row it keeps |
| `grouping_sets` | semantic |  | the arm does not group by grouping sets |
| `dirty_units` | semantic |  | some unit holds values a leaf the query reads cannot store, and the scan could not read those rows again as SQL does |
| `dirty_order` | semantic | yes | a condition casts a path whose value some unit cannot store, and PostgreSQL may test it before a condition that would drop the row whose value raises, or after one that raises too |
| `volatile` | semantic | yes | the query calls a volatile function, which PostgreSQL evaluates for every row and the scan cannot reproduce |
| `first_memory` | limit |  | the first rows of the groups would not fit in the memory of a hash aggregate |
| `group_memory` | limit | yes | the groups would not fit in the memory of a hash aggregate, and the scan cannot spill them, or the normal plan, which spills them, is estimated cheaper than the scan with its spill |
| `lateral_limit` | semantic |  | a LATERAL sub-select with LIMIT 1 stays a child that runs for each key: no unique index holds its key, or its ORDER BY leaves ties |
| `two_arms` | semantic |  | the arms of the two level aggregate are not UNION ALL branches of aggregates over one scan, each grouped by a unique row id |
| `two_keys` | semantic |  | a key of the two level aggregate is not one every arm groups by |
| `two_aggs` | semantic |  | an aggregate over the arms is not one the two level scan computes |
| `spine_shape` | semantic |  | the join of the arms to a spine is not an equality of their keys, or the WHERE reads the arms |
| `shape_width` | limit |  | the query has more joined tables, value combinations, leaves, arms or columns than the scan holds: the detail names the limit and its value |
| `spine_rows` | limit |  | the rows of the spine are estimated above sift.agg_lookup_max_kb |
| `rows_shape` | semantic |  | the grouping of the rows of a SiftAgg plan has no count(DISTINCT) of integers, or a key or aggregate it cannot group |
| `topn_shape` | semantic |  | the ORDER BY .. LIMIT is not over the scanned table's columns with a condition the scan tests |
| `cost` | semantic | yes | the normal plan is estimated cheaper than the scan |
| `cte_cost` | semantic | yes | the shared CTE plan is estimated cheaper than the plan with the CTE inlined |
<!-- refusals:end -->

PostgreSQL 17-19 table access methods cannot define their own `WITH (...)` options (a patch targets PostgreSQL 20), so a `sift_table` is tuned with PostgreSQL settings (`SET`, `ALTER DATABASE ... SET`, `postgresql.conf`), not per table; a `sift_col` index has real options stored with the index (`CREATE INDEX ... WITH`, `ALTER INDEX ... SET`). Background promotion by `VACUUM`, autovacuum and the maintenance worker uses the server or database value of the setting, not your session's; the exact rule is under [Limits](#limits).

| option | `sift_col` (per index) | `sift_table` (setting) |
|---|---|---|
| compression of the column chunks | `compression` (`auto` default, `none`, `lz4[:N]`, `zstd[:N]`, per column `'auto; col=zstd:9'`; needs `REINDEX`) | `sift.table_compression` (empty = `auto`; same grammar) |
| size of the write buffer before it is sealed | `write_buffer_limit` (kB, `ALTER INDEX ... SET`) | `sift.table_write_buffer_limit` (kB) |
| promotion while rows are inserted | not applicable (the index follows the heap) | `sift.table_inline_promote` (`on`; `off` leaves it to `VACUUM`) |
| dead rows | `dead_rewrite_pct` (per index) | `sift.table_dead_rewrite_pct` (percent of a segment) |

A `sift_table` has no `paths` or rows-per-unit setting: a `jsonb` column is stored as its exact bytes and units follow the 2 MB superblocks.

The chunk codec (encodings plus LZ4 or zstd, [docs/COL-CODEC.md](COL-CODEC.md)) compresses the chunks of a `sift_col` index (option `compression`: `auto`, `none`, `lz4`, `zstd`, or per column `'auto; col=zstd:9'`). It is built into the library; `make SIFT_NO_LZ4=1` or `SIFT_NO_ZSTD=1` build without one.

### Set filters

A filter on a set of ids (the members of a group, the entities that have an attribute) runs in `Custom Scan (SiftAgg)` as an array parameter, a literal `IN` list, or a
join to the table that holds the set. [docs/SET-FILTERS.md](SET-FILTERS.md) has the measurements and the settings; the array parameter is the fast one: send the ids as one
`int4[]` of a prepared statement on a generic plan (`SET LOCAL plan_cache_mode = force_generic_plan`), and keep a small set (about 100 ids) on a custom plan.

The array form pushes down for these key types (the element type of the array is the type of the column; cast the array, `ANY($1::int8[])`, `ANY($1::text[])`, `ANY($1::date[])`):

| key column | array | how `SiftAgg` tests it |
|---|---|---|
| `int2`, `int4`, `date`, `int8`, `timestamp`, `timestamptz` | the same type of array (an `int4[]` for an `int8` column too) | a byte table over the range of the array (up to 4 MB), else a sorted array (32-bit types) or a hash table (64-bit types) |
| `text` | `text[]` | PostgreSQL evaluates `col = ANY($1)` once per distinct value of the column, with a hash table of the array built on the first value |
| `uuid`, `bytea` | | not supported: a `sift_col` index refuses these columns, so the query keeps the normal plan |

A `NULL` element matches nothing (`= ANY`) and makes `<> ALL` empty, an empty array keeps no row (`= ANY`) or every row (`<> ALL`), as in SQL. The timings by type are in
[docs/SET-FILTERS.md](SET-FILTERS.md), section 2.

```c
/* libpq: one text parameter of type int4[] (OID 1007) */
Oid         types[1] = {1007};
const char *vals[1] = {"{101,102,103}"};
PGresult   *r = PQexecParams(conn, "SELECT channel_id, count(*) FROM events WHERE entity_id = ANY($1::int4[]) GROUP BY 1", 1, types, vals, NULL, NULL, 0);
```

```ruby
# Sequel: the pg_array extension binds the array as one parameter of a server-side prepared statement
DB.extension :pg_array
ps = DB[:events].where(Sequel.lit('entity_id = ANY(?::int4[])', :$ids)).select_group(:channel_id).select_append { count.function.* }.prepare(:select, :by_set)
DB.transaction { DB.run 'SET LOCAL plan_cache_mode = force_generic_plan'; ps.call(ids: Sequel.pg_array(ids, :int4)) }
```

### Is the index clustered, and what did the scans do

`sift.col_clustering(index, width_pct => 1)` tells, per integer, date and timestamp leaf of a `sift_col` index, whether sorting the table by it would let scans skip more units. It reads the
zone maps (min and max of each unit) of the immutable segments, no chunk, and answers with the units a filter on a value of the data touches, as `Units` of `EXPLAIN ANALYZE` counts them:

| column | meaning |
|---|---|
| `units`, `zoned_units` | units of the index; those whose zone is a bounded range of the leaf (a unit with an overflow value is always read) |
| `useful_pct` | share of them whose range is narrower than the span of the leaf: the zone can exclude a value |
| `point_units` | units an equality on a value of the data touches, on average |
| `range_units`, `range_ideal` | the same for a range of `width_pct` percent of the span, and what a column sorted by itself would touch |
| `depth_max` | the most units whose zones hold one value: the deepest overlap. `point_units` is the average and hides a few deep values; rows that arrive out of order (a late date, a key from the whole range) raise it by one unit per batch written |
| `correlation` | order of the units against the order of their values: 1 ascending, -1 descending, near 0 scattered |
| `verdict` | `clustered` (an equality touches 2 units or 2% of them), `partial` (up to 25%) or `scattered` |

A `scattered` leaf that queries filter on is the one to sort by: `CLUSTER`, load the rows in its order, or `INSERT .. SELECT .. ORDER BY` into a fresh table, then `REINDEX`. Rows still in the write buffer are not counted.

`sift.cluster_advice(rel => NULL, min_rows => 100000, key => NULL)` says which order is worth a rewrite, one row per `sift_col` index of the table (or index, or of every index you may read): the `order_by` to sort by, the `units_now` an
equality or window on it reads and the `units_after` a table sorted by it would, the `saved_rows` of every query, the `churn_pct` since the last `ANALYZE`, a `verdict` (`recommended` or `not recommended`), its `reason` and
the `pg_repack` call followed by the `VACUUM (ANALYZE)` the rewrite needs. `key => 'a, b'` asks about an order of your own (the filter keys of the workload: the function alone proposes the best single leaf, and a leaf it would scatter can make it say `not recommended` for it): `order_by` is that list, `units_now` and `units_after` those of an equality on its first leaf, and the leaves outside the list are the ones the sort can scatter. A reason ends with a note when under half of the heap pages are all-visible. It is not recommended when the table is small (a query saves under `min_rows` rows, about a millisecond of scan, or the index has under 16 units), already ordered, when the sort would scatter a leaf
that is ordered now and saves about as much, or when over 10% of the rows change a day (the order is lost faster than a repack restores it). A rewrite restores the order at once: the planner and the scans read the
zone maps of the rebuilt index. `CLUSTER`, `VACUUM FULL` and `pg_repack` leave the visibility map empty, and autovacuum does not
come for it (a rewrite counts no inserts, so the insert threshold is not reached): with `sift.col_auto_merge` on, the first scan that checks the rows in the heap starts the worker that vacuums the table (500,000 rows, a 200 ms `sift.maintenance_naptime`: the map set 1.6 s after the `CLUSTER`, a grouped sum 12 ms before and 0.15 ms after); with it off, run `VACUUM (ANALYZE) t` right after the rewrite. A scan checks the heap for every row of a page that is not all-visible, and the planner prices that (`relallvisible`
of `pg_class` is 0): on a 200,000-row table a scan of 133,660 rows read 33,415 heap pages and took 320 ms instead of 82 ms. The planner charges each page of the units the zones keep that holds a selected row, at what a page read costs: about 1.3 us from shared buffers, 3 us from the cache of the operating system and a sequential or random page read from the disk, by `shared_buffers`, `effective_cache_size` and the size of the table (as an index scan of PostgreSQL does), and 4 ns for each row of it. A clustered table of wide rows whose scan costs 531 with the map set costs 705 without it for a twentieth of the units, against 658 for the normal plan, which wins until the `VACUUM`; the larger the share of the table the scan reads, the larger the charge, and a table that does not fit in memory pays the disk for it. Switch it off with the setting `sift.cluster_advice` (off: every index answers `not recommended`) or one index with
`ALTER INDEX ... SET (cluster_advice = off)`.

Rewriting the table: `CLUSTER` and `VACUUM FULL` lock it exclusively and work for heap tables with every sift index and for a `sift_table`. `pg_repack` (online, `--order-by`, 1.5.3 tested on PostgreSQL 18) rebuilds the
`sift`, `sift_col` indexes and keeps their options and the results of the scans, also under concurrent writes, as a non-superuser table owner and on a standby; it needs a primary key or a not-null unique index. Its copy is an `INSERT .. SELECT .. ORDER BY`, which
fills the tail of a page through the free space map, so a table of rows of unequal width keeps a small share of rows out of place (0.5% of the rows of a test table of 900,000 wide rows, enough to double the units of a key and
year window); `CLUSTER`, `VACUUM FULL` and `CREATE TABLE .. AS .. ORDER BY` write the exact order. It
cannot rewrite a `sift_table` (it swaps in a heap copy): an event trigger refuses it, and `pg_repack --only-indexes` is fine. `pg_squeeze` only supports heap tables.
After any of them run `ANALYZE t` for the planner (`pg_repack` does), and `VACUUM t` too with `sift.col_auto_merge` off.

`sift.stats()` returns what the `Custom Scan (SiftAgg)` scans of the session did, one `(name, value)` row per counter, and `sift.stats_reset()` zeroes them. The counters are plain memory of the
backend: no shared memory and no `shared_preload_libraries`, nothing is shared between sessions and a restart or a reconnection starts them again (read them before a pooled connection is returned, or log
them with `auto_explain`). A scan adds its numbers once, when it ends, from the counters `EXPLAIN ANALYZE` shows; a statement that fails adds none. A query that kept the normal plan adds
`refused.<slug>` while `sift.explain_refusals` or `sift.count_refusals` is on (the second counts without the notice), and for a refusal by a fixed limit also `limit.<id>`, the id of
[LIMITS.md](LIMITS.md): `SELECT name, value FROM sift.stats() WHERE name LIKE 'limit.%'` lists the limits the session ran into.

| name | counts |
|---|---|
| `scans` | `Custom Scan (SiftAgg)` scans that ended (the arms of one fused pass are one) |
| `workers_launched` | parallel workers those scans launched |
| `units` | units the scans read or answered (not the pruned ones) |
| `units_pruned_zone` | units skipped before they were read: their zone maps hold no match |
| `units_pruned_set` | units skipped before they were read: only their summary of `key_columns` holds no match |
| `units_pruned_search` | units skipped before they were read: none of their rows is in the search selection |
| `units_preagg` | of the units, those answered from their pre-aggregates |
| `rows_decoded` | rows of the units that were scanned |
| `rows_vetted` | rows checked against the heap for visibility |
| `rows_rechecked` | rows of dirty units read again from the heap |
| `units_rechecked` | units that had such rows |
| `units_no_rollup` | exact units scanned for lack of their rollup |
| `units_rollup_cost` | exact units scanned because their rollup cost more than their elements |
| `lookup_cache_hits` | lookup tables taken from the cache of an earlier query |
| `lookup_cache_misses` | lookup tables the cache could have held and that were loaded |

`sift.col_rollup_misses()` lists the scans of the session that the pre-aggregates of their index did not answer, by index (`rel`) and reason, most scans first, fixable reasons first. `reason` is a slug
(`no_preagg`, `no_rollup`, `key_not_a_dim`, `dim_not_stored`, `measure_not_stored`, `key_expression`, `joined_table`, `union_arms`, `grouping_sets`, `aggregate_filter`, `aggregate_distinct`, ...,
`unit_without_rollup` for scans that had them and read units without their rollup); `fixable` is true when another `rollup` or `preagg` option of the index would answer it (the dims and measures the queries
asked for are the ones to name); `units` counts the units scanned for lack of their rollup. The list holds up to 256 pairs of index and reason.

## Replication

**Physical:** the index goes through generic WAL, so a hot standby (`pg_basebackup -R`) replays builds, inserts, VACUUM,
`CREATE INDEX CONCURRENTLY` and `REINDEX`, and serves `@@>`, ranked `sift.score()`, `count(*)` and pushed-down filters read-only with the
primary's rows and scores; a promoted standby takes writes at once. No `shared_preload_libraries`, but install the same pg_sift version on
every node that queries. `sift.*` settings are per node; `hot_standby_feedback = on` avoids retryable "segment recycled during scan" errors. Without feedback, `sift.standby_conflicts` (on by default, superuser) makes the primary log a recovery conflict when it reuses space a merge freed: standby scans are waited for (`max_standby_streaming_delay`) or cancelled like any standby query, never served recycled pages, and the replay of other long snapshots waits the same way a heap conflict makes it wait. `off` skips the record: such a scan fails at once with the retryable "segment recycled during scan".

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

Every fixed limit of the code, with its value, what happens at it and the setting that changes it, is in the generated table [LIMITS.md](LIMITS.md); the list below adds the context.

- PostgreSQL 17, 18 and 19; x86-64 tested, arm64 tested with PostgreSQL 18 (Linux in CI, macOS by the formula's test), other architectures untested; heap and sift_table tables; BLCKSZ 8192; database encoding UTF8 or SQL_ASCII.
- `sift_table` (full list in [docs/COL-TAM.md](COL-TAM.md), section 9):
  - at most 127 columns (`CREATE TABLE`, and an `ALTER TABLE` that adds a column or brings declared paths back past it, fail with `54000`; `DROP COLUMN` is not checked, so a table that 3.2 let grow too wide can be narrowed), 128 directory entries, `BLCKSZ` 8192;
  - a single `COPY` or `INSERT ... SELECT` writes any number of rows in one transaction (the write buffers of one command become column groups that carry their writer while it runs, and `VACUUM` freezes them once it is committed or drops them if it aborted; a 10,000,000-row `COPY` takes about 180 MB in the backend). a transaction of many commands (a loop of single-row `INSERT`s, savepoints, several sessions into the same write buffers) is not bounded either: once the directory is half full every write buffer it closes becomes such a group, and only a write buffer that holds a row deleted since the last `VACUUM` or a speculative insertion in progress waits (the insert that cannot open another write buffer then fails with `54000`, and the message says to `VACUUM` or to end the transaction); writers merge the column segments as they go;
  - `0A000` for a DML on a published table without the preload (see [Replication](#replication)), and an aggregate scan that reads a column holding a non-canonical float NaN (`SET sift.enable_agg = off` for that query);
  - `sift_table` has no pre-aggregate rollups of array elements, and a jsonb path is read only once it is declared and every segment holds its leaf (`VACUUM FULL`); no HOT, and shared row locks degrade to exclusive;
  - `xmax` of a row locked by several transactions is the last locker's, not a MultiXactId;
  - no per-table options: `sift.table_write_buffer_limit`, `sift.table_inline_promote`, `sift.table_compression` and `sift.table_dead_rewrite_pct` are ordinary settings that the backend doing the work reads when it does it (PostgreSQL 17 through 19 give a table access method no reloptions of its own: `WITH` knows heap's and TOAST's only and `amoptions` is for indexes; the patch that adds them, commitfest entry 5631, targets PostgreSQL 20). Inline promotion and the closing of write buffers follow the settings of the inserting session (`SET`, `SET LOCAL`, the role's or the database's default); a manual `VACUUM`, those of the session that runs it; autovacuum has no session, so it applies the server settings (`postgresql.conf`, `ALTER SYSTEM`), `ALTER DATABASE ... SET`, and the `ALTER ROLE ... SET` defaults of the bootstrap superuser (the role autovacuum runs as), never the writer's session or the owner's role; the maintenance worker does not touch a `sift_table`. The settings of the session that wrote the rows are not kept for the VACUUM that promotes them later. So set what background promotion should use at the server or database level; a `SET` in a session shapes only that session's inserts and its own VACUUM (the segments already written keep their codec until a merge or rewrite). `sift_col` indexes do have per-index options (`compression` and the rest, see above), so a column index on a table needs no setting for it. Tested by `test/t/010_tbl_settings.pl`;
- One key per index: multicolumn, `INCLUDE` and unique indexes and non-text keys are rejected (build a key of several columns with `sift.sections`). No plain or index-only scans.
- Queries: at most 256 leaves, nesting depth 32, phrases of 128 tokens, boosts up to 10000.
- `fields`: at most 14 field names, 128 bytes of names in all; `prefix_index` up to 15.
- A word over 255 bytes is stored as its first 236 bytes and a hash of all of it (`REINDEX` makes an index built before 3.3 follow: it holds such words cut at 255 bytes, so a query for one finds nothing there), `tf` saturates at 255 and document length at 65535; at most 128 segments per index (writers merge the smallest closed segments from 112 entries; with the directory full and the merges failing, the open write buffer takes the writes and `sift.workers` shows the failed step, until a merge succeeds). A merge reads at most 16 GB of segments: a segment of that size is not merged again (VACUUM still clears its dead postings through the liveness bits).
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
  `sift.rls_scores = on` (superuser) gives the role a ranked scan whose scores count the hidden rows: [Row-level security](#row-level-security).
  Security barrier views stay refused.
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
- pg_upgrade to a newer PostgreSQL major keeps the files as they are ([UPGRADING.md](UPGRADING.md), tested from the previous major when both are installed). `pg_sift_test` (built with `SIFT_TESTING=1`) is for the test suite only.
