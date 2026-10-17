# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml)
[![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml)
[![release](https://img.shields.io/github/v/release/brauliobo/pg_sift)](https://github.com/brauliobo/pg_sift/releases/latest)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018-336791)](#install)
[![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

Fast full-text search with BM25 ranking inside PostgreSQL. pg_sift is an index type (`USING sift`) that answers boolean, phrase, prefix,
wildcard, regex and fuzzy queries, ranks results with exact BM25 and counts matches straight from the index. It needs no external search
server and no `shared_preload_libraries`.

- **Fast:** ahead of ParadeDB pg_search on every query class we measured, 2.9x to 8.9x on 218,058 Wikipedia articles ([benchmarks](#benchmarks)).
- **Small:** 131 MiB for those articles with phrase support, 1.4x smaller than pg_search; 52 MiB without word positions.
- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, partial and expression indexes, crash and replica safe (generic WAL).
- **Built for search pages:** top-k ranking, filters pushed into the ranked scan, facet counts, autocomplete with field boosts.

## Install

Requires PostgreSQL 17 or 18 (19 is experimental), server headers with `pg_config` on `PATH`, and a C compiler. Tested on x86-64.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
```

- Arch Linux: AUR `pg_sift` (recipe in [packaging/](packaging/)).
- Debian 13, Ubuntu 24.04 with PGDG `postgresql-18`: download the `.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest)
  and `sudo apt install ./postgresql-18-pg-sift_1.2.0-1_amd64.deb`.
- RHEL, Rocky, Alma 9 or 10 with PGDG `postgresql18-server`:
  `sudo dnf install https://github.com/brauliobo/pg_sift/releases/download/v1.2.0/pg_sift_18-1.2.0-1.el9.x86_64.rpm`.

Then, as a superuser (after installing a newer version run `ALTER EXTENSION pg_sift UPDATE`):

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

## More

[docs/USAGE.md](docs/USAGE.md) covers the rest, each with examples:

- analyzers (stemming, accent folding), phrases and the `positions` option, several columns;
- autocomplete: prefix indexes, `fields` and field boosts;
- filter pushdown, partitioned tables, keyword facets, parallel scans;
- maintenance, settings and index options, replication, limits.

Design and on-disk format: [DESIGN.md](DESIGN.md). A proof of concept of the same engine for MariaDB (UDFs over a queue
table, no server patches): [mariadb/README.md](mariadb/README.md).

## Benchmarks

Simple English Wikipedia (218,058 articles, 8 clients) and GeoNames cities500 (236,262 places, autocomplete, 1 client), same data, queries and
CPU budget for every engine, median of 2-3 interleaved rounds on a shared host. Cells are queries per second relative to pg_search 0.26.1.

| articles | index | build | and | or | phrase | `count(*)` | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| **sift** (default) | 131 MiB | 2.2 s | 3.8x | 2.9x | 3.3x | 4.7x | 8.9x |
| sift `positions = off` | 52 MiB | 1.7 s | 4.0x | 3.1x | 0.79x | 0.64x | 4.5x |
| pg_search | 184 MiB | 3.1 s | 1x | 1x | 1x | 1x | 1x |
| pg_textsearch 1.5.1 | 147 MiB | 10.0 s | 0.002x | 2.7x | 0.011x | 0.010x | 0.076x |
| rum 1.3.15 | 310 MiB | 14.9 s | 0.36x | 0.009x | 0.014x | 0.026x | 0.071x |
| pgx-bm25 1.0 | 204 MiB | 11.7 s | 0.011x | 0.025x | 0.007x | 0.012x | 0.098x |

- Autocomplete on places: sift answers the top 20 in 0.16-0.27 ms in every query class, pg_search in 0.85-2.7 ms; sift is ahead in all 45 warm cells (1.4x to 30x).
- `positions = off` trades phrase and `count(*)` speed for an index 3.5x smaller than pg_search's.
- Prefix ranking agrees less with a BM25-over-n-grams oracle than pg_search's does (top-20 overlap 0.83 against 0.98, and 0.46 for one-letter
  prefixes), because sift scores a prefix as the best word it expands to.
- The host was shared, so absolute numbers are noisy and a 3.8x is not distinguishable from 3.0x or 4.5x; every cell with its standard error, the
  cold-cache tables and the method are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

![Throughput relative to pg_search](docs/img/articles-speedup.svg)
![Index size](docs/img/index-size.svg)
![Autocomplete latency](docs/img/places-latency.svg)

Reproduce with `bench/bench all` (see [bench/README.md](bench/README.md); about 2 minutes reduced, about 50 minutes for the published numbers);
`bench/bench regress --run` compares this checkout with an earlier release and exits non-zero on a regression.

## Testing, security, license

`make check` runs every suite in a throwaway cluster (about 2.5 minutes; `make check-full` adds sanitizers, soak, fuzz and mutants).
Report crashes, wrong results and index corruption privately as described in [SECURITY.md](SECURITY.md); to contribute see
[CONTRIBUTING.md](CONTRIBUTING.md). Changes per release: [CHANGELOG.md](CHANGELOG.md). pg_sift is released under the
[PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
