# Changelog

## Unreleased

- Packages: a `.deb` (`postgresql-18-pg-sift`, PGDG on Debian 13 and Ubuntu 24.04) and an `.rpm` (`pg_sift_18`, PGDG on RHEL, Rocky and
  Alma 9 and 10), x86-64, are attached to each release with `SHA256SUMS`. `scripts/package.sh` builds them from a tag and smoke-tests
  them in containers (install, create, upgrade from 1.0, remove); the `release` workflow runs it on every `v*` tag.

## 1.1.0

- Filter pushdown: a ranked scan can take a bitmap of the other quals (btree, BRIN, GIN) as a child and score only the matches it keeps;
  `sift.filter_pushdown = off | on | force`, default `on`; EXPLAIN shows `Filter Superblocks`, `Filter Pages Exact`, `Filter Pages Lossy`.
- Keyword facets: `analyzer = keyword` indexes each whole value as one term; `sift.facets(index, facet_index, query, lim, nulls)` counts
  the matches of a query per value.
- Parallel scan: parallel-aware SiftScan and SiftCount with a shared ranked threshold, behind `sift.parallel_scan` (default off). It is
  never planned together with a pushed-down filter.
- Partitioned tables: `ORDER BY sift.score(ctid) DESC LIMIT k` through a partitioned parent ranks each partition with a ranked scan under a
  MergeAppend (scores use each partition's own statistics); `sift.max_score()` is not available through a parent.
- Fix: a crash rechecking a phrase with `count(*)` or a ranked scan on an expression index (also affects 1.0.0).
- Upgrade from 1.0: install the new files and run `ALTER EXTENSION pg_sift UPDATE`.

## 1.0.0

- First release: the `sift` index access method (`USING sift`), `text @@> text` queries (boolean, phrase, prefix, wildcard, regex, fuzzy),
  exact BM25 ranking with `sift.score()`, `count(*)` from the index, mutable segments with background maintenance, parallel `CREATE INDEX`,
  generic WAL and replica support.
