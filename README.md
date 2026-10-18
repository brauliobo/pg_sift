# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml)
[![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml)
[![release](https://img.shields.io/github/v/release/brauliobo/pg_sift)](https://github.com/brauliobo/pg_sift/releases/latest)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018-336791)](#install)
[![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

Fast full-text search with BM25 ranking inside PostgreSQL. pg_sift is an index type (`USING sift`) that answers boolean, phrase, prefix,
wildcard, regex and fuzzy queries, ranks results with exact BM25 and counts matches straight from the index. It needs no external search
server and no `shared_preload_libraries`.

The same engine also runs [as a SQLite extension](#pg_sift-for-sqlite) and [as a MariaDB UDF library](#mariadb-bm25-search-as-a-udf-library).

- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, partial and expression indexes, crash and replica safe (generic WAL).
- **Built for search pages:** top-k ranking, filters pushed into the ranked scan, facet counts, autocomplete with field boosts.

## Install

Requires PostgreSQL 17 or 18 (19 is experimental), server headers with `pg_config` on `PATH`, and a C compiler. Tested on x86-64.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
```

- Arch Linux: AUR `pg_sift` (recipe in [packaging/](packaging/)).
- Debian 13, Ubuntu 24.04 with PGDG `postgresql-18`: download the `.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest)
  and `sudo apt install ./postgresql-18-pg-sift_1.4.0-1_amd64.deb`.
- RHEL, Rocky, Alma 9 or 10 with PGDG `postgresql18-server`:
  `sudo dnf install https://github.com/brauliobo/pg_sift/releases/download/v1.4.0/pg_sift_18-1.4.0-1.el9.x86_64.rpm`.

Then, as a superuser (after installing a newer version run `ALTER EXTENSION pg_sift UPDATE`):

```sql
CREATE EXTENSION pg_sift;
```

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

`text @@> text` matches a document against a query; without an index it runs per row with the same results, only slower.

## Query language

Words are `AND`ed; `OR`, `NOT` (upper case), parentheses, quotes and a few operators combine them:

```sql
SELECT id FROM docs WHERE body @@> 'breakfast NOT noisy';             -- exclusion: 2
SELECT id FROM docs WHERE body @@> '"friendly staff"';                -- phrase: 2, 5
SELECT id FROM docs WHERE body @@> '{pool spa noisy}';                -- any of a set of words
SELECT id FROM docs WHERE body @@> 'brea* OR n?isy OR /no.*y/ OR frendly~1';  -- prefix, wildcard, regex, fuzzy
SELECT id FROM docs WHERE body @@> '(pool OR spa) AND friendly^3';    -- grouping and boost
```

## Ranking and filters

`ORDER BY sift.score(ctid) DESC LIMIT k` runs a ranked scan that skips the blocks that cannot reach the top k. Other conditions of the
query (`published >= ...`, joins) are applied inside that scan when the planner finds it cheaper, so a selective filter stays fast even
with a very common search term:

```sql
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'friendly' AND published >= '2026-02-01'
ORDER BY sift.score(ctid) DESC LIMIT 10;
```

## Configuration

Index options go in `WITH (...)` at `CREATE INDEX`; `mutable_limit` and `vacuum_segments` can be changed afterwards (`ALTER INDEX ... SET (...)`), the rest need `REINDEX`.

| Index option | default | range | meaning |
|---|---|---|---|
| `k1` | 1.2 | 0-1000 | BM25 term-frequency saturation |
| `b` | 0.75 | 0-1 | BM25 length normalization |
| `analyzer` | `text` | `text`, `keyword` | tokenize words, or index the whole value as one term |
| `stemmer` | none | `none`, `en` | Snowball English stemming |
| `case_folding`, `accent_folding` | true |  | fold case / accents |
| `positions` | true |  | store positions (needed for phrase queries) |
| `prefix_index` | 0 | 0-15 | prefix lengths indexed for fast autocomplete |
| `fields` |  |  | columns weighted as separate fields (see USAGE) |
| `mutable_limit` | 4096 | 64-1048576 kB | size of the write lane before it is sealed into a segment; also the base size of the merge tiers |
| `build_segments` | 0 | 0-64 | force N segments at build and disable the parallel build |
| `vacuum_segments` | 2 | 1-128 | segments VACUUM leaves the index with; more keeps VACUUM cheaper and queries slightly slower |

| Setting (GUC) | default | meaning |
|---|---|---|
| `sift.async_maintenance` | on | background worker seals and merges segments (superuser); `off` leaves it to inserts and VACUUM |
| `sift.enable_scan`, `sift.enable_count` | on | allow the ranked `SiftScan` / `count(*)` `SiftCount` plans |
| `sift.maintenance_naptime` | 2000 ms | `postgresql.conf` only, 200-60000: how long the worker stays after it starts to promote the seals of a bulk load; holds one worker slot per loaded index for that time |
| `sift.parallel_scan` | off | let the planner add parallel SiftScan / SiftCount plans |
| `sift.prefetch` | on | read ahead the heap pages an unordered scan will visit |
| `sift.filter_pushdown` | on | `off`, `on`, `force`: give a ranked scan a bitmap of the other quals |
| `sift.max_expansions` | 512 | superuser: terms a wildcard, regex, fuzzy or prefix leaf scores individually |
| `sift.prefix_cache_size` | 8MB | superuser: per-backend memory for the postings of broad leaves |
| `sift.log_min_duration` | 1s | log promotions, merges and worker runs at least this long (`-1` none) |

Merging is automatic: sealed segments are merged 8 at a time per size tier, the directory (128 entries) is kept under 32 by merging the
8 smallest, inserts wait for the worker at 112 entries, and an insert seals the lane itself when 4 segments are waiting. VACUUM merges
down to `vacuum_segments`, except that a segment more than 8 times larger than the ones before it is left alone (`sift.merge(index, true)` merges
it anyway). These thresholds are constants today; `mutable_limit` is the knob that scales them. Coming with 2.0: `merge_fanin`, `dead_rewrite_pct`
(segment rewrite threshold, 50 today, as a reloption) and `sift.maintenance_cost_delay`.
Full reference, inspection functions and replication: [docs/USAGE.md](docs/USAGE.md).

## More

[docs/USAGE.md](docs/USAGE.md) covers the rest, each with examples:

- analyzers (stemming, accent folding), phrases and the `positions` option, several columns;
- autocomplete: prefix indexes, `fields` and field boosts;
- filter pushdown, partitioned tables, keyword facets, parallel scans;
- maintenance, settings and index options, replication, limits; sizing a large index: [docs/SIZING.md](docs/SIZING.md).

A proof of concept of the same engine for MariaDB (UDFs over a queue
table, no server patches): [mariadb/README.md](mariadb/README.md).

## pg_sift for SQLite

The same engine as a SQLite loadable extension (`sift`, Linux x86-64, SQLite 3.38 or newer) for apps that want search without a server: a
virtual table with exact BM25 and block-max top-k, phrase, prefix, wildcard, regex and fuzzy queries, highlights, snippets and facets, a
bounded page cache, and an external-content mode that indexes a table you already have. Full documentation: [sqlite/README.md](sqlite/README.md).

```sh
V=1.4.0   # or: git clone https://github.com/brauliobo/pg_sift && make -C pg_sift/sqlite   (builds sqlite/build/sift.so)
curl -LO https://github.com/brauliobo/pg_sift/releases/download/v$V/sift-sqlite-$V-linux-x86_64.tar.gz
curl -LO https://github.com/brauliobo/pg_sift/releases/download/v$V/SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS && tar xzf sift-sqlite-$V-linux-x86_64.tar.gz     # sift.so, load it with .load ./sift
```

```sql
.load ./sift
CREATE VIRTUAL TABLE docs USING sift(title, body);
INSERT INTO docs(rowid, title, body) VALUES (1, 'Venue note', 'Breakfast was great and the staff were friendly. The pool was noisy.'),
  (2, 'Cafe', 'Friendly staff, no breakfast, but a quiet pool and spa.'), (5, 'Noisy venue', 'The venue was noisy at night. Friendly staff though.');

SELECT rowid FROM docs WHERE docs MATCH 'breakfast friendly';                          -- all terms: 1, 2
SELECT rowid, rank FROM docs WHERE docs MATCH 'pool OR spa' ORDER BY rank LIMIT 10;    -- BM25 top-10, best first: 2, 1
SELECT sift_count('docs', 'friendly');                                                 -- 3, from the index
SELECT sift_highlight(docs, 1, '<b>', '</b>') FROM docs WHERE docs MATCH 'spa';        -- Friendly staff, ... pool and <b>spa</b>.
```

Against SQLite FTS5 (`porter unicode61 remove_diacritics 2`, `bm25` rank), 218,058 Simple English Wikipedia articles, one thread, warm cache, mean of
2 runs of 200 queries per class, measured 2026-10-17 on an AVX-512 host:

|  | sift | FTS5 | sift vs FTS5 |
|---|---|---|---|
| index, with positions / without | 130.8 MB / 46.9 MB | 125.1 MB / 35.0 MB | 1.05x / 1.34x larger |
| build (218k docs) | 12.0 s | 12.4 s | equal |
| AND / OR / phrase, top 10 by BM25 | 5746 / 1209 / 4019 qps | 64.6 / 5.0 / 400 qps | 89x / 242x / 10x |
| count AND / OR / phrase | 15.3k / 10.2k / 2.6k qps | 795 / 54 / 769 qps | 19x / 189x / 3.4x |

Method, the scalar-kernel column and the cache-size effect: [sqlite/README.md](sqlite/README.md#numbers-against-fts5) and
These are SQLite numbers; they come from a different system than the PostgreSQL
benchmarks above and are not comparable with them.

## MariaDB: BM25 search as a UDF library

`mariadb/` brings the pg_sift engine to MariaDB 10.5 to 13.x as `libsift_udf.so` plus two SQL files (release tarballs carry a tested
library for 10.11, 11.4, 11.8, 12.1 and 13.0): ranked top-k, counts, phrases, prefix / wildcard / fuzzy / regular expression terms,
boosts, fields, filters and facets, on InnoDB tables, with results (counts, top-10 order, scores) identical to pg_sift. It is the portable
core (`src/portable.list`) compiled unmodified over a small host in `mariadb/host/`. No storage engine, no server patch: triggers feed a
queue, the index lives in files next to the data directory, queries see writes at once. Full guide: [mariadb/README.md](mariadb/README.md).

```sh
nice make -C mariadb -j4 OUT=/tmp/sift-build               # needs the MariaDB server headers (libmariadbd-dev), no PostgreSQL
cp /tmp/sift-build/libsift_udf.so "$(mariadb -NBe 'SELECT @@plugin_dir')"/     # or lib/<series>/ of a release tarball
mariadb mydb < mariadb/sql/functions.sql && mariadb mydb < mariadb/sql/sift.sql
```

```sql
CALL sift_attach('news', 'articles', 'id', "CONCAT_WS(' ', t.title, t.body)", 32);   -- index, queue, triggers
SELECT a.id, a.title, j.s
FROM (SELECT sift_topk('news', 'solar OR "power plant"', 10) AS r) x,
     JSON_TABLE(x.r, '$[*]' COLUMNS (n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')) j
JOIN articles a ON a.id = j.id ORDER BY j.n;
SELECT sift_count('news', 'power AND plant');          CALL sift_sync('news');   -- drain the queue into a segment
```

| class | sift | InnoDB FULLTEXT | InnoDB / sift |
| --- | --- | --- | --- |
| AND top-10 | 0.48 ms | 87 ms | 183x |
| OR top-10 | 1.30 ms | 944 to 1,033 ms | 725x to 794x |
| phrase top-10 | 0.42 ms | 214 ms | 504x |
| count AND / OR | 0.11 / 0.27 ms | 113 / 1,506 ms | 1,025x / 5,608x |
| build | 6.6 s rebuild, 10.9 s first drain | 58.9 s | 8.9x, 5.4x |
| index size | 135.9 MB | 232.0 MB | 1.7x smaller |

Writes are visible to the next query by default; that costs 70 to 180 us per top-10 (20 to 30 us per count), and an index created with
`fresh=false, recheck=false` skips it. Moving the engine onto the shared core changed nothing measurable: interleaved before / after
rounds put every cell within the noise (mean ratio 0.97), the index is the same size, and the 5,098-query comparison with pg_sift has 0
differences. MariaDB, SQLite and PostgreSQL numbers are measured on different systems and are not comparable with one another.

## Security and license

`make check` runs every suite in throwaway clusters, in parallel lanes (about 1.5 minutes on 8 cores, 2.5 minutes on the 4 cores of a CI runner;
`make check-full` adds sanitizers, soak, fuzz and mutants).
Report crashes, wrong results and index corruption privately as described in [SECURITY.md](SECURITY.md); to contribute see
[CONTRIBUTING.md](CONTRIBUTING.md). Changes per release: [CHANGELOG.md](CHANGELOG.md). pg_sift is released under the
[PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
