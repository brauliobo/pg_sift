# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml)
[![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml)
[![release](https://img.shields.io/github/v/release/brauliobo/pg_sift)](https://github.com/brauliobo/pg_sift/releases/latest)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018-336791)](#install)
[![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

pg_sift is a full-text search index for PostgreSQL 17 and 18 (19 is experimental), an index access method used as `USING sift`. It answers boolean, phrase, prefix,
wildcard, regex and fuzzy queries, ranks with exact BM25 and counts matches from the index. Postings are keyed by heap ctid and stored as
bitmaps, so AND, OR and NOT are bitmap operations and MVCC visibility comes from the visibility map. Design: [DESIGN.md](DESIGN.md).

- Exact BM25 (`sift.score()`), ranked top-k with block-max pruning, `count(*)` straight from the index.
- Ordinary table semantics: MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, partial and expression indexes. Crash and replica
  safe through generic WAL ([replication](#replication)), no `shared_preload_libraries`.
- 1.1: filter pushdown, keyword facets, opt-in parallel scan.
- Small indexes: 48 MiB for 218,058 Wikipedia articles, 3.4x smaller than pg_search (see [Benchmarks](#benchmarks)).

## Install

Requires PostgreSQL 17 or 18 (server headers, `pg_config` on `PATH`; 19 is experimental) and a C compiler. Tested on x86-64.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
```

- Arch Linux: AUR `pg_sift` (recipe in [packaging/](packaging/)).
- Debian 13, Ubuntu 24.04 with PGDG `postgresql-18`: download the `.deb` from the [release](https://github.com/brauliobo/pg_sift/releases) and
  `sudo apt install ./postgresql-18-pg-sift_1.1.0-1_amd64.deb`.
- RHEL, Rocky, Alma 9 or 10 with PGDG `postgresql18-server`:
  `sudo dnf install https://github.com/brauliobo/pg_sift/releases/download/v1.1.0/pg_sift_18-1.1.0-1.el9.x86_64.rpm`.

Then, as a superuser (the extension creates the schema `sift`); after installing a newer version run `ALTER EXTENSION pg_sift UPDATE`:

```sql
CREATE EXTENSION pg_sift;
```

## Quick start

```sql
CREATE TABLE docs (id int PRIMARY KEY, title text, body text, published date, category int);
INSERT INTO docs VALUES
  (1, 'Hotel review', 'Breakfast was great and the staff were friendly. The pool was noisy.', '2026-01-10', 1),
  (2, 'Cafe', 'Friendly staff, no breakfast, but a quiet pool and spa.', '2026-02-11', 1),
  (3, 'Resume tips', 'A good résumé is short. Cafés near the office help.', '2026-03-12', 2),
  (4, 'Running', 'Runners were running; she runs every day.', '2026-04-13', 2),
  (5, 'Noisy hotel', 'The hotel was noisy at night, noise everywhere. Friendly staff though.', '2026-05-14', 1);
CREATE INDEX docs_body ON docs USING sift (body);

SELECT id FROM docs WHERE body @@> 'breakfast friendly';                   -- all terms: 1, 2
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'pool OR spa'
ORDER BY sift.score(ctid) DESC LIMIT 10;                                    -- BM25 top-10
SELECT count(*) FROM docs WHERE body @@> 'friendly';                        -- 3, from the index
```

`text @@> text` matches a document against a query; without an index it is evaluated per row with the same results, only slower.

## Queries

`AND`, `OR`, `NOT` are keywords in upper case only; juxtaposition is `AND`; precedence is `NOT` > `AND` > `OR`; a `NOT` needs a positive
term in its AND group. A wildcard, regex or fuzzy leaf expands to dictionary terms; more than `sift.max_expansions` (512) is an
error, not a truncation. A prefix never is: it matches every word that starts with it, however many (`h*` on a million
names), and above that many terms it scores as the one term `prefix_index` would hold for it.

```sql
SELECT id FROM docs WHERE body @@> 'breakfast NOT noisy';             -- exclusion: 2
SELECT id FROM docs WHERE body @@> '{pool spa noisy}';                -- any of a set of words or patterns
SELECT id FROM docs WHERE body @@> '"friendly staff"';                -- phrase: 2, 5
SELECT id FROM docs WHERE body @@> 'brea* OR n?isy OR /no.*y/ OR frendly~1';  -- prefix, wildcard, regex, fuzzy
SELECT id FROM docs WHERE body @@> '(pool OR spa) AND friendly^3';    -- grouping and boost
```

## Ranking, filters and joins

`ORDER BY sift.score(ctid) DESC LIMIT k` runs a ranked scan that skips blocks that cannot reach the top k; ties break by ctid.
`sift.score()` is computed by the scan: use it in the select list or `ORDER BY` of the query level that scans the table, not in `WHERE`
(filter on a score from a subquery).

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
(scattered). `force` pushes down whenever a bitmap path exists; with `on` the planner costs both plans.

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
stopword list. The analyzer is fixed per index: build the replacement with `CREATE INDEX CONCURRENTLY` and swap. Phrases are rechecked in
the heap; `phrase_index = pairs` also indexes adjacent word pairs, so phrases become bitmap ANDs, for 5.6x the index size. An index has one
key, a `text` column or an immutable expression, so concatenate columns (`coalesce` nullable ones, `NULL` keys are not indexed).

```sql
SELECT token, pos FROM sift.tokenize('Running Cafés, RÉSUMÉ', stemmer => 'en');             -- run, cafe, resum
CREATE INDEX CONCURRENTLY docs_body_en ON docs USING sift (body) WITH (stemmer = 'en', phrase_index = pairs);
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
low-cardinality columns (1.9-4.5x on 26 values); for selective queries or thousands of values `GROUP BY` wins ([DESIGN.md](DESIGN.md) (l)).

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
`sift.segments(index)` and `sift.verify(index, true)` inspect and check an index.

- `REINDEX [CONCURRENTLY]` works as usual; `CREATE INDEX` is parallel on large tables. Changing `k1`, `b` or an analyzer option needs it.
- pg_dump restores rebuild the index. Replication: see [Replication](#replication).
- `EXPLAIN` shows `Custom Scan (SiftScan)`, or `SiftCount` for the `count(*)` pushdown.

| GUC | default | meaning |
|---|---|---|
| `sift.max_expansions` | 512 | terms a wildcard, regex or fuzzy leaf may expand to (error above); a prefix above it scores as one term |
| `sift.prefix_cache_size` | 8MB | memory per backend for the postings of broad prefixes, `0` keeps none |
| `sift.filter_pushdown` | on | `off`, `on`, `force`: give a ranked scan a bitmap of the other quals |
| `sift.parallel_scan` | off | let the planner add parallel SiftScan / SiftCount plans |

Reloptions (`WITH (...)`; all but `mutable_limit` need REINDEX): `k1` (1.2), `b` (0.75), `analyzer` (`text` or `keyword`), `stemmer`
(`none` or `en`), `case_folding` and `accent_folding` (true), `phrase_index` (`none` or `pairs`), `prefix_index` (0-15), `fields`,
`mutable_limit` (kB, 4096), `build_segments` (0; forces N segments and disables the parallel build).

## Replication

**Physical:** the index goes through generic WAL, so a hot standby (`pg_basebackup -R`) replays builds, inserts, VACUUM,
`CREATE INDEX CONCURRENTLY` and `REINDEX`, and serves `@@>`, ranked `sift.score()`, `count(*)` and pushed-down filters read-only with the
primary's rows and scores; a promoted standby takes writes at once. No `shared_preload_libraries`, but install the same pg_sift version on
every node that queries. `sift.*` settings are per node; `hot_standby_feedback = on` avoids retryable "segment recycled during scan" errors.

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

`scripts/replication.sh` checks all of it on a primary, a standby and a subscriber in temporary clusters; `--container` puts every node in
its own docker container.

## Limits

- PostgreSQL 17 and 18, 19 experimental; x86-64 tested (other architectures untested); heap tables only; BLCKSZ 8192; database encoding UTF8 or SQL_ASCII.
- One key per index: multicolumn, `INCLUDE` and unique indexes and non-text keys are rejected. No plain or index-only scans.
- Queries: at most 256 leaves, nesting depth 32, phrases of 128 tokens, boosts up to 10000.
- Terms are truncated at 255 bytes, `tf` saturates at 255 and document length at 65535; at most 128 segments per index.
- Analyzers: English Snowball stemmer only, no stopword list, fixed per index (REINDEX to change).
- Phrases are rechecked in the heap unless the index uses `phrase_index = pairs`.
- `sift.score()` cannot be used in `WHERE`; read it from a subquery to filter on it.
- Partitions: `sift.score()` needs a sift index on every partition the query reads (otherwise an error; unranked matching and `count(*)`
  still work).
- Partitions: BM25 statistics are per partition, so scores can differ across partitions.
- Partitions: `sift.max_score()` is not available through a parent (call it on a partition).
- Partitions: the score cannot be used inside an aggregate or a non-nested-loop join; compute it in a subquery with `OFFSET 0` or `LIMIT`.
- Ranked partition scans are never parallel, and neither are scans with a pushed-down filter.
- Facets: text keys only (use an expression index for integers), cost linear in the distinct values, tables with row-level security are
  refused.
- pg_upgrade is tested within one major version only. `pg_sift_test` (built with `SIFT_TESTING=1`) is for the test suite only.

## Benchmarks

Against ParadeDB pg_search 0.26.1, Timescale pg_textsearch 1.5.1 and rum 1.3.15 on Simple English Wikipedia (218,058 articles, 8 clients,
median of 4 interleaved rounds) and GeoNames cities500 (236,262 places, 1 client, p50), same data, queries and cache state, all five
engines in one session. Cells are queries per second over pg_search. The host was loaded (load average 4-31), so absolute numbers are
noisy; compare engines within a table.

| articles | index | build | and | or | phrase | mixed | `count(*)` | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 47.9 MiB | 1.5 s | 2.49x | 2.33x | 0.65x | 1.78x | 0.70x | 0.93x | 4.31x |
| sift `phrase_index = pairs` | 267.2 MiB | 7.8 s | 2.76x | 2.44x | 2.83x | 2.69x | 3.23x | 1.20x | 7.49x |
| pg_search | 164.3 MiB | 2.9 s | 1x | 1x | 1x | 1x | 1x | 1x | 1x |
| pg_textsearch | 147.3 MiB | 9.7 s | 0.46x | 4.01x | 0.005x | 0.02x | 0.03x | 0.01x | 0.27x |
| rum | 309.9 MiB | 13.5 s | 0.41x | 0.01x | 0.01x | 0.02x | 0.03x | 0.01x | 0.05x |

The default index is the smallest and quickest to build and wins AND, OR, mixed and join, but is **behind pg_search on phrase and
`count(*)`** (heap recheck); `phrase_index = pairs` fixes that at 5.6x the size. pg_textsearch wins only the bare ranked OR. rum wins
nothing: it is the largest index and needs a stored `tsvector` column (19.1 s to fill, 437.7 MiB more).

Places (top-20 p50): index 42.6 MiB / 1.3 s for sift, 129.5 MiB / 3.0 s for pg_search, 68.3 MiB / 2.4 s for pg_textsearch, 94.9 MiB /
2.9 s for rum. sift is ahead of pg_search in all 45 warm cells (1.3x to 38x), e.g. 0.26 vs 1.71 ms for a whole word. pg_textsearch has
no prefix scoring or boost (ranked workloads NA); rum has no boost (NA) and its prefix top-20 takes 0.5-124 ms.

![Index size](docs/img/index-size.svg)
![Build time](docs/img/build-time.svg)
![Throughput by workload](docs/img/articles-throughput.svg)
![Throughput relative to pg_search](docs/img/articles-speedup.svg)
![Autocomplete latency](docs/img/places-latency.svg)

Cold-cache tables, method and smaller tiers: [docs/BENCHMARKS.md](docs/BENCHMARKS.md); raw tables: [docs/results/](docs/results/);
reproduce with `bench/bench all` (see [bench/README.md](bench/README.md)). Changes per release: [CHANGELOG.md](CHANGELOG.md).

## Testing, security, license

`make check` runs every suite in a throwaway cluster (about 2.5 minutes; `make check-full` adds sanitizers, soak, fuzz and mutants), and
`make installcheck` runs the regression tests against a server. Report crashes, wrong results and index corruption privately as described
in [SECURITY.md](SECURITY.md); to contribute see [CONTRIBUTING.md](CONTRIBUTING.md). pg_sift is released under the
[PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
