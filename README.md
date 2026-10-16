# pg_sift

pg_sift is a full-text search index for PostgreSQL 18, an index access method used as `USING sift`. It answers boolean, phrase, prefix,
wildcard, regex and fuzzy queries, ranks with exact BM25 and counts matches from the index. Postings are keyed by heap ctid and stored as
bitmaps, so AND, OR and NOT are bitmap operations, and MVCC visibility comes from the visibility map instead of a document-id mapping. The

- Exact BM25 (`sift.score()`), ranked top-k with block-max pruning, `count(*)` straight from the index.
- One query language: `AND`, `OR`, `NOT`, `{any of}`, phrases, prefix, wildcard, regex, fuzzy, boosts, named fields.
- Ordinary table semantics: MVCC, concurrent writes, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, partial and expression indexes,
  parallel `CREATE INDEX`.
- Crash safe and replica safe through generic WAL, with no `shared_preload_libraries`.
- Small indexes: 48 MiB for 218,058 Wikipedia articles, 3.8x smaller than pg_search (see Benchmarks).

## Install

Requires PostgreSQL 18 (server headers, `pg_config` on `PATH`) and a C compiler. Developed and tested on x86-64, where AVX-512, AVX2 and
scalar kernels are chosen at run time; other architectures are untested.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
```

Arch Linux: AUR: `pg_sift` (the recipe is in [packaging/](packaging/)). Then, as a superuser (the extension is not trusted; it creates the
schema `sift`):

```sql
CREATE EXTENSION pg_sift;
```

No `shared_preload_libraries` entry or restart is needed. Physical replication works because every change goes through generic WAL; a
standby needs pg_sift installed to run queries.

## Quick start

```sql
CREATE TABLE docs (id int PRIMARY KEY, title text, body text, published date, category int);
INSERT INTO docs VALUES
  (1, 'Venue note', 'Breakfast was great and the staff were friendly. The pool was noisy.', '2026-01-10', 1),
  (2, 'Cafe', 'Friendly staff, no breakfast, but a quiet pool and spa.', '2026-02-11', 1),
  (3, 'Resume tips', 'A good résumé is short. Cafés near the office help.', '2026-03-12', 2),
  (4, 'Running', 'Runners were running; she runs every day.', '2026-04-13', 2),
  (5, 'Noisy venue', 'The venue was noisy at night, noise everywhere. Friendly staff though.', '2026-05-14', 1);
CREATE INDEX docs_body ON docs USING sift (body);

SELECT id FROM docs WHERE body @@> 'breakfast friendly';                   -- all terms: 1, 2
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'pool OR spa'
ORDER BY sift.score(ctid) DESC LIMIT 10;                                    -- BM25 top-10
SELECT count(*) FROM docs WHERE body @@> 'friendly';                        -- 3, from the index
```

`text @@> text` matches a document against a query (the operator lives in `pg_catalog`, no `search_path` change). Without an index it is
evaluated per row with the same results, only slower.

## Queries

Terms are analyzed like the indexed text. `AND`, `OR`, `NOT` are keywords in upper case only, juxtaposition is `AND`, and precedence is
`NOT` > `AND` > `OR`. A `NOT` needs a positive term in the same AND group. A word that splits into several tokens (`e-mail`) is a phrase.
Prefix and wildcard literals are folded but not stemmed; a regex is matched against whole indexed terms. Fuzzy has edit distance 1 or 2 with
the first letter fixed, and short terms allow less (`saq~2` is `saq~1`).

```sql
SELECT id FROM docs WHERE body @@> 'breakfast staff';             -- all terms
SELECT id FROM docs WHERE body @@> 'pool OR spa';                 -- any term
SELECT id FROM docs WHERE body @@> '{pool spa noisy}';            -- any of a set of words or patterns
SELECT id FROM docs WHERE body @@> 'breakfast NOT noisy';         -- exclusion: 2
SELECT id FROM docs WHERE body @@> '"friendly staff"';            -- phrase: 2, 5
SELECT id FROM docs WHERE body @@> 'brea*';                       -- prefix
SELECT id FROM docs WHERE body @@> 'n?isy';                       -- wildcard
SELECT id FROM docs WHERE body @@> '/no.*y/';                     -- PostgreSQL regular expression
SELECT id FROM docs WHERE body @@> 'frendly~1';                   -- fuzzy
SELECT id FROM docs WHERE body @@> '(pool OR spa) AND friendly^3';    -- grouping and boost
SELECT sift.parse_query('Frendly~2 "staff friendly"^2 NOT x*');   -- canonical form; syntax errors carry a position
```

A prefix, wildcard, regex or fuzzy leaf expands to dictionary terms; more than `sift.max_expansions` (512) is an error, not a truncation.
Limits: 256 leaves, nesting depth 32, phrases of 128 tokens, boosts up to 10000.

## Ranking, top-k and pagination

`ORDER BY sift.score(ctid) DESC LIMIT k` runs a ranked scan that skips blocks that cannot reach the top k; ties break by ctid. `OFFSET`
works, but the scan then needs `LIMIT + OFFSET` rows, so deep pages cost more. `sift.score()` and `sift.max_score()` (the best score of the
query, the same on every row) are computed by the scan: use them in the select list or `ORDER BY` of the query level that scans the table,
not in `WHERE` (filter on a score from a subquery).

```sql
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'friendly OR breakfast^3 OR noisy'
ORDER BY sift.score(ctid) DESC LIMIT 2 OFFSET 1;                  -- second and third best
SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM docs WHERE body @@> 'friendly OR pool'
                   ORDER BY s DESC LIMIT 100) top WHERE s > 0.5;  -- filter on the score
SELECT sift.highlight(body, 'friendly staff', '<mark>', '</mark>') FROM docs WHERE id = 2;
EXPLAIN (analyze, costs off, timing off, summary off, buffers off)       -- see "Reading EXPLAIN" below
SELECT id FROM docs WHERE body @@> 'friendly OR breakfast' ORDER BY sift.score(ctid) DESC LIMIT 2;
```

A tie-breaker (`ORDER BY sift.score(ctid) DESC, id`) makes pages fully deterministic, at the cost of reading every row tied with the last
one.

## Filters and joins

Other predicates and joins apply to the rows the sift scan produces; without ranking the planner can also combine the index with btree
indexes in a bitmap scan. Filters are not pushed into the ranked scan (1.1, see [Limits](#limits-and-roadmap)): a very selective filter on
top of a very common term makes the scan rank many candidates to find `LIMIT` survivors.

```sql
CREATE INDEX docs_published ON docs (published);
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'friendly' AND published >= '2026-02-01' AND category = 1
ORDER BY sift.score(ctid) DESC LIMIT 10;
SELECT d.id, c.name FROM docs d JOIN (VALUES (1, 'travel'), (2, 'jobs')) c(id, name) ON c.id = d.category
WHERE d.body @@> 'friendly' ORDER BY sift.score(d.ctid) DESC LIMIT 10;
```

## Analyzers: stemming, folding, languages, stopwords

Text is split into words (letters, digits and combining marks; punctuation and apostrophes separate, so `don't` is `don`, `t`), case folded
(`Straße` is `strasse`), accent folded (`résumé` is `resume`) and optionally stemmed. Han ideographs and hiragana are one token per
character. There is no stopword list: every word is indexed and common words score low. The only stemmer is Snowball English (`stemmer =
'en'`); other languages use `none` and match exact forms (prefixes help). `case_folding` and `accent_folding` (default true) can be switched
off in `WITH (...)`. The analyzer is fixed per index: `ALTER INDEX ... SET` of an analyzer option makes the index unusable (reads and writes
fail) until `REINDEX`, so build the replacement next to the old index and swap:

```sql
SELECT token, pos FROM sift.tokenize('Running Cafés, RÉSUMÉ', stemmer => 'en');             -- run, cafe, resum
SELECT token FROM sift.tokenize('Cafés', accent_folding => false, case_folding => false);   -- Cafés
CREATE INDEX CONCURRENTLY docs_body_en ON docs USING sift (body) WITH (stemmer = 'en');
DROP INDEX docs_body;
ALTER INDEX docs_body_en RENAME TO docs_body;
SELECT id FROM docs WHERE body @@> 'run';                         -- 4: runners, running, runs
```

## Phrases: `phrase_index = pairs`

A phrase is the AND of its words, then a recheck of the candidates in the heap, which is slow for `count(*)` or ranking of common-word
phrases. `phrase_index = pairs` also indexes adjacent word pairs: two-word phrases become exact bitmap ANDs, longer ones are narrowed by
their pairs, scores do not change. The price is index size (5.6x on Wikipedia) and build time.

```sql
CREATE INDEX CONCURRENTLY docs_body_pairs ON docs USING sift (body) WITH (stemmer = 'en', phrase_index = pairs);
DROP INDEX docs_body;
ALTER INDEX docs_body_pairs RENAME TO docs_body;
SELECT count(*) FROM docs WHERE body @@> '"friendly staff"';      -- 2
```

## Several columns, expressions, fields

An index has one key: a `text` or `varchar` column, or an expression of immutable functions returning text. To index several columns
concatenate them and use the same parenthesized expression in the query. `NULL` keys are not indexed, so wrap nullable columns in `coalesce`
(`concat_ws` is not immutable and is rejected).

```sql
CREATE INDEX docs_title_body ON docs USING sift ((coalesce(title, '') || ' ' || coalesce(body, '')));
SELECT id FROM docs WHERE (coalesce(title, '') || ' ' || coalesce(body, '')) @@> 'venue note';
```

For autocomplete, separate sections with `chr(29)`, name the leading ones with `fields` (up to 14) and set `prefix_index = n` (up to 15) so
prefixes of up to n letters are one term lookup instead of a dictionary expansion:

```sql
CREATE TABLE places (id int PRIMARY KEY, name text, admin1 text, country text);
INSERT INTO places VALUES (1, 'Grand Venue', 'Texas', 'US'), (2, 'Granada', 'Andalusia', 'ES'), (3, 'Green Hollow', 'Ohio', 'US');
CREATE INDEX places_sift ON places USING sift ((name || chr(29) || admin1 || ' ' || country))
  WITH (fields = 'name', prefix_index = 3);
SELECT id FROM places WHERE (name || chr(29) || admin1 || ' ' || country) @@> '(gr* ho*) AND (name:{gr* ho*}^3 OR (gr* ho*))'
ORDER BY sift.score(ctid) DESC LIMIT 20;                          -- every word a prefix, name matches boosted x3
```

## Partitioned tables

`CREATE INDEX` on the parent builds one sift index per partition, also for partitions attached later. Matching and `count(*)` span
partitions; `sift.score()` does not work through the partitioned parent (it raises an error), so rank one partition at a time. BM25
statistics are per partition index.

```sql
CREATE TABLE logs (id int, region text, msg text) PARTITION BY LIST (region);
CREATE TABLE logs_eu PARTITION OF logs FOR VALUES IN ('eu');
CREATE TABLE logs_us PARTITION OF logs FOR VALUES IN ('us');
INSERT INTO logs SELECT i, CASE WHEN i % 2 = 0 THEN 'eu' ELSE 'us' END, 'disk full on node ' || i % 10 FROM generate_series(1, 1000) i;
CREATE INDEX logs_msg ON logs USING sift (msg);
SELECT region, count(*) FROM logs WHERE msg @@> 'disk "node 7"' GROUP BY region;
SELECT id, sift.score(ctid) FROM logs_us WHERE msg @@> 'disk "node 7"' ORDER BY sift.score(ctid) DESC LIMIT 3;
```

## Updates, deletes, VACUUM

New and updated rows go to small mutable segments inside the index (appended through generic WAL, no hot spot). At `mutable_limit` (4 MB) a
segment is sealed, and a background worker (`sift.async_maintenance`) turns it into an immutable segment and merges small ones; VACUUM
promotes and merges too, and a writer promotes by itself when the backlog grows. A deleted or updated row stays in the index until VACUUM
and scans hide it through heap visibility; VACUUM clears the dead postings (`dirty` in `sift.segments`) and a merge rewrites them. HOT
updates add no entry.

```sql
INSERT INTO docs VALUES (6, 'Late', 'Late checkout, friendly desk', '2026-06-01', 1);
UPDATE docs SET body = body || ' Parking is free.' WHERE id = 2;
DELETE FROM docs WHERE id = 4;
VACUUM docs;
SELECT kind, ndocs, dirty FROM sift.segments('docs_body');
SELECT sift.promote('docs_body'), sift.merge('docs_body', true);  -- manual maintenance, e.g. from pg_cron
SELECT sift.verify('docs_body', true);                            -- structure check, and the heap re-tokenized and compared
```

## Operations

- **Rebuild:** `REINDEX [CONCURRENTLY]` and `CREATE INDEX CONCURRENTLY` work as usual. `CREATE INDEX` runs in parallel on large tables
  (`max_parallel_maintenance_workers`, memory from `maintenance_work_mem`) and builds the same index as a serial build; `build_segments = N`
  forces N segments and disables the parallel build. Changing `k1`, `b` or an analyzer option needs a REINDEX (`sift.verify` says so).
- **Replicas:** physical standbys replay the index through generic WAL and answer queries (a build under `wal_level = minimal` is tested
  too). Set `hot_standby_feedback = on`, otherwise a long standby query can fail with a retryable serialization error ("segment recycled
  during scan"). Logical replication does not carry indexes; build one on the subscriber.
- **pg_dump** writes `CREATE INDEX ... USING sift`; a restore rebuilds the index and needs pg_sift installed. **pg_upgrade** within one
  major version carries the index files over (tested); across major versions it is unverified.
- **Inspect:** `sift.segments(index)` (kind, `ndocs`, `avgdl`, `bytes`, `dirty`, `nterms`), `sift.segment_postings(index)` (postings and
  bits per posting by document-frequency class), `sift.verify(index [, heapallindexed])`, `pg_relation_size`.
- **Reading EXPLAIN** (the last query of the ranking section): `Custom Scan (SiftScan)` is a sift scan (`Order: score DESC` ranked, `Limit Hint` the k it got); `SiftCount` is the
  `count(*)` pushdown (one table, only `count(*)`, every condition a sift match; otherwise `Aggregate` over `SiftScan`). With `ANALYZE`:
  `Superblocks` (256-page ranges read), `Blocks Scored` / `Blocks Skipped` (16-page blocks scored or pruned), `Candidates` produced, `Heap
  Checks` (visibility checks in the heap, 0 for `count(*)` on a vacuumed table). `Bitmap Index Scan` appears when the planner combines the
  index with others; `sift.enable_scan = off` forces it.

| GUC | default | meaning |
|---|---|---|
| `sift.max_expansions` | 512 | terms a prefix, wildcard, regex or fuzzy leaf may expand to (error above) |
| `sift.enable_scan`, `sift.enable_count` | on | use SiftScan, use the `count(*)` pushdown |
| `sift.prefetch` | on | read ahead the heap pages of an unordered scan |
| `sift.async_maintenance` | on | superuser; promote and merge in a background worker |
| `sift.simd` | auto | cap the SIMD level: `avx512`, `avx2`, `scalar` |

| reloption | default | meaning |
|---|---|---|
| `k1`, `b` | 1.2, 0.75 | BM25 saturation (0-1000) and length normalization (0-1); REINDEX |
| `stemmer` | `none` | `en` or `none`; REINDEX |
| `case_folding`, `accent_folding` | true | fold case, fold accents; REINDEX |
| `phrase_index`, `prefix_index`, `fields` | `none`, 0, none | `pairs`; prefix length 0-15; section names; REINDEX |
| `mutable_limit` | 4096 | kB of a mutable segment before it is sealed (64-1048576); no REINDEX |
| `build_segments` | 0 | force N segments in `CREATE INDEX` (disables the parallel build) |

## Limits and roadmap

- PostgreSQL 18 only; heap tables only; BLCKSZ 8192 only; database encoding UTF8 or SQL_ASCII.
- One key per index: multicolumn, `INCLUDE` and unique indexes and non-text keys are rejected. No plain or index-only scans.
- Terms are truncated at 255 bytes, `tf` saturates at 255 and document length at 65535; at most 128 segments per index.
- Phrases are rechecked in the heap unless the index uses `phrase_index = pairs`.
- `sift.score()` is not available through a partitioned parent table; rank each partition.
- Only the 1.0 install script exists; there is no `ALTER EXTENSION UPDATE` path yet.
- Parallel scan was measured and dropped. **Roadmap (1.1):** filter-mask pushdown (exact filtered top-k) and keyword facets.

## Facets

`analyzer = keyword` indexes each whole value as one term (folded like a word unless `case_folding` /
`accent_folding` are off). `sift.facets(index, facet_index, query, lim, nulls)` counts the visible matches of a
query on `index` per value of `facet_index`, from the bitmaps, without reading the heap beyond what `count(*)` reads:

```sql
CREATE INDEX docs_category ON docs USING sift (category) WITH (analyzer = keyword);

SELECT * FROM sift.facets('docs_body', 'docs_category', 'pool OR spa', 10);
-- the rows of: SELECT category, count(*) FROM docs WHERE body @@> 'pool OR spa'
--              GROUP BY category ORDER BY 2 DESC, category COLLATE "C" LIMIT 10
```

`lim` NULL returns every value; `nulls => true` adds a row with a NULL value for the matches without one (empty
and NULL values have no term). The counts equal the `GROUP BY` of the same snapshot, after DELETE, UPDATE and
VACUUM. Values are at most 255 bytes (longer ones are an error), one column per keyword index, no `text[]`
elements, and tables with row-level security are refused. The cost is linear in the number of distinct values plus
the matches, and the heap is not read: it beats `GROUP BY` for broad queries over columns with tens of values
(4.5x on 25,000 articles, a single common word) and loses for selective queries or thousands of values
.

## Correctness and testing

`make check` runs every suite in a throwaway cluster in about 2.5 minutes; `make check-full` adds the ASan/UBSan build, long soak and fuzz
runs and all mutants (about 22 minutes); `make installcheck` runs the regression tests against a server.

- pg_regress tests compare every query shape with a sequential scan and a SQL reference BM25 (scores bit-equal); a randomized differential
  test mutates the corpus (inserts, updates, deletes, VACUUM, merge, REINDEX) and compares the index with the reference, and 16 injected
  bugs (mutants) must all be caught.
- Isolation specs (insert during seal, VACUUM and merge during scans, CIC) and TAP tests: `kill -9` during build, promotion, merge and
  VACUUM, a streaming standby, `wal_level = minimal`, extension lifecycle and `pg_upgrade`.
- Fuzzers for the query parser, analyzer and codec under ASan/UBSan; a soak test at 1,000 updates per second with `sift.verify` checks;
  `bench/bench check oracle ENGINE` compares match sets with a tsvector oracle.

## Security, contributing, license

Report crashes, wrong results and index corruption privately as described in [SECURITY.md](SECURITY.md). To build, test and send changes see
[CONTRIBUTING.md](CONTRIBUTING.md). pg_sift is released under the [PostgreSQL License](LICENSE); third-party notices are in
[NOTICE](NOTICE).
