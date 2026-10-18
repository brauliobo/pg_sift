# Changelog

This file is the changelog of the PostgreSQL extension. The SQLite extension (`sift` for SQLite, artifact `sift-sqlite`) has its own: [sqlite/CHANGELOG.md](sqlite/CHANGELOG.md).

## Unreleased

For each chunk it picks, from exact sizes, the
  smallest cascade of constant, frame of reference (vertical bit-packing or plain bytes), delta, run-length, sorted dictionary, decimal-float
  (ALP-style) and common-divisor (timestamps of whole seconds, amounts in cents) encodings, and layers LZ4 or zstd on a stream only where the cost rule says the saved bytes are worth the decode time. Filters run on the
  encoded form with scalar, AVX2 and AVX-512 kernels that are bit-identical and selected by `sift.simd`; what is written never depends on the CPU.
  A damaged chunk is a clean `ERRCODE_DATA_CORRUPTED` error (corruption fuzz under ASan/UBSan). The per-column `compression` policy (`auto`, `none`,
  `lz4[:N]`, `zstd[:N]`, explicit encodings) is parsed here and applied by the engine. Build: LZ4 and zstd are linked when their development packages are
  installed (`liblz4-dev`, `libzstd-dev`; the packages depend on them); `make SIFT_NO_LZ4=1` / `SIFT_NO_ZSTD=1` build without one. Tests: `colcodec_basic`,
  `colcodec_simd` and `make -C tools/colcodec test`.

- Added: aggregate pushdown on column indexes (`sift.enable_agg`, default on). A `GROUP BY` query over a table with a `sift_col` index
  (or over a lateral unnest of an indexed jsonb array or object) with `count`, `count(DISTINCT)`, `sum`, `avg`, `min` and `max`, filters,
  `IN` lists and group keys that are expressions of one column runs as `Custom Scan (SiftAgg)` on the column data when the planner can
  prove the result is the plain plan's; everything else keeps its plan (`docs/COL-PLAN.md`). `scripts/coldiff.sh` runs a corpus both
  ways and compares the results, including float tolerances, errors and the DML states of the table.

- Added: the aggregate pushdown now matches `GROUPING SETS`, `ROLLUP`, `CUBE` and `GROUPING()` (all sets in one scan, with the NULLs and
  bit masks PostgreSQL gives), float4/float8 group keys (`-0` and `0` are one group, every NaN is one group), `count`, `count(DISTINCT)`,
  `min`, `max`, `sum` and `avg` of the columns of a lookup join, and a `WHERE` on the outer query of per-order aggregates (quals on the
  keys filter the rows, quals on the inner aggregates are a per-order HAVING). Anything else keeps its plan.

- Added: numeric(p <= 18, s) predicates, group keys, expressions and distinct counts in the aggregate pushdown. A `numeric` column is an int64
  leaf with a scale: `= <> < <= > >=`, `BETWEEN`, `IN` and `IS NULL` against constants and parameters (scaled with PostgreSQL's `numeric_mul`,
  `numeric_floor` and `numeric_ceil`; NaN, infinite and out of range constants are decided without a scan), `GROUP BY` the column or an
  expression, `count(DISTINCT)`, `sum`/`avg(DISTINCT)`, and `+ - *`, `abs`, `round`, `trunc`, `floor`, `ceil` and integer casts in aggregate
  arguments, keys and filters, with the scales PostgreSQL gives and exact int128 arithmetic; whatever is not proven exact (division, other
  functions, more than 38 digits) keeps the plan. `GROUP BY` an `int8` column is pushed down too (`docs/COL-PLAN.md`, Numeric).

- Added: the vectorized operators of the aggregate pushdown: comparisons and range tests on
  int32, int64 and double vectors, dictionary and integer lookup tables, bitmap algebra, selection vectors, gathers, group ids, sums,
  min/max, per-group accumulation and adjacent-distinct counting, as one scalar, AVX2 and AVX-512 table chosen at run time by `sift.simd`.
  Every level returns bit-identical results (doubles follow PostgreSQL's float8 order, the float sum has one fixed accumulation order).
  The `aggkern_simd` regression test and `tools/aggkern` (`make test`, `make bench`) check every operator at every level against scalar.

- Fix: a ranked scan with `LIMIT` plus `OFFSET` of 67,108,864 or more failed with `invalid memory alloc request size` (so did
  `LIMIT 2147483647` meaning "all"), and so did a ranked scan drained without a limit once it had more than 16.7 million matches. The
  top-k heap now grows with the hits found, in huge allocations, and counts in 64 bits: a huge limit costs what the matches take.

- Fix: the score upper bounds that prune ranked scans were derived for up to 256 scored terms, but a plan has a term for every distinct
  term of its phrases (up to 256 phrases of 128 terms). Their slack now grows with the number of terms above 256 (nothing changes below
  it: same bounds, same pruning), so a document matching thousands of terms can no longer score above its block's bound and be pruned.
  No wrong result was found at 300 terms (`rank_leaves` compares the top k with an exact reference).

- Fix: a wildcard, regex or fuzzy leaf that expands to more than `sift.max_expansions` (512) terms failed with `the leaf at position N
  expands to more than 512 terms` in execution (`*ing`, `/.*tion/` or `~2` on a natural vocabulary; `EXPLAIN` worked). Like a prefix, it
  now matches every word it matches and scores as one term (the documents with such a word, tf the number of those words); a leaf with
  512 terms or fewer is unchanged. Cost on 100,000 articles (456,027 terms): `*ing` (72,478 documents) 90-170 ms, 42 ms with the blob in
  the backend's cache; `/.*tion/` 320 ms. `sift.max_expansions` is now the number of terms a leaf scores individually, for every kind.
- Changed: `sift.max_expansions` and `sift.prefix_cache_size` are superuser settings (`PGC_SUSET`) instead of settable by any role: with
  up to 1,000,000 expansions and 1 GB of cache per backend they let any role exhaust the server's memory. Set them in `postgresql.conf`,
  `ALTER SYSTEM`, `ALTER DATABASE ... SET` or `ALTER ROLE ... SET`; a role that sets them in a session gets `permission denied to set parameter`.
- Fix: a wide prefix gathered four bytes of memory per posting of every term it expands to, without bound (`t*` on a billion postings is
  4 GB). A pass now gathers at most 128 MB, then counts the postings of every superblock and builds the superblocks in windows that fit
  it (a superblock that alone does not fit sums the term frequencies as they come, 128 kB); the result is the same blob, bit for bit. A
  prefix that fits the budget (every one before) takes the same single pass as before. The scan of the dictionary and of the postings
  of the expansion serves cancel and `statement_timeout` every 16 dictionary blocks and at every group (a buffer lock held during the scan held them off).
- Changed: a regular expression leaf cannot have back-references (`/(a+)\1/`): they make a match exponential in the length of the term.
  The query fails at parse time like any invalid expression.

- Fix: a cursor over a sparse term (fewer than 8192 postings, or inline in the dictionary) allocated the 32 kB buffer a group of a frequent
  term needs; it now allocates one the size of its rows (at most one per posting, 64 bytes each). A 200-term `OR` over six segments
  allocated 39 MB for its cursors, now 1.1 MB. Frequent terms are unchanged. The copy of a frequent term's whole blob (a term in every
  row of a billion-row index is hundreds of MB per segment and query) is not windowed yet.

- Fix: a term lookup in a dictionary whose terms share their first 8 bytes (fixed-width ids such as `ord-0000` plus digits, order
  numbers, hashes with a common prefix) read every block of the run of terms with that prefix, linearly: 625 blocks per lookup in 40,000
  ids. It finds the block by bisection (10 blocks); other dictionaries read the same blocks as before.

- Fix: every query that reads the documents of a segment (ranked scans, filters) read and summed the whole SB directory of its docmap,
  24 bytes per superblock: more than 300 kB per segment and query on a billion-row index. A backend now keeps the directories of
  segments of 128 superblocks or more (16 MB, least recently used out; a segment never changes, so its generation names the directory
  for good) and a repeated query reads none of it. Smaller segments are read each time, as before.

- Fix: the memory of `CREATE INDEX` grows no more with the table at a small `maintenance_work_mem`. The runs a term in most rows makes were
  merged level after level into runs that held the term whole when written as a segment (1MB budget, a term in 9 of 10 rows: 6.4 to 31.5MB
  from 0.4M to 6.4M rows). Runs now stop merging where a segment's largest term fits the budget, and the build writes the table as more,
  smaller segments instead (about 20 for 400,000 such rows at 20,000 postings a term; at the default budget the layout is the same).

- Fix: `CREATE INDEX` no longer fails with `segment dictionary exceeds 4 GB` or `postings of one term exceed 4 GB` (the lengths and offsets
  of the format are 32 bits): the runs of a build merge, and make a segment, only while their largest term and their dictionary stay
  under those limits (estimated high, so a segment holds up to a third of them), and the build writes more segments instead.

- Fix: `CREATE INDEX` on documents of a heap page that together passed 1 GB (a few large, out-of-line values: three of 360 MB) failed with
  `string buffer exceeds maximum allowed length`: a page's documents are no longer all copied before they are analyzed, a large one is
  analyzed at once. A `maintenance_work_mem` of 4 GB or more (or a document with very many distinct terms) could fail with `inverted buffer
  pool exhausted`: the buffer is now spilled before a document that could exhaust its pool.

- Fix: a keyword value of more than 255 bytes failed an `INSERT`, a `COPY` or the end of a long `CREATE INDEX` with `a keyword value is longer
  than 255 bytes`. It is now indexed as its first 236 bytes and a hash of all of it (the same term from a build, an insert and a query),
  so two long values that share their start stay two values and `@@>` finds exactly the rows with that value.

- `CREATE INDEX` reports its progress in `pg_stat_progress_create_index`: the phases `scanning table`, `spilling runs`, `merging runs` and
  `writing segments`, and the documents analyzed and then written (`tuples_done` of `tuples_total`), parallel workers included. A parallel
  build that gets fewer workers than requested logs it at `DEBUG1`.

- Security: `sift.promote`, `sift.merge`, `sift.verify`, `sift.segments` and `sift.segment_postings` were executable by every role with `USAGE`
  on schema `sift`, which could force rewrites of any index or hold its maintenance lock. They now need the owner of the index's table or
  the `MAINTAIN` privilege on it (as `VACUUM` and `REINDEX` do); `ALTER EXTENSION pg_sift UPDATE` to 1.2 is not needed for this.
- Fix: a query with `col @@> ANY (array)` failed in `EXPLAIN` and at run time with `invalid byte sequence for encoding "UTF8": 0x00`. It now
  plans as a bitmap scan over one index scan per element (an OR), prepared or not; `ALL` and `NOT ANY` use a sequential scan as before.
- Planner: materialized views get the ranked `SiftScan` and the `SiftCount` pushdown like tables. A prepared `LIMIT $1 [OFFSET $2]` gives the
  ranked scan of a generic plan its limit hint (a broad term with `LIMIT 10` took 1.38x less time per query on 50,000 articles; custom plans
  and other shapes are unchanged). `sift.score()` in `UPDATE`, `DELETE`, `MERGE` and `FOR UPDATE` now fails when it is planned, saying so.
- Extension 1.2 (`ALTER EXTENSION pg_sift UPDATE`): `sift.segments` gains `extents` and `dead_docs`; new `sift.index_info(index)` (relation
  size, high-water mark, free and reusable blocks, directory entries, maintenance lock held) and `sift.version()` (the library reported 1.0
  for a 1.1 extension).
- Operations: promotions, merges and maintenance worker runs of at least `sift.log_min_duration` (default 1 s, `-1` off) are logged, with
  the inserts that wait for room in the directory and a maintenance worker that could not be registered; errors of the worker name the index.
  `ALTER INDEX ... SET/RESET` of an option that changes the analyzer warns that the table's queries and inserts fail until `REINDEX`.
  `sift.highlight` no longer parses its query for every row.
- Docs: [docs/SIZING.md](docs/SIZING.md) (disk, memory and time of large indexes, dead documents in BM25 statistics, `SERIALIZABLE`, row-level
  security, updates). `bench/ab-query.sh` times one statement on two builds, `bench/sql/dead_docs.sql` measures the effect of rolled-back
  loads on scores. `bench/cpu-query.sh` counts the instructions and CPU time of a backend per transaction on two builds, which a loaded host
  does not change.
- Tests: `scripts/ci.sh quick` also runs the regression tests that need no test-only function against the release library
  (`SIFT_TESTING` off).

## 1.4.0

- No change to the PostgreSQL extension's behavior, on-disk format or SQL: install the new files, no `ALTER EXTENSION` and no `REINDEX`. Indexes, scores and plans are the same as 1.3.1; the benchmark queries run at the same speed.
- The engine now builds as a portable core shared with the SQLite extension and the new MariaDB extension (`src/portable.list`, one host header `src/sift_host.h`, checked by `scripts/check-portable.sh` in `make check`). The sorted-run and merge code moved from `src/sift_build.c` to `src/sift_runs.c`, the merge policy to `src/sift_merge.c`; the compiled query path is instruction for instruction the same.
- `make check` takes about 1.5 minutes on 8 cores (it took about 4): one job queue over up to 8 lanes instead of four fixed lanes, faster differential and TAP tests, same coverage.
- `scripts/build-bench.sh` compares `CREATE INDEX` time, memory and temporary files of two builds at several budgets; the runs merged into each segment are logged at `debug2`.

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
  attributes of a tenant with 10% of the names (`id IN (SELECT ...)` through a left join that repeats them): hash join 2.6 s, now a
  nested loop semi join under the ranked scan, 18 ms.
- Planner: a ranked scan with a `LIMIT` is costed for what it does when drained. Past its limit hint it ranks again for four times as many
  rows, to the last match, as a hash join or a sort above it makes it; its total cost was its startup plus a hundredth per row, so hash
  plans over the whole scan looked far cheaper than they are and won against the nested loop semi join that stops at the limit. 1M names,
  152k matches of `venue`, a tenant with 0.3% of the names: 190 ms, now 68 ms (`h*`: 247 to 69 ms; a rare term 9.1 to 1.6 ms); tenants
  with 2% to 50% keep their nested loop (17 and 3 ms), and one with 0.05% its hash join (about 110 ms).
- Removed: reloption `phrase_index = pairs`; phrases use word positions. `CREATE INDEX ... WITH (phrase_index = ...)` fails like any unknown
  parameter, and an index built with it is refused ("built with other analyzer options") until `ALTER INDEX ... RESET (phrase_index)` and
  `REINDEX`.
- Benchmarks: the engines are measured one at a time, each with every CPU of the host, interleaved in rounds (the CPUs are no longer shared out or capped);

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
