# Changelog

## Unreleased

- Word positions are stored by default (reloption `positions`, default on): phrase queries are decided from the index instead of
  re-tokenizing the heap text of each candidate. A phrase that every match must contain (not under `OR` or `NOT`) is verified in the
  index, and a phrase under `OR` (a CJK word becomes one) filters its candidates the same way; counts and ranked scans of such phrases
  read no heap tuple. On the 218k-article benchmark phrase queries run about 4.1x faster than with `positions = off` and 3.3x faster than pg_search,
  counts 7.3x faster. The price is a default index about 2.5x larger (52.5 MiB to 130.9 MiB on the 218k articles, pg_search: 183.6 MiB) and a
  build about 30% slower (1.7 to 2.2 s): `positions = off` restores the small index and the heap recheck of phrases. Indexes built by earlier
  versions keep working but have no positions: `REINDEX` them to get phrases decided by the index. Terms with at most two postings
  keep their positions in the dictionary entry (up to 256 bytes). Documents inserted after the build (mutable segments, and the
  segments promoted and merged from them) and phrases under `NOT` keep the heap recheck.
- On-disk format 1.1 (metapage minor version): indexes created or reindexed by this version are 1.1 and must not be opened by earlier
  releases, which write 1.0 and do not check the minor version (`REINDEX` the index there to go back). Format 1.0 indexes stay readable and work without positions until
  `REINDEX`. A build that meets a newer minor version than it knows refuses the index with "REINDEX the index".
- Counts and filters: a query with a phrase rechecks in the heap only the candidates the bitmaps and positions do not decide (an
  `OR` branch without the phrase, a `NOT` of a superset and every segment with positions are not rechecked); `count(*)` counts the
  all-visible pages of a group straight from its bitmaps; the planner no longer overcharges a pushed-down filter, so a ranked scan
  takes it when it is cheaper (a ranked `OR` with a 2% filter 6.0 -> 4.8 ms, plan choice wrong for 2 of 100 queries instead of 70);
  terms from 8K postings (was 64K) are bitmap groups with block bounds: ranked `AND` and `OR` about 20% faster and `count(*)` of
  `OR`s about 27% faster for about 10% more index (applies to new builds and `REINDEX`).
- Top-k: ranked blocks accumulate leaves by descending upper bound and score only the documents that can still reach the threshold
  (new `bm25_sel` and `prune_gt` kernels in AVX-512, AVX2 and scalar), and the bounds of dense LIST terms are read without decoding
  their postings. Scores and ties are bit-identical. Ranked `AND` and `OR` use about 27% fewer cycles, prefix top-k and boosted
  place queries 8-34% fewer.
- Fix: `sift.score()` through joins. A ranked scan under a join tree of depth two or more (a join plus a semi join, a lateral join, outer, anti and self joins, aggregates over a join) raised "must be computed by a sift scan" although the plan scanned the table with a ranked SiftScan: the joins now pass the scan's score on, also from a partitioned or inheritance parent. Regression `rank_join`.
- Prefix queries never fail on the expansion limit: a prefix with more than `sift.max_expansions` terms (`h*` or `ho*` on a large
  table without `prefix_index`) matches all the words that start with it and scores as one term, the same score `prefix_index` gives it
  (below the limit a prefix still scores as the max over its terms). `prefix_index` is now a speed and size choice, not a requirement
  (a million names: 10-15 ms for `h*` the first time in a backend, under 1 ms cached, `sift.prefix_cache_size`, against under 1 ms and
  a 2-4x larger index with it). Wildcard, regex and fuzzy leaves still raise the error above the limit. No index format change.
- PostgreSQL 17 support, with 19 (release candidate) experimental: `make` accepts 17 and newer, and the version differences live in
  `src/sift_compat.h`. PostgreSQL 17 has no `extension_control_path`, so `scripts/pgtest.sh install` copies sift into the server's own
  directories there (`SIFT_SUDO=sudo` when they are not writable) and the update-path checks of the lifecycle test are skipped. CI
  runs 17 and 18, and 19 built from git without failing the workflow.
- Fix: `name:(a b)` and `name:{a b}` lost their qualifier when the group was unboosted and the same operator as its parent (the leaves
  searched the whole text, on the index and on a seq scan alike), and `name:(NOT a)` dropped it too. Without `fields` a colon is text
  in groups as well (no more "unknown field (the index has none)"); a name an index with fields lacks lists its fields.
- Fix: with several sift indexes on one key, an index with another analyzer (one with `fields` next to a plain one) could serve `@@>`, so
  a query answered differently depending on the cheapest plan, or failed while the planner costed the index. `@@>` now means one analyzer
  (the default one when an index has it) and only indexes that answer the same serve it.
- Fix: a ranked scan with a broad match and a selective filter on a column that follows the heap order (an id range or id list at the
  end of the table) was planned as filter-after-ranker, which verifies every match before the filter's first row (tens to hundreds of
  ms against under 1 ms pushed down). The post-filter estimate now counts the matches before and after the rows the filter's btree
  quals can keep, from the column's histogram and correlation, and the bitmap's tuple count no longer includes the planner's lossy-page
  inflation. The estimate also prices the blocks the ranker scores (a broad term ties its superblock bounds, so the scan runs in heap
  order) and the descents of a `= ANY(const array)` bitmap by the leaf pages they touch rather than one random page per element, so
  1000 to 10000 scattered ids with a broad term are pushed down (418 ms against 5 ms in the report) and ids at the head of the heap
  stay on the plain scan only when it is faster.
- Fix: assertion failures on a server built with `--enable-cassert` (a stray `foreach_node(Node/Path/Plan)` cast, the GlobalVis horizon
  taken from the index instead of its heap, and a page special area read before the page layout check in `sift.verify`).
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
