# Benchmarks

sift against ParadeDB pg_search 0.26.1 (and sift with `phrase_index = pairs`) on two public datasets, measured with
`bench/bench run` (articles through `bench/steps/ab.sh`, places through `bench/steps/places.sh`; method, datasets, licenses
and checksums: `bench/README.md`). Every engine indexes the same rows with the same analysis (lowercase, no stemming,
no stopwords) and answers the same queries from the same cache state; cells are run one at a time with the engine
order rotated every round, and the tables report medians over the rounds. rum 1.3.15 is part of the harness
(`--engines sift,sift_pairs,pg_search,rum`); its numbers are not in these tables. The numbers of every table below are
also in [results/articles.tsv](results/articles.tsv) and [results/places.tsv](results/places.tsv), one row per
measurement (columns: `bench/README.md`), for charts.

Host: AMD Ryzen 7 9700X (8 cores, 16 hardware threads), 188 GB RAM, Linux 7.2, PostgreSQL 18.6, scratch data on tmpfs,
engine servers in a cgroup scope of 8 vCPUs and 32 GB (sift built from commit `ac2b4bb`, pg_search from the ParadeDB 0.26.1
package), shared with other jobs. The 1-minute load average during the measurements was 16-46 (median 22; articles,
full tier), 4-29 (places, full), 4-25 (articles, 150k) and 16-30 (places, 150k tier), so absolute numbers carry noise
and the per-round ranges in the tables are wide; no round ever reached the 15% spread that ends a run early. Compare
engines within a table, not across tables or hosts. Index sizes and build times of every table are for one build per
engine.

## Articles: Simple English Wikipedia, 218,058 articles (full tier)

8 clients, 6 interleaved rounds of 8 s per cell (2 s warm-up), queries are 2-15 consecutive words of random
articles (1000 each as AND, OR and phrase); `bench/bench run --dataset articles --tier full`, load average 16-46. Throughput
in queries per second, median (minimum-maximum over the rounds), with the size of the index and the time of its `CREATE INDEX`:

| engine | index (MiB) | build (s) | and (top-10) | or (top-10) | phrase (top-10) | mixed (and, or, phrase) | count(*) | filter (top-10, newest 2%) | join (top-10 with `categories`) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 47.9 | 1.7 | 7637 (3137-8082) | 1697 (686-1819) | 1869 (737-2054) | 2528 (1913-2682) | 1078 (929-1157) | 2462 (949-2548) | 692 (339-796) |
| sift `pairs` | 267.2 | 10.6 | 7039 (2932-7918) | 1778 (946-1822) | 9058 (3415-9363) | 3652 (2339-3842) | 4936 (2035-5635) | 2864 (2348-2941) | 1391 (441-1443) |
| pg_search | 183.4 | 4.0 | 3245 (2352-3310) | 920 (526-1020) | 2531 (802-2671) | 1419 (1256-1515) | 1537 (657-1651) | 2477 (2142-2564) | 186 (87-192) |

Ratio of queries per second to pg_search:

| engine | and | or | phrase | mixed | count(*) | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift / pg_search | 2.35 | 1.84 | **0.74** | 1.78 | **0.70** | 0.99 | 3.72 |
| sift `pairs` / pg_search | 2.17 | 1.93 | 3.58 | 2.57 | 3.21 | 1.16 | 7.48 |

Index size and build time move with the build: pg_search's size depends on how its parallel workers split the segments
(164-185 MiB over the builds of this and earlier sessions), build times move with load; sift's sizes do not.

Cold cache (index, heap and TOAST evicted from shared buffers before every query; `EXPLAIN ANALYZE` execution time
of 12 queries per class, `bench/bench cold --dataset articles --tier full`), p50 / maximum in ms. The scratch data is on
tmpfs, so there is no disk behind the evicted pages: this is the cost of rebuilding the buffers from memory-backed files,
not of disk reads.

| class | sift | sift `pairs` | pg_search |
|---|---:|---:|---:|
| phrase | 1.50 / 84.25 | 1.58 / 3.78 | 5.00 / 23.37 |
| count(*) | 1.18 / 659.97 | 0.62 / 2.20 | 2.84 / 17.94 |
| and | 1.51 / 3.98 | 1.41 / 5.60 | 2.93 / 4.29 |

Reading the numbers:

- AND, OR, mixed and the join are about 1.8-3.7 times faster than pg_search with the default index, which is also
  3.8 times smaller (47.9 against 183.4 MiB) and builds 2.4 times faster.
- Without `pairs`, a phrase is checked against the heap after the term intersection, and the check (decompressing and
  tokenizing the candidates) dominates when the words are common: phrase and `count(*)` are at 0.74x and 0.70x of
  pg_search (0.81x and 0.82x in the previous session; the per-round ranges overlap, so read it as 0.7-0.8x), and the cold
  maximum of the default index shows the same cost. `phrase_index = pairs` removes it and wins every class, but its index
  is 5.6 times the default one and 1.46 times pg_search's (267.2 against 183.4 MiB; 1.4-1.6 times over the builds of
  the sessions), and it takes 10.6 s to build against 4.0 s for pg_search: sift with `pairs` is the larger index.
- The recheck used to be slower. Profiling the phrase class (perf, one client) put 38% of the CPU in the per-document
  term table that the recheck built before looking for the phrase; the recheck now scans the document's tokens and builds
  the table only for the other leaf kinds. The same measurement with fixed instruction counts: phrase 48.9M to 36.6M
  instructions per query (7.0 to 4.7 ms CPU), `count(*)` 46.0M to 33.9M (6.4 to 3.8 ms), mixed 39.4M to 34.2M, and 8.2M
  (unchanged) for AND. The interleaved run before the change measured phrase 743 against pg_search 1478 (0.50x) and
  `count(*)` 391 against 747 (0.52x). What is left of the phrase cost is tokenizing (30% of the CPU) and decompressing
  the TOASTed text (21%).
- The filter class is level with pg_search (0.99x with the default index, 1.16x with `pairs`; 1.06x and 1.11x in the
  previous session).

### 150,000 articles (`ab` tier)

`bench/bench run --dataset articles --tier ab`: 3 s per cell, 6 rounds (load 4-25). Same classes, queries per second
(minimum-maximum over the rounds):

| engine | index (MiB) | build (s) | and | or | phrase | mixed | count(*) | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 33.5 | 1.0 | 7293 (6271-14211) | 2400 (1737-3684) | 2663 (2104-2866) | 3246 (2678-4068) | 1554 (1386-2298) | 2669 (2149-4697) | 990 (750-1726) |
| sift `pairs` | 192.2 | 5.3 | 8351 (4923-14092) | 2235 (1821-3498) | 9872 (6345-12419) | 3967 (3430-6326) | 9896 (7417-14005) | 3311 (2607-5188) | 1749 (1524-2927) |
| pg_search | 115.8 | 1.9 | 2865 (2384-4930) | 1069 (815-1289) | 2399 (2114-3350) | 1489 (1122-1721) | 1708 (1432-1927) | 2385 (1959-3851) | 239 (227-318) |

| ratio to pg_search | and | or | phrase | mixed | count(*) | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift | 2.55 | 2.24 | 1.11 | 2.18 | 0.91 | 1.12 | 4.14 |
| sift `pairs` | 2.91 | 2.09 | 4.12 | 2.66 | 5.80 | 1.39 | 7.31 |

The 150k tier has the full tier's ordering for every class and for `pairs` everywhere; the exception is the default
index's `count(*)` (0.91x) and, less clearly, its phrase (1.11x), which are behind pg_search at 218k articles (0.74x,
0.70x). The heap recheck grows with the corpus, so the crossover depends on the size: with an earlier build and a
75,000 article tier the ratios were 1.22x and 1.35x, and with the 25,000 article tier of the first harness version 1.11x
and 1.54x. Judge the default index's phrase and `count(*)` on the full tier. The size ordering is the same as at 218k:
`pairs` is 192.2 MiB against 115.8 MiB for pg_search (1.66 times) and 33.5 MiB for the default index.

## Places: GeoNames cities500, 236,262 places (full tier)

Multi-field autocomplete as an application would run it: sift indexes the name section and the all-fields text with
`fields = 'name', prefix_index = 3`, pg_search indexes `edge_ngram(1, 15)` fields for both; every typed word is a
prefix. `sift.max_expansions` is set to 100000 for the database: at the default 512, prefixes of four or more letters
of common words (`pari*`, `san*`) raise the expansion error on this corpus. Single client, 6 interleaved rounds of 4 s
per cell (`bench/bench run --dataset places --tier full`, load 4-29). Index size, build time and p50 latency in ms of the
top-20 workload per query class:

| engine | index (MiB) | build (s) | p1 | p2 | p3 | p4 | w1 | a2 | a2s | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 42.6 | 1.5 | 0.71 | 0.56 | 0.37 | 0.34 | 0.21 | 0.27 | 0.34 | 0.31 | 0.29 |
| pg_search | 140.5 | 3.5 | 0.92 | 1.09 | 1.22 | 1.26 | 1.34 | 1.81 | 1.56 | 3.00 | 2.14 |

All workloads, p50 latency in ms, `pg_search / sift (ratio)`:

| class | top-20 | name boost x3 | filtered (`country_code = 'US'`) | count(*) | unordered (`population >= 100000`) |
|---|---:|---:|---:|---:|---:|
| p1 (1 letter) | 0.92 / 0.71 (1.3x) | 3.47 / 1.26 (2.8x) | 12.78 / 8.68 (1.5x) | 1.30 / 0.57 (2.3x) | 16.00 / 11.26 (1.4x) |
| p2 (2 letters) | 1.09 / 0.56 (1.9x) | 3.07 / 0.82 (3.7x) | 8.32 / 2.69 (3.1x) | 1.36 / 0.40 (3.4x) | 4.74 / 2.61 (1.8x) |
| p3 (3 letters) | 1.22 / 0.37 (3.3x) | 2.81 / 0.45 (6.2x) | 6.62 / 0.70 (9.5x) | 1.31 / 0.26 (5.0x) | 1.74 / 0.54 (3.2x) |
| p4 (4 letters) | 1.26 / 0.34 (3.7x) | 2.45 / 0.53 (4.6x) | 5.69 / 0.42 (13.5x) | 1.39 / 0.36 (3.9x) | 1.27 / 0.29 (4.4x) |
| w1 (whole word) | 1.34 / 0.21 (6.4x) | 2.54 / 0.23 (11.0x) | 5.52 / 0.24 (23.0x) | 1.83 / 0.24 (7.6x) | 1.68 / 0.19 (8.8x) |
| a2 (word, 2 letters) | 1.81 / 0.27 (6.7x) | 3.54 / 0.38 (9.3x) | 8.27 / 0.32 (25.8x) | 2.18 / 0.27 (8.1x) | 1.93 / 0.21 (9.2x) |
| a2s (word, 1 letter) | 1.56 / 0.34 (4.6x) | 3.09 / 0.61 (5.1x) | 8.64 / 0.46 (18.8x) | 2.02 / 0.28 (7.2x) | 1.81 / 0.22 (8.2x) |
| a3 (name, region, country) | 3.00 / 0.31 (9.7x) | 5.97 / 0.45 (13.3x) | 7.71 / 0.33 (23.4x) | 4.07 / 0.28 (14.5x) | 3.53 / 0.22 (16.0x) |
| x2 (name, country) | 2.14 / 0.29 (7.4x) | 4.06 / 0.48 (8.5x) | 7.85 / 0.35 (22.4x) | 2.94 / 0.28 (10.5x) | 2.70 / 0.24 (11.3x) |

Places queries are prefixes and ANDs and never reach the phrase recheck.

Cold cache (`bench/bench cold --dataset places --tier full`, 12 queries per cell, scratch data on tmpfs as above), p50 /
maximum in ms:

| workload | class | pg_search | sift |
|---|---|---:|---:|
| top-20 | p1 | 2.35 / 5.40 | 1.65 / 5.04 |
| top-20 | p2 | 2.17 / 4.82 | 1.33 / 5.29 |
| top-20 | a2 | 2.78 / 4.31 | 0.30 / 3.31 |
| top-20 | a3 | 4.09 / 8.65 | 0.34 / 0.79 |
| boost | p1 | 4.96 / 8.32 | 2.72 / 6.07 |
| boost | p2 | 6.98 / 17.02 | 1.93 / 4.54 |
| boost | a2 | 5.04 / 10.21 | 0.35 / 9.58 |
| boost | a3 | 6.99 / 8.13 | 0.53 / 2.33 |
| filter | p1 | 27.81 / 35.07 | 19.50 / 21.94 |
| filter | p2 | 22.50 / 43.93 | 9.87 / 16.77 |
| filter | a2 | 9.94 / 17.82 | 0.23 / 3.62 |
| filter | a3 | 11.81 / 30.55 | 0.33 / 0.61 |
| count | p1 | 3.53 / 5.31 | 2.18 / 5.98 |
| count | p2 | 2.71 / 5.36 | 0.97 / 2.03 |
| count | a2 | 3.01 / 4.34 | 0.32 / 2.26 |
| count | a3 | 5.35 / 6.06 | 0.36 / 0.66 |

sift is ahead in every warm cell (45 of 45; smallest ratio 1.3x) and in every cold cell. The closest are the one-letter
prefixes (p1), where the result is a large share of the corpus and both engines spend their time on ranking and fetching
rows, and the unordered scan of p1/p2. The index is 3.3 times smaller (42.6 against 140.5 MiB) and builds 2.3 times faster.

### 34,165 places (`ab` tier, cities15000)

`bench/bench run --dataset places --tier ab`: 5 rounds of 2 s per cell (load 16-30). Index size and build time: sift 13.7 MiB
and 0.6 s, pg_search 27.8 MiB and 2.6 s; p50 latency of the top-20 workload in ms:

| engine | p1 | p3 | w1 | a2 | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|
| sift | 0.48 | 0.31 | 0.23 | 0.23 | 0.29 | 0.27 |
| pg_search | 0.71 | 0.79 | 0.99 | 1.20 | 1.73 | 1.33 |

The 34k tier has the same winner as the full tier in all 24 cells they share (smallest ratio 1.5x), with smaller ratios in
20 of them (pg_search's latency grows with the corpus faster than sift's). All cells are in `results/places.tsv`.

## Harness run time

| | before (review-text corpus) | now |
|---|---|---|
| per interleaved round, 3 engines | about 5 min 11 s for 6 classes at 10 s per cell, plus about 35 s for the join | about 3 min 30 s for 7 classes including the join (8 s per cell and 2 s warm-up; 4 min 50 s with rum in the round) |
| corpus and indexes | about 20 s corpus load per run | restore of the cached dump 8 s, each index built once (16 s for the three at 218k articles) |
| full articles run | not repeatable in one command | 6 rounds with four engines (rum measured but not reported): 31 min (load 16-46); early stop when every cell is within 15% |
| 150k articles run | | 8 min 47 s for 6 rounds, three engines (load 4-25) |
| places | | 150k tier 8 min 11 s (5 rounds), full 36 min 17 s (6 rounds), two engines |

Under this host's load no run reached the early-stop spread; on a quiet host the minimum of four rounds is about 15 min
(full) and 6 min (150k tier). Results are kept as small tables per run (`cells.tsv`, `builds.tsv`, `summary.tsv`,
`machine.txt` with the CPU, engine versions and load) in the result directory, and the scratch cluster is removed when
the run ends.
