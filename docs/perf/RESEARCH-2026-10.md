# Where pg_sift is behind Elasticsearch, and what to do about it (October 2026)

Branch `perf/research`, based on release 1.3.0. The branch is merged: the two prototypes (`rank: overpull:`, `plan: estimate:`) and the prepared-statement gate of section 10 are in release 2.0.0.
Written on 2026-10-17: "the committed session" below is the slim tier of 2026-10-17 (`90faba5`: 50,000 articles, 34,165 places, every engine alone on the whole host of 16 threads, load average 7-18),
which `docs/results` has since replaced by the 3-CPU run of 2026-10-19 ([BENCHMARKS.md](../BENCHMARKS.md)); its numbers (sift or 6,210 / Elasticsearch 7,600 queries per second, for one) are a historical baseline and do not compare with the current tables.
Every number is labelled **measured** (this branch, this host), **cited** (a paper or a benchmark of someone else, with its reference in section 9) or **estimated**
(arithmetic from measured numbers, not run).

## 1. Summary

| class (slim tier, 8 clients) | sift / Elasticsearch in the committed session | where the gap is, from the profiles below |
|---|---|---|
| ranked OR | 6,210 / 7,600 | not the pruning algorithm: an OR of a trace that holds stop words matches 78% of the documents, 81% of the blocks are scored, and a perfect threshold from the first block saves 5% of them. Cost is per scored block and per query (plan + parse + bind about a quarter). Not reproduced here: 8,300 qps on this host, a tie with Elasticsearch's 7,600 |
| filter | 12,100 / 14,400 | PostgreSQL side and setup: planner 18% of the cycles, parse 5%, bitmap-to-mask conversion 13%, the sift bind 9%; ranking itself is half |
| join | 5,320 / 16,400 | the algorithm in the over-pull path: a join that keeps a tenth of the rows made the scan search three times (10, 40, 160 hits), 2.3x the block work, plus 376 candidates verified twice. Prototype 1 below: +26% to +31% qps. What stays is the real difference: Elasticsearch filters inside the search, PostgreSQL after it |
| count(*) | 22,700 / 25,000 | a tie. 28% of the cycles are planning (the estimate ran four times per statement), 6% parse, 54% counting (a third of it phrase positions). Prototype 2: +12% qps |
| mixed | 10,600 / 10,200 | a tie; same shape as filter and count |
| places autocomplete | sift ahead in 17 of 24 cells | behind in a few cells with a post-filter or a count: p1 filter 0.92 vs 0.45 ms, p1 count, a3 count and filter, x2 count. One cause found: the planner leaves the filter outside the scan, 2.9 ms against 0.40 ms with the pushdown forced |

Two prototypes, both bit-identical in rows, scores and ties, both with new regression tests, `scripts/ci.sh quick` green:

| commit | change | measured |
|---|---|---|
| `rank: overpull:` | a consumer that pulls past k grows the next search 16x (4x beyond 4096 hits) instead of 4x | join: 5.16M to 4.39M user cycles per query (-15%), 4,778 to 6,030 qps (+26%) at 8 clients |
| `plan: estimate:` | the last 8 estimates of the index are kept under (file, directory version, query) | count: 1.18M to 1.03M cycles (-12%), filter -9%, mixed -8%; count 26,050 to 29,255 qps (+12%) at 8 clients |

The ranked list of what to do next is in section 6.

## 2. Method

* Host: 16 hardware threads (8 cores x 2), AVX-512 with VPOPCNTDQ (sift runs its `avx512` kernels), PostgreSQL 18.6, kernel 7.2.4, `perf_event_paranoid = 2`
  (user-space counters only; kernel time comes from `utime` and `stime` of the backend). The host is shared: load average 5 to 30 during the runs, recorded with every table.
* Data: the slim tier, 50,000 Simple English Wikipedia articles (4,904 heap pages, 10.2 documents per page, 31 MiB index with positions), the suite's own query trace
  (`bench/sql/load.sql`, `harness.sql`, the statements of `bench/engines/sift.sh`). Built in a private cluster with the settings of `bench/steps/cluster.sh` (1 GB shared buffers, `jit = off`).
  `bench/bench` was not used (it holds a global lock other agents need); the statements and pgbench scripts are the suite's.
* Profiles: one backend (`pgbench -c 1`) running the class script, `perf record -F 1999 --call-graph dwarf` for 4 s (about 8,000 samples per class) and `perf stat`; the samples are put in
  mutually exclusive phases by the first frame of the stack that matches (planner, parse, bind, score block, seek, verify...). The PostgreSQL binary is stripped, so some executor frames
  are unresolved: they stay in "exec other".
* A/B: two builds of the library loaded in the same cluster and database through `PGOPTIONS='-c dynamic_library_path=...'` (the on-disk format is untouched), interleaved,
  5 repeats of 3 s single backend (user cycles and instructions per transaction from `perf stat -p`, which hold up under host load) or 6 repeats of 3 s at 8 clients (qps). Mean +- standard error of the repeats.
* Counters per query: `EXPLAIN (ANALYZE, FORMAT JSON)` over the first 500 statements of the class.

## 3. Part 2: where the time goes

### 3.1 Reproduction

pgbench `-c 8 -j 4`, 3 s, `--random-seed=7`, the committed session in the last column (different host, load 7-18):

| class | this host, qps (3 runs) | committed |
|---|---|---|
| or | 8,300-8,350 | 6,210 |
| filter | 11,900-12,260 | 12,100 |
| join | 5,240-5,370 | 5,320 |
| count | 23,400-25,600 | 22,700 |
| mixed | 13,000-14,100 | 10,600 |
| and / phrase | 23,100-25,700 / 19,200-23,100 | 19,300 / 17,300 |

Filter, join and count reproduce; OR does not (this host is 34% faster on it, 25% on `mixed`). The OR gap to Elasticsearch in the committed table may be partly the session, so the engine work below is judged on its profile, not on that cell.

### 3.2 Cycles by phase (one backend, user space, % of samples)

| phase | or | filter | join | count | mixed |
|---|---:|---:|---:|---:|---:|
| parse | 2.8 | 4.8 | 3.0 | 6.5 | 4.9 |
| plan (planner + `sift_eval_estimate`) | 8.6 | 17.9 | 10.3 | 28.4 | 15.7 |
| executor setup: sift bind, rank begin, filter build, start/end | 6.6 | 23.7 (filter build 12.7) | 4.7 | 5.6 | 10.9 |
| rank: score blocks | 40.2 | 21.6 | 36.4 | | 27.8 |
| rank: seeks, unit terms, block terms | 24.2 | 16.4 | 16.4 | | 20.1 |
| rank: verify + heap fetch | 5.7 | 4.9 | 16.9 | | 8.2 |
| rank: other (loop, top-k) / count evaluation | 8.9 | 6.7 | 8.0 | 53.8 | 8.0 |
| executor nodes (join, inner index scan), portal | 1.3 | 1.7 | 3.4 | 2.3 | 1.9 |
| protocol and the rest of the backend loop | 1.9 | 2.3 | 1.2 | 3.5 | 2.4 |
| kernel time (syscalls) as a share of the backend's CPU | 8% | 12% | 5% | 18% | 13% |

Self time by function group inside the sift library (share of all samples): decode and bitmap operations (`ef_decode`, `ls_*`, `or2`, `chunks2`, popcounts, `term_mask`, docmap loads) 27-38%,
scoring kernels (`bm25_sel`, `prune_gt`, `knorm`, `zero_rows`, top-k) 7-20% (join 19.6, or 19.6, filter 10.5, mixed 13, count 0.8), libc `memcpy`/`memset` 7-10%, buffer manager and heap (`LWLockRelease`, `ResourceOwnerForget`, `heap_hot_search_buffer`) 6-10%,
positions 0.4-7% (count 6.9: the phrase third of the trace).

### 3.3 Hardware counters (4 s windows, user space)

| class | user cycles per query | IPC | instructions per query | LLC miss / reference | branch miss | L1d misses per 1k instr |
|---|---:|---:|---:|---:|---:|---:|
| or | 2.8M | 2.62 | 7.2M | 7.8% | 2.0% | 19 |
| filter | 1.9M | 2.27 | 4.1M | 18.7% | 2.5% | 12 |
| join | 4.7M | 2.26 | 10.7M | 8.0% | 2.2% | 19 |
| count | 0.82M | 2.53 | 2.2M | 24.0% | 1.9% | 6.6 |
| mixed | 1.5M | 2.43 | 3.7M | 13.8% | 2.0% | 17.5 |

IPC above 2.2 and a few percent of branch misses: the queries are instruction-bound, not memory-bound; the lever is fewer instructions, not prefetching.

### 3.4 What the engine reads, and the lower bound

Per query, mean of 500 statements (EXPLAIN counters; the table has 307 blocks of 16 pages, 19.2 superblocks of 256 pages):

| class | matches | superblocks | blocks scored / skipped | candidates verified | heap checks | buffer hits | exec time |
|---|---:|---:|---:|---:|---:|---:|---:|
| or | 39,229 (78% of the corpus) | 19.3 | 248.7 / 53.5 | 87.9 | 97.9 | 148 | 0.56 ms |
| filter | 338 | 14.2 | 112.2 / 1.9 | 20.6 | 24.6 | 96 | 0.35 ms |
| join | 1,779 before the join | 30.2 (summed over 3 searches) | 116.2 / 3.6 (last search only) | 376 (last search) | 567 | 465 | 0.88 ms |
| count | | 9.6 | | 0 | 0 | 56 | 0.10 ms |

**Oracle threshold.** A research GUC (not on the branch) starts the top-k heap with the true k-th score of the query, i.e. the best any ordering of the blocks could do with these block bounds:

| class | blocks scored, normal | with the true threshold from block 1 | candidates verified | exec time |
|---|---:|---:|---:|---:|
| or | 248.7 | 235.6 (-5%) | 87.9 -> 10.0 | 0.84 -> 0.70 ms (-16%) |
| filter | 112.2 | 110.8 (-1%) | 20.6 -> 5.1 | 0.50 -> 0.49 ms |
| mixed | 99.2 | 94.8 (-4%) | 34.3 -> 5.1 | 0.40 -> 0.36 ms |
| join | 116.2 | 109.9 (-5%) | 376 -> 35 | 0.92-1.39 -> 0.73 ms (about -30%) |

So the order in which the best-first loop visits blocks is already within 5% of ideal; the threshold only matters for the number of verified candidates. A block bound of the current granularity (16 pages, about 160 documents here) is above the true k-th score for 77% of the blocks of an OR trace that holds `the`, `of`, `a`; to score fewer blocks the bounds must get tighter (section 6, row 9), the search cannot get smarter. The lower bound for OR is then the time to score 236 blocks, about 0.65 ms of the 0.84 ms execution.

### 3.5 Planning, prepared statements, JIT (to understand, the suite's rules unchanged)

Planning is per statement (the suite sends literals in the simple protocol). `pg_stat_statements` with `track_planning`, mean plan time 0.035-0.09 ms against 0.12-0.88 ms execution: 12-30% of a statement. 1 backend, 5 interleaved runs, base library:

| class | simple, literal (suite) | extended, parameter | prepared (plan cached) | prepared, `force_generic_plan` | simple + `jit=on` |
|---|---:|---:|---:|---:|---:|
| count | 3,164 +- 296 | 3,214 +- 301 | **4,773 +- 456** | 4,701 +- 410 | 3,430 +- 221 |
| or | 1,188 +- 52 | 1,155 +- 46 | 1,322 +- 18 | 1,192 +- 43 | 1,126 +- 52 |
| filter | 1,589 +- 162 | 1,606 +- 168 | **1,202 +- 133** | 1,229 +- 129 | 1,742 +- 187 |
| join | 672 +- 85 | 659 +- 77 | 730 +- 89 | 784 +- 95 | 680 +- 70 |

(host load 13-21, wide errors.) Findings: skipping parse and plan is worth about +50% on `count(*)` and +10% on OR; JIT makes no difference (all plans are far below `jit_above_cost`);
and a **generic plan loses the filter pushdown**: with `$1` as the query the planner cannot estimate the matches, so the plain scan wins on cost and the filter query is 25% *slower* prepared than as a literal
(the custom plan keeps the `Bitmap Index Scan on docs_created` child). That is a defect to fix before prepared statements are advised for ranked filters (section 6, row 4).

Sift plans each statement three or four times over: the estimate (opened index, parsed query, dictionary lookups per term and segment) ran 3 times for OR and join and 4 times for count and filter, then once more
at execution with the real bind. Prototype 2 removes the repeats.

### 3.6 Verdicts

* **OR.** Algorithm: the block loop is at the ideal order (3.4). Setup: plan + parse + bind is a quarter of a 0.64 ms query. PostgreSQL per-row overhead: 6% (verify and fetch of 88 candidates; the executor sees 10 rows).
  The bulk is 249 blocks times about 2.6 us: decoding the OR of eight terms per superblock (`sift_seek_eq` 24%, `ls_seek`/`ef_decode`/`or2`), then masks, `bm25_sel`, `prune_gt`, `zero_rows`, `memset` per block. Positions on or off make no difference here
  (7,688 +- 174 against 7,637 +- 176 qps at 8 clients; 0.55 against 0.53 ms), so the 6,208 vs 7,146 of the committed table is not the index size. Elasticsearch and sift are in the same range once the session is factored out.
* **filter.** Mostly setup: 18% planning, 5% parse, 13% turning the bitmap of `created >= cutoff` into masks (the Bitmap Index Scan 4%, `tbm_begin_private_iterate` with its `pg_qsort` 6%, `sift_filter_build` the rest), 9% the sift bind; the ranking is 45%.
  Benchmark setup: none (the plan is the right one, with pushdown). Prepared with a generic plan it gets worse (3.5).
* **join.** Algorithm. The ranked scan sits under a nested loop that keeps a tenth of the rows (`categories.region = 3`); the scan is pulled past its hint of 10 and every pull past it was a new search with 4x the hits, each scoring the blocks again:
  10 / 40 / 160 hits scored 81 / 292 / 292 blocks and verified 90 / 296 / 1,148 candidates for one OR query (instrumented run). The 376 candidates of the mean query are fetched from the heap twice (verify, then emit: 567 heap checks).
  Fix: prototype 1. What stays (about 2/3 of the profile) is the model: PostgreSQL applies the join predicate after the scan; Elasticsearch's `region` is a field of the document, so the equivalent for sift is pushing the join predicate into the scan as a mask (section 6, row 3).
* **count.** A tie; 28% planning, 54% counting of which a third is phrase positions (`phrases_hold`, 18% of all cycles: the phrase third of the trace needs them for exactness; Lucene's early termination of `TotalHits` does not apply to an exact `count(*)`, and Elasticsearch's own default tracks only up to 10,000 hits, section 5).
* **mixed.** A tie; its profile is the three above mixed (plan 16%, setup 11%, rank 64%).

### 3.7 Places (single client, p50 ms; committed session and a reproduction here)

Committed (slim), cells where Elasticsearch is ahead by more than the standard errors:

| cell | sift | Elasticsearch |
|---|---:|---:|
| p1 filter (1-letter prefix, `country_code = 'US'`, top-20) | 0.919 +- 0.013 | 0.450 +- 0.038 |
| p1 count | 0.153 +- 0.014 | 0.096 +- 0.013 |
| a3 count / a3 filter | 0.204 / 0.232 | 0.163 / 0.197 |
| x2 count | 0.168 | 0.121 |

Everything else is a tie or a lead for sift (top-20: sift ahead in 5 of 6 classes by 1.1-2.1x, boost 4 of 6). Reproduction on this host (cluster of section 2, the places schema of `bench/sql/places_schema.sql`, sift index with `fields = 'name', prefix_index = 3`, 300 queries per class):

| class | top | filter | count |
|---|---:|---:|---:|
| p1 | 0.269 | **3.165** | 0.358 |
| a3 | 0.192 | 0.220 | 0.137 |
| x2 | 0.189 | 0.202 | 0.129 |
| p3 | 0.182 | 0.271 | 0.118 |

`country_code = 'US'` keeps 10% of the places and the filter has no index: the scan verifies 15,086 candidates (heap fetch and qual each) for one query of `a*`. With a btree on `country_code` the planner still takes the unfiltered scan
(2.93 ms); `SET sift.filter_pushdown = force` gives **0.396 ms** (101 candidates, 196 exact pages), i.e. 7.4x, below Elasticsearch's 0.45. The p1 filter gap is a planner cost-model miss on prefix queries (their scores tie, so the post-filter
estimate of `k / pass` candidates is far too low) plus, without the index, the model itself. I did not reproduce the committed 0.919 ms; my p1 filter is slower because of the missing index.

### 3.8 Cold cache (committed slim table, p50 / max ms, data on tmpfs so no disk behind the evictions)

sift is ahead of Elasticsearch in all seven article classes (and 0.33, or 1.02, phrase 0.32, mixed 0.37, count 0.23, filter 0.36, join 0.38 against 1.82, 1.76, 1.56, 1.68, 1.09, 1.34, 1.41), the only cell where another engine beats it is OR against
pg_textsearch (0.93). Maxima: join 4.26 ms. No class is behind on cold cache; the table does not measure disk reads, so a real disk would add the heap fetch of each candidate (section 6, row 12).

### 3.9 Where the lead over pg_search is modest (ranked OR, 3.7x)

pg_search is nearly flat across classes (1,660-2,310 qps for and / or / phrase / mixed / count / filter), a per-query fixed cost of about 3.5 ms per client at 8 clients; sift's `and` is 8.4x it and `or` 3.7x. OR is the class where sift does the most real work
(78% of the corpus matches, 81% of the blocks are scored) while pg_search's cost is dominated by what it pays regardless of the query. So the "modest" lead is a floor on pg_search's side; sift's own OR cost is the profile above. Not profiled on pg_search's side (unverified).

## 4. Part 1: the engine in one page

Block-ordered best-first evaluation (BMP-like, DESIGN.md (a)): a unit is a (segment, superblock of 256 heap pages); its 16 blocks carry u8 score bounds per term; units are sorted by their best bound, blocks within a unit by their bound;
the loop stops when the next bound is not above the verified k-th score. Inside a block: leaves ordered by bound, term-at-a-time pruning with the sum of the remaining bounds (`prune_leaves`, `prune_gt`), terms that cannot lift a score over the
threshold are dropped per unit (`unit_terms`) and per block (`block_terms`) - this is MaxScore's essential/non-essential split, already in place. Scores are float32 with a fixed operation order, identical in the scalar, AVX2 and AVX-512 kernels.
Counts use group counts and the visibility map with a popcount; filters are masks ANDed into every block. Earlier optimization history: bitmap groups from 8K postings, `bm25_sel`/`prune_gt` (-27% cycles of ranked AND/OR), positions (phrases from the index),
`sift_seek_eq`, tf positions per block, filter pushdown.

What the backlog items look like after this profile: *bit-packed positions* and *positions through merges* only touch phrases (count's third); *phrase count exactness* is not a cost problem; *page-at-a-time fetch* is the verify cost (6-17%);
*multiway OR kernel* is the `or2` + `ls_seek` + `ef_decode` share (about 15% of OR); *MaxScore split* exists; *skipping useless prune calls* is part of row 8 below; *cold prefetch* is cold-only and sift already leads cold.

## 5. Part 3: literature and prior art

Condensed from a research pass (sources in section 9; "snippet only" means only a search snippet was seen). Gains are those of the source on its own benchmark, not predictions for sift.

| technique | how | evidence | fit with the two-level bitmap design | cost / risk |
|---|---|---|---|---|
| Lucene `MaxScoreBulkScorer`, windows, clause-at-a-time (PRs 12361, 14701, 13941) | essential/non-essential terms by the current threshold; score a window of docs per clause and update the heap once per window | OrHighHigh +15% (bulk scorer), +28% (clause at a time), OrMany +42% (window sizing); filtered conjunctions lost 9-10% | sift already splits per block; the transferable part is batching candidates per window before the verify callback and deciding the split per run of blocks | medium; exactness unaffected, ramp-up before k hits is the pitfall |
| Intersect the essential terms when one remains (Lucene PR 12589) | when only one essential clause is left, visit only the docs that also have the best non-essential clause | OrHighHigh +11-14%, OrHighMed +6-14% | bitmap AND is native | low; mostly 2-term queries, little for stop-word-heavy traces |
| WAND / BMW / VBMW / longer skipping | pivot selection over doc-ordered cursors with block maxima | longer skipping up to -37% latency on short queries (BMW-LS); VBMW best for small k, MaxScore for long queries and k > 100 | DESIGN.md rejected pivoting (branchy, not SIMD); skip distances are redundant with the 64-entry group maximum | low for skips, high for WAND |
| BMP / superblock pruning (SIGIR'24/'25, 2026) | block-max arrays summed with SIMD, blocks visited in bound order; superblock level above | BMP 2.3-10.5 ms vs MaxScore 13.6-120 ms on learned sparse indexes, best exact block size 8-32 docs | this is sift's design; the lesson is granularity (sift's block is about 160 docs here) | medium, format change; gain on BM25 with stop words unknown |
| threshold seeding / quantile estimates | predict the k-th score before traversal | >2x for large k (snippet only) | needs a verified lower bound under MVCC; our oracle shows only 5% fewer blocks and 16% less exec time | medium, low gain |
| document reordering (BP), anytime / SaaT (JASS), Seismic | renumber docs by similarity; impact-ordered or approximate | PEF index -36%; random order ~3x slower than URL order | sift's key is the heap ctid, merges never renumber; approximate methods break bit-identical scores | not applicable; heap clustering is a user tuning tip |
| dense postings as bitsets, SIMD-BP128, StreamVByte, Roaring | blocks of 128 deltas bit-packed with SIMD, dense terms as bitmaps | Lucene 10.2 dense bitsets: disjunctions +77% to 4x, conjunctions +38% to 5x; SIMD-BP128 ~2x faster than varint | sift's groups are the bitset idea; LIST blobs (sparse terms) use Elias-Fano; a swap is a format change | high; the decode share here is 27-38% of self time, mostly bitmap ORs and Elias-Fano |
| `track_total_hits` / `TotalHits` early termination | count exactly only to 1,000 (Lucene) / 10,000 (Elasticsearch default), then report `gte` | third-party blog 10 -> 9 ms (snippet only) | SQL `count(*)` must be exact; ParadeDB-like all-visible shortcuts already exist (VM pext, group counts) | an explicit capped function is possible; it does not make the suite's exact counts faster |
| filter inside the pruner (Lucene PR 14014, ParadeDB) | filter as a bitset consulted by the bulk scorer; columns in the index with min/max | FilteredOrMany +36%, FilteredOrHighMed +14%, FilteredOrStopWords -10% | sift ANDs a bitmap-derived mask already; ParadeDB's column-in-index would remove the bitmap build (13% of the filter class) | high (storage, DDL, update cost) |
| joins with hits | ranked outer + primary-key probes; ParadeDB's JoinScan pushes the join into its scan | no numbers; issues 6276, 6352 show whole-probe-side scans and wrong results; still beta | the nested loop under a ranked scan exists; cost accuracy is the lever, not a JoinScan clone | cost model low, JoinScan very high |
| ReadStream / AIO for heap fetches | callback of block numbers, the stream prefetches and combines reads | no figure for this case; index-prefetch patch targets PostgreSQL 20 (v37, 2026-10-11) and needs `amgetbatch`, sift has `amgettuple = NULL` | the unordered scan already streams; ranked verify would need a two-stage batch | medium; cold/disk only, setup cost hurts k = 10 on warm data |
| parallel custom scans, JIT, plan caching | DSM callbacks; cost-based JIT; generic plans | measured here: JIT irrelevant, generic plan loses pushdown | `sift.parallel_scan` exists; Gather costs milliseconds against 0.1-0.9 ms queries | none for parallel; plan fix in row 4 |

## 6. Part 4: ideas, ranked

Expected gain is a range with its basis; effort is in days of one engineer including tests; "done" rows are the prototypes of section 7. Correctness is the same bar for all: rows, scores and ties identical to the current evaluator, on-disk format changes need a REINDEX path.

| # | idea | class | mechanism | expected gain | effort | risk |
|---|---|---|---|---|---|---|
| 1 | **over-pull growth** (done) | join, any lossy consumer | next search 16x while the hit array is small | measured: join -15% cycles, +26% qps | 0.5 | none; result is the full ranking |
| 2 | **estimate memo** (done) | count, filter, mixed, all | keep the last estimates under (file, directory version, query) | measured: count -12%, filter -9%, mixed -8% cycles, count +12% qps | 0.5 | none: an estimate reads immutable segments only; test fails without the version in the key |
| 3 | join predicate as a mask: evaluate the dimension side first (`categories.region = 3`), turn its ids into a bitmap over `docs.category_id` and push it with the existing filter machinery | join | candidates 376 -> 35, heap checks 567 -> 110 (oracle) | measured oracle: exec -30% and no repeated searches; estimated 1.3-1.5x on the class | 8-10 | planner shapes (semi join, unique inner), bitmap size for large dimension sets; cost model must choose it |
| 4 | generic-plan cost for a Param query: keep the pushdown when the filter is selective and the query unknown (assume the default match fraction) | filter, any prepared use | decide the pushdown from the bitmap's own selectivity | measured: prepared filter -25% today; prepared count +50% available | 2 | wrong plans for broad queries; check with `plan_cache_mode` variants |
| 5 | pushdown cost model for prefix queries (ties make `k / pass` too small) and for filters with an index | places p1 filter, a3 filter, filter | cost the post-filter scan from observed ties or a floor of the match count | measured: p1 filter 2.93 -> 0.40 ms with `force` (7.4x) | 3 | plan regressions elsewhere; `filter_pushdown_cost` tests |
| 6 | cheaper bitmap-to-mask: avoid the `pg_qsort` of `tbm_begin_private_iterate`, build the masks from the bitmap child's TIDs directly | filter | 6% of the filter class is the sort, 4% the bitmap scan | estimated -5 to -8% cycles on filter | 3 | PostgreSQL internals (TIDBitmap iteration), PG17-19 differences |
| 7 | dictionary term cache across binds (key: segment generation, term) | all, mostly count and filter | the bind at execution repeats the lookups the planner just did; `sift_seg_lookup` is 13% of count | estimated -3 to -6% cycles | 3 | invalidation (immutable segments make it safe by key), inline positions pointers |
| 8 | OR block pipeline: keep each leaf's rows from the prune pass and sum them in leaf order instead of scoring survivors again; fewer `zero_rows`/`memset`/`term_mask` per block | or, join, mixed | one `bm25_sel` per leaf and block instead of up to two | not measured; the per-block cost is 2.6 us, estimated -3 to -8% on OR | 3-4 | bit-identical sums need the same addition order (documented in `prune_leaves`) |
| 9 | tighter bounds: a per-page (or 4-page) bound tier under the 16-page block bound | or, filter | BMP's best exact blocks are 8-32 documents, sift's is about 160 here | oracle: perfect threshold alone gives -5% blocks, so the gain must come from bound tightness; unknown, cited BMP numbers are for learned sparse | 8 | format change (REINDEX), +0.3 bits/posting for dense terms, UB pass reads more |
| 10 | Lucene-style windowed candidate batching: collect candidates over several blocks, sort by score, verify the best first | or, join | 88 verified for 10 results (oracle: 10); join 376 | cited: +15-42% on Lucene OR; estimated here: verify share 6-17% halved | 4 | ties: verify order must not change the result (it does not for a top-k with total order) |
| 11 | verify without a slot, or from the visibility map | join, or | `sift_vis_fetch` builds a buffer tuple slot per candidate and the survivors are fetched again | estimated -2 to -5% on OR, -6 to -8% on join | 3 | HOT redirects change the tie key on all-visible pages: needs a page-level check |
| 12 | ReadStream / AIO for ranked verify on cold or disk-backed data | cold | batch the heap pages of candidates | none warm; cold figures not measurable on this tmpfs | 4 | setup cost for k = 10; gate on expected pages |
| 13 | SIMD fusion: expand the term mask straight into the kernel, one multiway OR for the leaf masks | or, mixed | the `or2` + `ls_seek` + `ef_decode` + `term_mask` share is about 15% of OR | estimated -5 to -10% on OR | 5 | kernel equivalence tests (`kernels_simd`), AVX2/scalar paths |
| 14 | capped count function (Lucene `TotalHits`, Elasticsearch `track_total_hits`) | count | `sift.count_upto(query, n)` stops at n | none for the suite (exact counts) | 1 | semantics: opt-in only |
| 15 | parallel ranked scan thresholds | none | Gather costs ms, queries take 0.1-0.9 ms | none | 0 | do not pursue |

Order of work: 1 and 2 (done), then 4 and 5 (small, big effect on prepared and places filters), then 3 (the join gap), 6 and 7 (setup shares), 8 and 10 (OR constants), 13, 9 last (format).
Not on the list because the profile shows nothing to gain: MaxScore splitting (exists), threshold seeding (oracle 5%), WAND pivoting, the per-row executor overhead (1-4%), parallel ranked scans, JIT.

## 7. Part 5: prototypes

Both are on the branch, based on `1f25b99`, each in a focused commit with its regression tests. All A/B below ran two builds in the same cluster, interleaved; host load average 10-22.

### 7.1 `rank: overpull:` - grow the search 16x

`rank_ensure` multiplied the hit array by 4 on every search past the hint, and each search scores the blocks again. A join keeping a tenth of the rows pulled 10 -> 40 -> 160; now 10 -> 160 (and past 4,096 hits it grows by 4 as before,
which bounds the memory a deep drain can ask for). Rows, scores and ties are those of the full ranking: the new `rank_overpull` test pulls a 1.5% join through two growth steps and compares the top-30 scores and the drained sequence with the full ranking.

| measure | base | prototype | change |
|---|---:|---:|---:|
| join, 1 backend: user cycles per query (5 runs) | 5.16M +- 0.15M | 4.39M +- 0.10M | -15% |
| join, 1 backend: instructions per query | 11.2M +- 0.18M | 8.46M +- 0.15M | -24% |
| join, 8 clients: qps (6 runs, load 15) | 4,778 +- 200 | 6,030 +- 206 | +26% |
| or / filter / mixed / count, 1 backend cycles | | | within noise (-2% to +3%) |
| or / filter / mixed / count, 8 clients qps | 6,785 / 11,710 / 12,964 / 26,052 | 6,923 / 11,233 / 13,106 / 25,904 | within noise |

### 7.2 `plan: estimate:` - remember the last index estimates

`sift_eval_estimate` opens the index, parses the query and binds the terms; the planner calls it 3-4 times per statement. An estimate reads the dictionaries of the immutable segments only, so (file locator, directory version, expansion cap, query texts) decides it:
the last 8 answers are kept per backend. `plan_estimate` checks that a promotion in the same transaction, a different query and a REINDEX are estimated again, and fails when the directory version is dropped from the key (checked).

| measure | before (prototype 1) | after | change |
|---|---:|---:|---:|
| count, 1 backend cycles per query | 1.178M +- 0.018M | 1.033M +- 0.025M | -12% |
| count, instructions | 2.158M | 1.826M | -15% |
| filter | 2.035M +- 0.088M | 1.852M +- 0.051M | -9% |
| mixed | 1.831M +- 0.090M | 1.681M +- 0.053M | -8% |
| or / join | | | within noise |
| count, 8 clients qps (6 runs) | 25,904 +- 932 | 29,255 +- 1,238 | +13% |
| join, 8 clients qps | 6,030 +- 206 | 6,245 +- 278 | +4% (noise) |
| filter / mixed / or, 8 clients | 11,233 / 13,106 / 6,923 | 11,340 / 13,092 / 6,841 | within noise |

On the loaded host the 8-client filter and mixed cells do not show the single-backend gain (error 4-6%); the cycle counts are the reliable column.

### 7.3 Quality gates

* New tests `rank_overpull` and `plan_estimate`; `plan_estimate` is a mutant check (dropping the directory version from the key fails it).
* Full `installcheck` (40 tests) on the testing flavor, then `scripts/ci.sh quick` (build, ISA baseline, installcheck, isolation emulated and real, TAP, fuzz, difftest 3,000 documents with 8 mutants, positions-off difftest, soak, leak gate) on the final head: see the commit-time result in the final report; run in a copy of the tree because the quick tier builds the testing flavor.
* Scores and ties: the over-pull change only changes how many hits a search keeps; the estimate only changes planner costs. Neither touches a kernel or the evaluation order.

## 8. What I could not verify

* The committed OR gap to Elasticsearch (6,210 vs 7,600): on this host sift is at 8,300 qps; the session difference is not explained.
* The committed p1 filter p50 (0.919 ms): my reproduction is 3.2 ms (no index on `country_code`, as in the adapter); the suite's number comes from a setup I did not reproduce. The 7.4x with a forced pushdown is on my setup.
* Kernel-side cost (syscalls, scheduling) is only the `stime` share; `perf_event_paranoid = 2` blocks kernel samples and `ptrace_scope = 1` blocked gdb, so call counts came from a temporary elog patch (not on the branch).
* Rows 6-8, 10, 11, 13 of section 6 are estimates from profile shares, not prototypes.
* pg_search's side of the OR comparison, Elasticsearch's own profile, and any disk-backed cold-cache behaviour (this host's scratch is tmpfs).
* The literature numbers are from the sources' own benchmarks (Lucene luceneutil Wikipedia, MS MARCO learned sparse); several only as search snippets (marked).

## 9. References

Fetched (read) unless marked.

* Mallia, Porciani. "Faster BlockMax WAND with Longer Skipping". ECIR 2019. https://www.antoniomallia.it/uploads/ECIR19a.pdf
* Mallia, Siedlaczek, Suel. "An Experimental Study of Index Compression and DAAT Query Processing Methods". ECIR 2019. https://www.antoniomallia.it/uploads/ECIR19c.pdf
* Mallia, Suel, Tonellotto. "Faster Learned Sparse Retrieval with Block-Max Pruning" (BMP). SIGIR 2024. https://arxiv.org/pdf/2405.01117
* Carlson, Xie, He, Yang. "Dynamic Superblock Pruning for Fast Learned Sparse Retrieval". 2025. https://arxiv.org/pdf/2504.17045
* Carlson, Xie, Shah, Yang. "Efficiency Optimizations for Superblock-based Sparse Retrieval". 2026. https://arxiv.org/pdf/2602.02883
* Gou, Mallia, Liu, Shao, Suel. "Fast and Effective Early Termination for Simple Ranking Functions". SIGIR 2025. https://www.pinecone.io/research/SIGIR25c.pdf
* "Beyond Quantile Methods: Improved Top-K Threshold Estimation". 2024 (abstract). https://arxiv.org/abs/2412.10701
* Savino, Venturini. "RISE: A Rust Library for Inverted Index Search Engines". 2026. https://arxiv.org/pdf/2606.07187
* Mackenzie, Petri, Moffat. "Anytime Ranking on Document-Ordered Indexes". TOIS 2021 (abstract). https://arxiv.org/abs/2104.08976
* Lemire, Boytsov. "Decoding billions of integers per second through vectorization" (abstract). https://arxiv.org/abs/1209.2137
* Lemire, Kurz, Rupp. "Stream VByte: Faster Byte-Oriented Integer Compression" (abstract). https://arxiv.org/abs/1709.08990
* Lucene PRs: MaxScore bulk scorer https://github.com/apache/lucene/pull/12361 ; intersecting clauses in disjunctions https://github.com/apache/lucene/pull/12589 ; window sizing https://github.com/apache/lucene/pull/13941 ; filtered disjunctions https://github.com/apache/lucene/pull/14014 ; clause-at-a-time https://github.com/apache/lucene/pull/14701
* Elastic. "MAXSCORE & block-max MAXSCORE". https://www.elastic.co/search-labs/blog/more-skipping-with-bm-maxscore ; "Bringing speedups to top-k queries with many and/or high-frequency terms" https://www.elastic.co/blog/speedups-top-k-queries-high-frequency-terms ; "Faster Retrieval of Top Hits in Elasticsearch with Block-Max WAND" https://www.elastic.co/blog/faster-retrieval-of-top-hits-in-elasticsearch-with-block-max-wand
* Lucene release news 10.2-10.5 (first 100,000 characters). https://lucene.apache.org/core/corenews.html ; `IndexSearcher` javadoc 10.1 https://lucene.apache.org/core/10_1_0/core/org/apache/lucene/search/IndexSearcher.html ; nightly OrHighHigh annotations (no QPS values) https://benchmarks.mikemccandless.com/OrHighHigh.html
* Grand. "An analysis of Search Benchmark, the Game". 2025. https://jpountz.github.io/2025/05/12/analysis-of-Search-Benchmark-the-Game.html
* ParadeDB. "How We Optimized Top K in Postgres" https://www.paradedb.com/blog/optimizing-top-k ; "Making COUNTs and the Rest of Search Really Fast (Part II)" https://www.paradedb.com/blog/opening-a-closed-tin-part-2
* Geoghegan. "Update on Index Prefetching". PGConf.dev 2026. https://speakerdeck.com/peterg/update-on-index-prefetching ; commitfest patch 4351 https://commitfest.postgresql.org/patch/4351/
* Freund. "AIO in Postgres 18 and beyond". PGConf.EU 2025 (text; the charts were unreadable). https://anarazel.de/talks/2025-10-23-pgconf-eu-aio-in-PG-18-and-beyond/aio-in-PG-18-and-beyond.pdf
* PostgreSQL 18 documentation: index scanning https://www.postgresql.org/docs/current/index-scanning.html ; JIT decision https://www.postgresql.org/docs/18/jit-decision.html ; custom scan execution https://www.postgresql.org/docs/18/custom-scan-execution.html
* Snippet only: Mallia et al. "Faster BlockMax WAND with Variable-sized Blocks" SIGIR 2017 https://dl.acm.org/doi/10.1145/3077136.3080780 ; "Fast Disjunctive Candidate Generation Using Live Block Filtering" WSDM 2021; Siedlaczek et al. "Using Conjunctions for Faster Disjunctive Top-k Queries" WSDM 2022 https://dl.acm.org/doi/10.1145/3488560.3498489 ; Chakrabarti et al. "Interval-based pruning" 2011 https://www.microsoft.com/en-us/research/publication/interval-based-pruning-for-top-k-processing-over-compressed-lists/ ; Bruch et al. "Seismic" 2024 https://arxiv.org/abs/2404.18812 ; CRoaring PR 851 https://github.com/RoaringBitmap/CRoaring/pull/851 ; Elasticsearch PR 37466 https://github.com/elastic/elasticsearch/pull/37466 ; Weaviate BlockMax WAND https://weaviate.io/blog/blockmax-wand ; timescale/pg_textsearch https://github.com/timescale/pg_textsearch
* Not fetched, cited through the papers above: Broder et al. 2003 (WAND); Ding, Suel 2011 (Block-Max WAND); Turtle, Flood 1995 (MaxScore).

## 10. Follow-up: what was implemented (October 2026)

The user approved implementing the findings. Branch `perf/research`, one commit and one regression test per item; every number is user cycles per
transaction of one backend, the same seeded statements for both builds (`bench/steps/ab.sh run ... Nt`), mean +- standard error of 5 interleaved
runs, host load 6 to 12.

| Item | Class | main (b6e7d3b) | this branch |
|---|---|---|---|
| 4. generic plans keep the pushdown, decided at execution | prepared filter | 3.23M +- 0.12M | 1.66M +- 0.06M (-49%) |
| | prepared join | 5.93M +- 0.41M | 4.68M +- 0.37M (-21%) |
| | prepared rare AND with a 2.3% filter | 772k +- 27k | 658k +- 11k (-15%) |
| 5. late stretch of the heap | places p1 filter | 3.52M +- 0.10M | 0.67M +- 0.01M (-81%) |
| | places p2 filter | 1.05M +- 0.03M | 0.68M +- 0.02M (-36%) |
| | clustered article shapes (late, middle) | 2.46M, 1.51M | 0.11M, 0.23M |
| estimate memo | count / filter / mixed / and | 867k / 1.86M / 1.52M / 846k | 761k / 1.77M / 1.45M / 811k |
| over-pull 16x | join | 4.74M +- 0.02M | 3.90M +- 0.05M (-18%) |

**Regression check** (`bench/bench regress --run --ref b6e7d3b`, slim, articles and places, all classes): 51 cells, 0 regressions, 2 improved (places
a2 and w1 boost). The suite's own noise on this loaded host is 70% to 200% for the articles qps cells and 10% to 30% for the places p50 cells,
so it cannot tell 2% from 20%; every class was therefore measured again interleaved (5 repeats, one backend, cycles and instructions per
transaction): no article class and no places top, filter or count cell of the nine classes is worse than main beyond noise. Instructions
fell in all of them (or -3%, phrase -9%, prefix -26%, fuzzy -70%, places top -11% to -33%, count -5% to -44%); the cycle ratio of `term` read 1.05
+- 0.04 in one run and 0.85, 0.99 and 1.12 in three more (its instructions are 3% lower every time). The `update` class has no single-backend
script in `ab.sh` and was measured by the suite only (0.93, standard error 72%).

**Prepared statements, the first version and the fix.** The first version priced a generic plan as if its query matched a tenth of the rows,
which pushed the filter down for common queries but made a prepared rare AND query with a 2.3% filter 5% (cycles) and 12-14% (instructions)
dearer than on main: main's generic plan assumed the operator's default (0.5% of the rows), priced the post-filter scan as nearly free, won
against the custom plans and was reused, while the new plan cost more than the custom plans of rare queries, so PostgreSQL planned it again.
PostgreSQL has no choose-plan node, but the scan knows the query when it starts, so the decision moved there: the generic plan keeps the filter's
bitmap as a child and a window of match counts (`SIFT_PRIV_GATE`, `SIFT_PRIV_GATE_TO`, found by `push_window` from the costs of both paths on a
geometric grid of match counts), and `start_scan` binds the query and builds the bitmap only when `sift_plan_docs` is inside the window. The plan
is priced as main priced it, so the plan cache reuses it as before. Two things the measurements forced: the cost ratio is not monotonic in the
match count (a query that matches so much that the ranker holds its rows after a few candidates is better off without the bitmap), and the
planner's charge per candidate checked (a random page) is above what data in memory costs, so the window opens only where the pushed down plan is
expected to cost a quarter of the other and stays open while it is not dearer: at a margin of one half, a prepared rare AND query with a 10% filter
cost 7% more instructions than main. Shape matrix, 5 interleaved runs, cycles / instructions of the gated build over main (automatic plan
cache; the generic-only and custom-only modes give the same ratios, and the custom plans are the same code):

| query class | filter 0.1% | 2.3% | 10% | 50% |
|---|---|---|---|---|
| and (rare) | 0.69 / 0.74 | 0.89 / 0.88 | 0.97 / 1.00 | 0.95 / 1.01 |
| filter (common) | 0.23 / 0.28 | 0.54 / 0.64 | 1.05 / 1.01 | 0.98 / 0.92 |
| or (common, ranked) | 0.13 / 0.23 | 0.45 / 0.61 | 0.94 / 1.00 | 0.96 / 0.99 |

The other prepared classes (and, or, phrase, mixed, count, filter, join, term, prefix, fuzzy) and the literal ones are within noise or better in
cycles and instructions; `fuzzy_prep`, `or`, and the shapes fc_end5, fc_scat5, fc_grp7 and fk_g7 read 1.1 to 1.3 in some single runs where the
reference had a low outlier or the instructions did not move, and were measured again with 8 runs in both orders: no difference beyond noise.

**Row 3 (join predicate pushed into the scan as a mask) was not implemented.** The oracle of section 6 (-30% for perfect knowledge of the join
selectivity) needs a dimension child plan, a TIDBitmap built at execution from one index probe per join key, a cost model against `only_best`
(a ranked relation keeps one path) and the limit hint; the safe first step, giving the join the right number of rows to hint the scan with,
was built and measured: instructions -9%, cycles +9% (more cache misses), so it was dropped. The over-pull growth (above) took the part of the
gain that is free. The gated bitmap of the prepared filter is the mechanism that would carry the join as well (a child plan that the scan builds
when the bound query is known), but the join differs in three ways that make it a project of its own and not a step: the mask must be the
union of the index probes of the dimension's keys (a different executor node, with a count of keys that is not known at planning time), the
number of keys to probe depends on the join's other side (a nested loop that has not run yet), and the ranked relation keeps one path whatever
the join order (`only_best`), so a masked scan that wins for one tenant cannot be offered next to the plain one for the tenants where the hash
join wins. Without a guard that picks between them at execution by the number of keys, a tenant with many keys pays for a mask it does not need.
Not started: the oracle gain (-30% of the join class) does not pay for the risk of a slower shape.

**Sanitizer finding.** `SANITIZE=1 scripts/ci.sh quick` failed `parallel_scan` on this branch and passed on main. The cause is older than the
branch: `add_parallel_count` passed an uninitialized `AggClauseCosts` to `get_agg_clause_costs`, so the parallel count was priced with stack
garbage (889 or 2684 for the same plan depending on the stack) and sometimes lost to a partial aggregate over a parallel scan. The late-stretch
commit moved the stack. Fixed by zeroing the struct (commit `planner: count:`).

### 10.1 Measurements of the follow-up

User cycles and instructions per transaction, mean +- standard error of 5 interleaved runs of one backend (host load 6 to 22), main = `b6e7d3b` and the gated build; a ratio above 1.02 that is more than two standard errors is marked. Rows with a standard error of a third of the mean or more had an outlier run in one of the builds; the ones that mattered were measured again (section 10).

#### Prepared ranked queries with a filter of 0.1, 2.3, 10 and 50% of the rows, by plan cache mode (`ab.sh filter_matrix`)

| class | filter | plan cache | main cycles | gate cycles | cycles ratio | main instr | gate instr | instr ratio |
|---|---|---|---|---|---|---|---|---|
| and | 0.1% | auto | 920k +- 94k | 635k +- 39k | 0.69 | 1556k +- 13k | 1153k +- 6k | 0.74 |
| and | 0.1% | custom | 1027k +- 84k | 847k +- 52k | 0.82 | 1918k +- 10k | 1523k +- 9k | 0.79 |
| and | 0.1% | generic | 948k +- 61k | 587k +- 41k | 0.62 | 1567k +- 11k | 1161k +- 3k | 0.74 |
| and | 10% | auto | 756k +- 26k | 737k +- 43k | 0.97 | 1367k +- 1k | 1365k +- 5k | 1.00 |
| and | 10% | custom | 1227k +- 65k | 1098k +- 44k | 0.89 | 2205k +- 6k | 1833k +- 21k | 0.83 |
| and | 10% | generic | 761k +- 46k | 716k +- 34k | 0.94 | 1357k +- 9k | 1369k +- 1k | 1.01 |
| and | 2.3% | auto | 897k +- 85k | 801k +- 25k | 0.89 | 1451k +- 11k | 1283k +- 8k | 0.88 |
| and | 2.3% | custom | 990k +- 250k | 1147k +- 67k | 1.16 | 1634k +- 409k | 1702k +- 1k | 1.04 |
| and | 2.3% | generic | 981k +- 37k | 649k +- 167k | 0.66 | 1459k +- 7k | 1041k +- 260k | 0.71 |
| and | 50% | auto | 760k +- 25k | 719k +- 30k | 0.95 | 1269k +- 11k | 1278k +- 6k | 1.01 |
| and | 50% | custom | 1184k +- 24k | 961k +- 43k | 0.81 | 1980k +- 7k | 1643k +- 2k | 0.83 |
| and | 50% | generic | 709k +- 35k | 702k +- 44k | 0.99 | 1275k +- 5k | 1266k +- 13k | 0.99 |
| filter | 0.1% | auto | 4168k +- 186k | 953k +- 11k | 0.23 | 8608k +- 1k | 2399k +- 11k | 0.28 |
| filter | 0.1% | custom | 1286k +- 48k | 1201k +- 20k | 0.93 | 3143k +- 10k | 2797k +- 2k | 0.89 |
| filter | 0.1% | generic | 4051k +- 170k | 950k +- 22k | 0.23 | 8633k +- 1k | 2383k +- 14k | 0.28 |
| filter | 10% | auto | 2024k +- 129k | 2116k +- 108k | 1.05 | 4034k +- 15k | 4064k +- 4k | 1.01 |
| filter | 10% | custom | 2425k +- 94k | 2501k +- 115k | 1.03 | 4842k +- 3k | 4508k +- 7k | 0.93 |
| filter | 10% | generic | 2080k +- 168k | 2081k +- 94k | 1.00 | 4057k +- 11k | 4062k +- 3k | 1.00 |
| filter | 2.3% | auto | 2715k +- 56k | 1458k +- 35k | 0.54 | 5276k +- 4k | 3370k +- 11k | 0.64 |
| filter | 2.3% | custom | 1951k +- 62k | 1801k +- 52k | 0.92 | 4124k +- 5k | 3769k +- 8k | 0.91 |
| filter | 2.3% | generic | 2727k +- 77k | 1512k +- 42k | 0.55 | 5287k +- 1k | 3367k +- 19k | 0.64 |
| filter | 50% | auto | 1991k +- 113k | 1948k +- 98k | 0.98 | 3910k +- 14k | 3594k +- 1k | 0.92 |
| filter | 50% | custom | 1949k +- 129k | 1975k +- 87k | 1.01 | 3984k +- 12k | 3663k +- 2k | 0.92 |
| filter | 50% | generic | 1669k +- 66k | 1656k +- 90k | 0.99 | 3302k +- 2k | 3299k +- 5k | 1.00 |
| or | 0.1% | auto | 18732k +- 1501k | 2451k +- 57k | 0.13 | 22119k +- 4k | 5137k +- 10k | 0.23 |
| or | 0.1% | custom | 2421k +- 614k | 2925k +- 158k | 1.21 | 4755k +- 1189k | 5571k +- 2k | 1.17 |
| or | 0.1% | generic | 17454k +- 1170k | 1998k +- 503k | 0.11 | 22201k +- 8k | 4124k +- 1031k | 0.19 |
| or | 10% | auto | 5613k +- 306k | 5293k +- 315k | 0.94 | 9349k +- 8k | 9361k +- 2k | 1.00 |
| or | 10% | custom | 5108k +- 1289k | 5682k +- 333k | 1.11 | 8198k +- 2050k | 9905k +- 6k | 1.21 |
| or | 10% | generic | 5421k +- 348k | 5302k +- 350k | 0.98 | 9342k +- 9k | 9357k +- 1k | 1.00 |
| or | 2.3% | auto | 8499k +- 581k | 3837k +- 174k | 0.45 | 12714k +- 11k | 7742k +- 13k | 0.61 |
| or | 2.3% | custom | 4522k +- 213k | 4214k +- 233k | 0.93 | 8526k +- 10k | 8143k +- 10k | 0.96 |
| or | 2.3% | generic | 8394k +- 511k | 3739k +- 222k | 0.45 | 12775k +- 5k | 7753k +- 2k | 0.61 |
| or | 50% | auto | 3159k +- 51k | 3040k +- 53k | 0.96 | 7319k +- 2k | 7274k +- 12k | 0.99 |
| or | 50% | custom | 3798k +- 230k | 3363k +- 122k | 0.89 | 7977k +- 14k | 7651k +- 15k | 0.96 |
| or | 50% | generic | 3067k +- 72k | 3129k +- 146k | 1.02 | 7299k +- 6k | 7280k +- 14k | 1.00 |

#### Every class and shape (`ab.sh run`; _prep: prepared statements; fixed number of seeded statements)

| shape | main cycles | gate cycles | cyc ratio | main instr | gate instr | instr ratio |
|---|---|---|---|---|---|---|
| articles and | 938k +- 37k | 971k +- 91k | 1.03 | 1821k +- 5k | 1598k +- 3k | 0.88 |
| articles or | 1988k +- 843k | 3360k +- 307k | 1.69 | 4398k +- 1796k | 7104k +- 9k | 1.62 |
| articles phrase | 945k +- 27k | 946k +- 41k | 1.00 | 1986k +- 1k | 1801k +- 1k | 0.91 |
| articles mixed | 1863k +- 61k | 1784k +- 31k | 0.96 | 3681k +- 7k | 3455k +- 5k | 0.94 |
| articles count | 1275k +- 96k | 1237k +- 73k | 0.97 | 2162k +- 5k | 1851k +- 4k | 0.86 |
| articles filter | 2118k +- 183k | 2036k +- 110k | 0.96 | 4254k +- 3k | 3888k +- 9k | 0.91 |
| articles join | 5581k +- 293k | 4510k +- 281k | 0.81 | 11300k +- 6k | 8455k +- 1k | 0.75 |
| articles term | 991k +- 45k | 1007k +- 71k | 1.02 | 2184k +- 8k | 2131k +- 3k | 0.98 |
| articles prefix | 1591k +- 114k | 1303k +- 105k | 0.82 | 3230k +- 11k | 2406k +- 3k | 0.74 |
| articles fuzzy | 13634k +- 335k | 4388k +- 193k | 0.32 | 38867k +- 5k | 11758k +- 3k | 0.30 |
| articles and_prep | 797k +- 20k | 705k +- 9k | 0.89 | 1712k +- 1k | 1492k +- 2k | 0.87 |
| articles or_prep | 2855k +- 178k | 2757k +- 36k | 0.97 | 6767k +- 4k | 6776k +- 1k | 1.00 |
| articles phrase_prep | 975k +- 56k | 922k +- 45k | 0.95 | 1853k +- 13k | 1687k +- 5k | 0.91 |
| articles mixed_prep | 1615k +- 80k | 1644k +- 75k | 1.02 | 3573k +- 1k | 3357k +- 10k | 0.94 |
| articles count_prep | 694k +- 24k | 670k +- 15k | 0.97 | 1550k +- 4k | 1556k +- 2k | 1.00 |
| articles filter_prep | 3232k +- 123k | 1656k +- 64k | 0.51 | 5445k +- 6k | 3452k +- 0k | 0.63 |
| articles join_prep | 5927k +- 411k | 4683k +- 371k | 0.79 | 11177k +- 3k | 8323k +- 5k | 0.74 |
| articles term_prep | 588k +- 20k | 577k +- 8k | 0.98 | 1900k +- 1k | 1876k +- 16k | 0.99 |
| articles prefix_prep | 951k +- 40k | 969k +- 36k | 1.02 | 2170k +- 9k | 2175k +- 4k | 1.00 |
| articles fuzzy_prep | 3766k +- 991k | 4900k +- 263k | 1.30 | 9261k +- 2315k | 11512k +- 9k | 1.24 |
| articles ab_and_filter_prep | 772k +- 27k | 658k +- 11k | 0.85 | 1472k +- 1k | 1306k +- 4k | 0.89 |
| places top p1 | 811k +- 22k | 745k +- 30k | 0.92 | 1310k +- 4k | 1174k +- 8k | 0.90 |
| places top p2 | 764k +- 47k | 700k +- 50k | 0.92 | 1433k +- 4k | 1313k +- 8k | 0.92 |
| places top p3 | 505k +- 44k | 443k +- 14k | 0.88 | 1037k +- 2k | 906k +- 18k | 0.87 |
| places top p4 | 630k +- 40k | 516k +- 15k | 0.82 | 1123k +- 1k | 903k +- 9k | 0.80 |
| places top w1 | 365k +- 11k | 293k +- 15k | 0.80 | 737k +- 2k | 501k +- 14k | 0.68 |
| places top a2 | 590k +- 60k | 475k +- 35k | 0.81 | 962k +- 3k | 722k +- 12k | 0.75 |
| places top a2s | 616k +- 50k | 497k +- 34k | 0.81 | 950k +- 1k | 725k +- 7k | 0.76 |
| places top a3 | 579k +- 55k | 477k +- 47k | 0.82 | 1130k +- 3k | 764k +- 2k | 0.68 |
| places top x2 | 677k +- 59k | 604k +- 45k | 0.89 | 962k +- 10k | 725k +- 4k | 0.75 |
| places filter p1 | 3832k +- 200k | 723k +- 18k | 0.19 | 12087k +- 2k | 1916k +- 9k | 0.16 |
| places filter p2 | 1446k +- 143k | 826k +- 67k | 0.57 | 2855k +- 15k | 1908k +- 2k | 0.67 |
| places filter p3 | 724k +- 29k | 686k +- 45k | 0.95 | 1720k +- 4k | 1465k +- 7k | 0.85 |
| places filter p4 | 781k +- 59k | 647k +- 35k | 0.83 | 1570k +- 3k | 1215k +- 12k | 0.77 |
| places filter w1 | 744k +- 99k | 539k +- 156k | 0.72 | 1078k +- 4k | 634k +- 159k | 0.59 |
| places filter a2 | 928k +- 103k | 672k +- 101k | 0.72 | 1283k +- 4k | 907k +- 6k | 0.71 |
| places filter a2s | 636k +- 28k | 488k +- 13k | 0.77 | 1290k +- 3k | 927k +- 6k | 0.72 |
| places filter a3 | 747k +- 25k | 579k +- 16k | 0.78 | 1442k +- 3k | 913k +- 2k | 0.63 |
| places filter x2 | 879k +- 115k | 812k +- 99k | 0.92 | 1262k +- 11k | 889k +- 6k | 0.70 |
| places count p1 | 414k +- 70k | 373k +- 80k | 0.90 | 658k +- 11k | 537k +- 6k | 0.82 |
| places count p2 | 256k +- 21k | 264k +- 27k | 1.03 | 642k +- 41k | 541k +- 16k | 0.84 |
| places count p3 | 210k +- 12k | 196k +- 13k | 0.93 | 503k +- 16k | 399k +- 6k | 0.79 |
| places count p4 | 352k +- 21k | 284k +- 19k | 0.81 | 692k +- 6k | 477k +- 3k | 0.69 |
| places count w1 | 267k +- 21k | 229k +- 29k | 0.86 | 572k +- 3k | 398k +- 2k | 0.70 |
| places count a2 | 330k +- 8k | 274k +- 12k | 0.83 | 797k +- 1k | 534k +- 3k | 0.67 |
| places count a2s | 352k +- 21k | 301k +- 44k | 0.86 | 745k +- 4k | 510k +- 3k | 0.68 |
| places count a3 | 423k +- 28k | 313k +- 14k | 0.74 | 1009k +- 3k | 588k +- 2k | 0.58 |
| places count x2 | 403k +- 32k | 321k +- 38k | 0.80 | 834k +- 4k | 540k +- 2k | 0.65 |
| shape fc_end5 | 276k +- 88k | 304k +- 36k | 1.10 | 588k +- 153k | 634k +- 54k | 1.08 |
| shape fc_grp7 | 808k +- 79k | 872k +- 69k | 1.08 | 1398k +- 29k | 1393k +- 12k | 1.00 |
| shape fc_last70 | 1865k +- 167k | 1755k +- 149k | 0.94 | 4979k +- 28k | 4869k +- 29k | 0.98 |
| shape fc_scat30 | 454k +- 17k | 455k +- 55k | 1.00 | 1258k +- 24k | 1104k +- 57k | 0.88 |
| shape fc_scat5 | 800k +- 204k | 910k +- 39k | 1.14 | 1405k +- 351k | 1516k +- 45k | 1.08 |
| shape fc_start5 | 654k +- 45k | 566k +- 11k | 0.87 | 1180k +- 19k | 1028k +- 7k | 0.87 |
| shape fk_every | 847k +- 85k | 820k +- 88k | 0.97 | 1840k +- 13k | 1849k +- 13k | 1.00 |
| shape fk_g0 | 573k +- 44k | 435k +- 111k | 0.76 | 1314k +- 52k | 1015k +- 257k | 0.77 |
| shape fk_g1 | 1801k +- 45k | 336k +- 138k | 0.19 | 4275k +- 56k | 592k +- 242k | 0.14 |
| shape fk_g2 | 2471k +- 301k | 376k +- 37k | 0.15 | 7347k +- 22k | 989k +- 24k | 0.13 |
| shape fk_g3 | 2690k +- 138k | 348k +- 17k | 0.13 | 10122k +- 62k | 872k +- 16k | 0.09 |
| shape fk_g4 | 1418k +- 36k | 463k +- 32k | 0.33 | 3298k +- 70k | 959k +- 42k | 0.29 |
| shape fk_g5 | 2849k +- 260k | 333k +- 90k | 0.12 | 6237k +- 80k | 744k +- 194k | 0.12 |
| shape fk_g6 | 3582k +- 311k | 458k +- 71k | 0.13 | 9371k +- 22k | 1101k +- 46k | 0.12 |
| shape fk_g7 | 632k +- 11k | 757k +- 91k | 1.20 | 2309k +- 44k | 2262k +- 33k | 0.98 |
| shape fk_g8 | 1646k +- 127k | 407k +- 33k | 0.25 | 5352k +- 30k | 1265k +- 8k | 0.24 |
| shape fk_g9 | 2905k +- 166k | 383k +- 27k | 0.13 | 8351k +- 41k | 1038k +- 9k | 0.12 |
