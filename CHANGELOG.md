# Changelog

This file is the changelog of the PostgreSQL extension. The SQLite extension (`sift` for SQLite, artifact `sift-sqlite`) has its own: [sqlite/CHANGELOG.md](sqlite/CHANGELOG.md).

## Unreleased

- Fix: promoting a mutable segment no longer takes memory in proportion to the segment. The records were all copied to memory, about six
  times the size of the segment, whatever `maintenance_work_mem` (a segment of 138 MB at `mutable_limit = 1048576`: 827 MB at a 64 MB budget, 797 MB
  at 16 MB); they are now sorted in a tuplesort that spills to disk (191 MB and 155 MB; a segment of 46 MB at a 4 MB budget: 54 MB, where it took
  268 MB). A segment that fits the budget promotes as fast as before (1.0 to 1.2 s against 1.2 to 1.7 s at 64 MB); one that does not is slower than with
  all of it in memory (3.8 s against 1.7 s at 16 MB), the price of staying within the budget.
- Fix: a long merge no longer stalls the inserts. Promotion and merge steps now claim the segments they replace instead of holding one
  exclusive lock per index, so the background worker's merge, an inserter that finds the directory nearly full (112 of 128 entries) and
  the inserters that promote a backlog of sealed segments work on different segments side by side. Before, every such inserter waited
  for the whole merge, and the throughput of 8 bulk writers equalled that of one. VACUUM, `sift.promote()` and `sift.merge()` still run
  alone. The inserter waits only when everything left to merge is being merged by others.
- New: `sift.standby_conflicts` (superuser, default off). On a hot standby without `hot_standby_feedback`, the primary logs a recovery conflict before it reuses blocks released by a merge, so a standby scan is waited for (up to `max_standby_streaming_delay`) or cancelled by the usual conflict instead of failing with "segment recycled during scan". Default behavior is unchanged.
- Fix: a promotion or merge that fails (out of disk, a corrupt segment, an error of the host) now returns the blocks it had taken to the
  free list, where it used to leave them allocated until the next orphan sweep, and the background retry doubled its delay after each
  failure (2 s up to an hour) instead of failing again every 2 s and growing the index file each time. `VACUUM` and the inserts that
  merge for room no longer fail with the step: the error is logged as a `WARNING` and counted for the delay. An explicit `sift.promote()` or
  `sift.merge()` still raises it. A background worker that cannot be registered is logged. The wrapper and the delay add nothing to the
  insert path or to a step that succeeds.
- Fix: the file of an index grew to 2.6 times its live segments while the background worker merged a backlog of segments, and kept that size.
  The worker's one transaction held back the horizon the free extents are judged by, and the free space the merges released was too
  fragmented for the long runs a merged segment asked for. The worker now runs every promotion or merge in a transaction of its own, a
  step reuses what the previous one freed (the horizon is computed anew at each allocation), and the streams of a step take shorter runs of
  free space, down to a 256th of the size they are expected to need. 60 small segments merged by the worker: file 2.6 times the live
  segments, now 1.6. An old snapshot elsewhere (an idle transaction) still keeps what is released after it from being reused.
- Fix: a merge or promotion of a segment above about 8 GB failed with `sift segment stream needs more than 1002 extents` and, with it,
  `VACUUM`'s cleanup and the inserts that merge for room, when other writers allocated between the runs of its stream. The runs of a
  stream now grow with it (up to 64 MB), 16 GB take about 300 extents, and a merge reads at most 16 GB of segments, so a segment past
  that size is left alone (the policy takes the smaller ones of its run, `sift.merge(index, true)` too). Nothing changes below 1 GB.
- `VACUUM` merges until the policy asks for no more, instead of one step: an index that a build with a small `maintenance_work_mem` left in
  60 to 128 segments (queries 2.2x slower at 11 segments than merged) needed one VACUUM per 8 segments less; now one merges down to
  `vacuum_segments` (below). A `VACUUM` of such a backlog takes about 4 times as long as the single step it did before (0.08 s to 0.33 s at 60
  segments of 50,000 documents): deliberate maintenance work, which leaves 2 segments instead of 46 and makes the queries after it faster
  (`count(*)` 1.20 ms to 0.68 ms, ranked 2.16 ms to 1.95 ms). Queries that run while `VACUUM` merges are 6 to 7% slower, from the
  extra merging itself: the same queries with the same segments and no merge running, and the same maintenance on the old build, measure the same.
- New: reloption `vacuum_segments` (default 2, 1 to 128): the number of segments `VACUUM` leaves an index with. A higher value is shorter
  `VACUUM` and more segments to query (60 segments of 50,000 documents: 0.19, 0.15, 0.12 and 0.10 s at 2, 4, 8 and 16, `count(*)` 0.08, 0.14,
  0.22 and 0.35 ms). `ALTER INDEX ... SET` applies it with no `REINDEX`; the worker's and the inserters' merges are unchanged.

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
