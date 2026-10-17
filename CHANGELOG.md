# Changelog

This file is the changelog of the PostgreSQL extension. The SQLite extension (`sift` for SQLite, artifact `sift-sqlite`) has its own: [sqlite/CHANGELOG.md](sqlite/CHANGELOG.md).

## Unreleased

- Planner: the estimate of a query is computed once per statement instead of three or four times (the selectivity of the clause, the cost of each
  path, the index path). The last 8 estimates of a backend are kept under the index file, the version of its directory, the expansion cap and the
  query; a promotion, a merge, VACUUM or REINDEX changes the key. Slim tier, one backend, user cycles per statement: count 1.18M to 1.03M
  (-12%), filter -9%, mixed -8%, or and join unchanged; count at 8 clients 25.9k to 29.3k qps.
- Ranker: a join (or any consumer) that keeps a small share of the rows and pulls past the limit hint makes the scan search again for 16 times
  as many hits (4 times beyond 4096) instead of 4 times, so a join that keeps a tenth of the rows searches twice instead of three times. Rows,
  scores and ties are those of the full ranking. Slim tier join class, one backend, user cycles per query: 5.16M to 4.39M (-15%); at 8 clients
  4.78k to 6.03k qps (+26%).
- Planner: a prepared statement (a generic plan, whose query the planner cannot read) now gets the filter pushdown like the literal query.
  The ranked scan priced such a query by the operator's default of 0.5% of the rows, which made a filter after the ranker look free; it now
  assumes a tenth of the rows, so a selective filter is pushed down and a loose one stays after the ranker. Rows, scores and ties are
  unchanged. Slim tier, one backend, user cycles per query: prepared filter class 4.36M to 2.30M (the literal query takes 2.33M), prepared
  join class 6.67M to 5.28M, prepared or and mixed classes unchanged.
- Planner: the filter pushdown is chosen for a filter on a value stored in one late stretch of the heap. The planner placed the rows of
  an equality filter on a btree column from the column's statistics, which say nothing about rows loaded by group (correlation near
  zero), and priced the scan that checks the filter after the ranker as if they were spread over the heap; it now reads where the
  entries of the key lie from the btree (a few entries, at planning time) and verifies every match before them. A filter at the start
  of the heap keeps the post-filter scan, so no plan that was fast changes. Slim places, one backend, user cycles per query: filtered
  one-letter prefix (p1) 8.74M to 1.26M, two-letter prefix (p2) 1.55M to 0.94M; the other places and article shapes are unchanged
  within noise. Rows, scores and ties are unchanged.

## 1.3.1

- Fix: `CREATE INDEX` failed with `sift parallel build worker needs more than 256 runs` (`HINT: Increase maintenance_work_mem or build
  without parallel workers`) when a parallel worker spilled more than 256 runs, i.e. on a large table with a `maintenance_work_mem` small for
  it; a build of enough runs for more than 128 segments failed with `sift index ... would need more than 128 segments`. The build now
  merges runs in passes (the last fan-in runs of a level into one of the next, a fan-in bounded by the memory budget), so any table builds
  at any `maintenance_work_mem` (down to 1MB), in parallel or not: a small budget only makes the build slower, and when it would need more
  than 128 segments the build writes fewer, larger ones. The index answers and scores as one built with more memory. Builds at the default
  or a generous budget take as long as before; at 1MB they take up to 1.5x longer (second merge pass), where they used to fail or exceed
  the budget. `sift` logs the runs merged into each segment at `debug2`, and `scripts/build-bench.sh` compares build time, memory and temporary
  files of two builds at several budgets.

## 1.3.0

- Planner: `@@>` on a key whose index has `fields` (or any analyzer other than the default) is estimated from the index like any other. It is
  the call `sift.match_idx`, which had no estimate, so the planner expected a third of the rows for every query (a term of 44 of 1M names as
  a third of them) and costed the scan and the joins above it as for a broad term. A rare term of such an index, searched among the
  properties of a tenant with 10% of the names (`id IN (SELECT ...)` through a left join that repeats them): hash join 2.6 s, now a
  nested loop semi join under the ranked scan, 18 ms.
- Planner: a ranked scan with a `LIMIT` is costed for what it does when drained. Past its limit hint it ranks again for four times as many
  rows, to the last match, as a hash join or a sort above it makes it; its total cost was its startup plus a hundredth per row, so hash
  plans over the whole scan looked far cheaper than they are and won against the nested loop semi join that stops at the limit. 1M names,
  152k matches of `hotel`, a tenant with 0.3% of the names: 190 ms, now 68 ms (`h*`: 247 to 69 ms; a rare term 9.1 to 1.6 ms); tenants
  with 2% to 50% keep their nested loop (17 and 3 ms), and one with 0.05% its hash join (about 110 ms).
- Removed: reloption `phrase_index = pairs`; phrases use word positions. `CREATE INDEX ... WITH (phrase_index = ...)` fails like any unknown
  parameter, and an index built with it is refused ("built with other analyzer options") until `ALTER INDEX ... RESET (phrase_index)` and
  `REINDEX`.
- Benchmarks: the engines are measured one at a time, each with every CPU of the host, interleaved in rounds (the CPUs are no longer shared out or capped);
  `bench/bench all` runs a slim tier by default (50,000 articles, 34,165 places, about 2 GB of disk at the peak) and `--full` the full-size one; `bench/bench du` and `clean [--cache|--build|--all]` show and free the disk; `all --docs` regenerates `docs/results`, the
  charts, `docs/BENCHMARKS.md` and the benchmark blocks of `README.md` from the slim tier.

## 1.2.0

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
