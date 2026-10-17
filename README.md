# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml)
[![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml)
[![release](https://img.shields.io/github/v/release/brauliobo/pg_sift)](https://github.com/brauliobo/pg_sift/releases/latest)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018-336791)](#install)
[![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

Fast full-text search with BM25 ranking inside PostgreSQL. pg_sift is an index type (`USING sift`) that answers boolean, phrase, prefix,
wildcard, regex and fuzzy queries, ranks results with exact BM25 and counts matches straight from the index. It needs no external search
server and no `shared_preload_libraries`.

- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, partial and expression indexes, crash and replica safe (generic WAL).
- **Built for search pages:** top-k ranking, filters pushed into the ranked scan, facet counts, autocomplete with field boosts.

## Install

Requires PostgreSQL 17 or 18 (19 is experimental), server headers with `pg_config` on `PATH`, and a C compiler. Tested on x86-64.

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install     # PG_CONFIG=/path/to/pg_config picks another installation
```

- Arch Linux: AUR `pg_sift` (recipe in [packaging/](packaging/)).
- Debian 13, Ubuntu 24.04 with PGDG `postgresql-18`: download the `.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest)
  and `sudo apt install ./postgresql-18-pg-sift_1.3.1-1_amd64.deb`.
- RHEL, Rocky, Alma 9 or 10 with PGDG `postgresql18-server`:
  `sudo dnf install https://github.com/brauliobo/pg_sift/releases/download/v1.3.1/pg_sift_18-1.3.1-1.el9.x86_64.rpm`.

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

## More

[docs/USAGE.md](docs/USAGE.md) covers the rest, each with examples:

- analyzers (stemming, accent folding), phrases and the `positions` option, several columns;
- autocomplete: prefix indexes, `fields` and field boosts;
- filter pushdown, partitioned tables, keyword facets, parallel scans;
- maintenance, settings and index options, replication, limits.

A proof of concept of the same engine for MariaDB (UDFs over a queue
table, no server patches): [mariadb/README.md](mariadb/README.md).

## pg_sift for SQLite

The same search engine is also a SQLite loadable extension, `sift` for SQLite (release asset `sift-sqlite-<version>-linux-x86_64.tar.gz`): a
separate component with its own install, documentation and changelog, see [sqlite/README.md](sqlite/README.md).

## Security and license

`make check` runs every suite in a throwaway cluster (about 2.5 minutes; `make check-full` adds sanitizers, soak, fuzz and mutants).
Report crashes, wrong results and index corruption privately as described in [SECURITY.md](SECURITY.md); to contribute see
[CONTRIBUTING.md](CONTRIBUTING.md). Changes per release: [CHANGELOG.md](CHANGELOG.md). pg_sift is released under the
[PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
