# Benchmarks

sift against ParadeDB pg_search 0.26.1 (and sift with `phrase_index = pairs`) on two public datasets, measured with
`bench/ab.sh` and `bench/places.sh` (method, datasets, licenses and checksums: `bench/README.md`). Every engine
indexes the same rows with the same analysis (lowercase, no stemming, no stopwords) and answers the same
queries from the same cache state; cells are run one at a time with the engine order rotated every round, and
the tables report medians over the rounds.

Host: 16 hardware threads, 188 GB RAM, PostgreSQL 18.6, shared with other jobs. The 1-minute load average during
the measurements was 38-58 (articles) and 19-49 (places), so absolute numbers carry noise and no round ever reached
the 15% spread that ends a run early; compare engines within a table, not across tables or hosts.

## Articles: Simple English Wikipedia, 218,058 articles (full tier)

8 clients, 6 interleaved rounds of 8 s per cell (2 s warm-up), queries are 2-15 consecutive words of random
articles (1000 each as AND, OR and phrase); `bench/ab.sh full`, load average 38-58. Throughput in queries per second,
median (minimum-maximum over the rounds):

| class | sift | sift `pairs` | pg_search | sift / pg_search | pairs / pg_search |
|---|---:|---:|---:|---:|---:|
| and (top-10) | 3692 (2440-4072) | 3715 (2890-4007) | 1605 (1463-1945) | 2.30 | 2.32 |
| or (top-10) | 912 (732-977) | 886 (740-991) | 362 (286-394) | 2.52 | 2.45 |
| phrase (top-10) | 974 (768-1168) | 4014 (2735-4505) | 1209 (754-1326) | **0.81** | 3.32 |
| mixed (and, or, phrase) | 1236 (953-1334) | 1682 (1340-2061) | 507 (375-612) | 2.44 | 3.32 |
| count(*) | 619 (419-696) | 2518 (1491-2890) | 753 (574-778) | **0.82** | 3.34 |
| filter (top-10, newest 2%) | 1294 (952-1575) | 1356 (1238-1695) | 1223 (956-1348) | 1.06 | 1.11 |
| join (top-10 with `categories`, region filter) | 419 (335-475) | 739 (498-818) | 110 (76-125) | 3.82 | 6.74 |

Index size and build time (one build per engine, parallel workers; three builds on the same host at different loads):

| index | build | size |
|---|---:|---:|
| sift | 2.3-4.2 s | 48 MiB |
| sift `phrase_index = pairs` | 13.0-20.4 s | 267 MiB |
| pg_search bm25 | 5.8-9.4 s | 164-185 MiB |

Build times move with load; sizes do not (pg_search's depends on how its parallel workers split the segments).

Cold cache (index, heap and TOAST evicted from shared buffers and the OS cache before every query; `EXPLAIN ANALYZE`
execution time of 12 queries per class, `bench/ab.sh cold full`), p50 / maximum in ms:

| class | sift | sift `pairs` | pg_search |
|---|---:|---:|---:|
| phrase | 5.18 / 109.11 | 2.24 / 5.58 | 6.60 / 22.35 |
| count(*) | 1.33 / 691.19 | 0.77 / 17.96 | 3.80 / 25.37 |
| and | 1.74 / 10.88 | 3.19 / 6.10 | 4.49 / 14.80 |

Reading the numbers:

- AND, OR, mixed and the join are about 2.3-3.8 times faster than pg_search with the default index, which is also
  three times smaller.
- Without `pairs`, a phrase is checked against the heap after the term intersection, and the check (decompressing and
  tokenizing the candidates) dominates when the words are common: phrase and `count(*)` stay at 0.81x and 0.82x of
  pg_search, and the cold maximum of the default index shows the same cost. `phrase_index = pairs` removes it and
  wins every class, for 5.5 times the default index size (1.4-1.6 times pg_search's).
- The recheck used to be slower. Profiling the phrase class (perf, one client) put 38% of the CPU in the per-document
  term table that the recheck built before looking for the phrase; the recheck now scans the document's tokens and builds
  the table only for the other leaf kinds. The same measurement with fixed instruction counts: phrase 48.9M to 36.6M
  instructions per query (7.0 to 4.7 ms CPU), `count(*)` 46.0M to 33.9M (6.4 to 3.8 ms), mixed 39.4M to 34.2M, and 8.2M
  (unchanged) for AND. The interleaved run before the change measured phrase 743 against pg_search 1478 (0.50x) and
  `count(*)` 391 against 747 (0.52x). What is left of the phrase cost is tokenizing (30% of the CPU) and decompressing
  the TOASTed text (21%).
- The filter class is level with pg_search (1.06x with the default index, 1.11x with `pairs`; 0.93x and 1.17x in the
  earlier run).

### Quick tier (150,000 articles)

`bench/ab.sh quick`: 3 s per cell, 6 rounds (load 41-56). Same classes, queries per second and ratio to pg_search:

| class | sift | sift `pairs` | pg_search | sift / pg_search | pairs / pg_search |
|---|---:|---:|---:|---:|---:|
| and | 4116 | 4841 | 1617 | 2.54 | 2.99 |
| or | 1151 | 1235 | 452 | 2.54 | 2.73 |
| phrase | 1334 | 4483 | 1388 | 0.96 | 3.23 |
| mixed | 1846 | 2347 | 588 | 3.14 | 3.99 |
| count(*) | 986 | 5193 | 863 | 1.14 | 6.02 |
| filter | 1473 | 1821 | 1314 | 1.12 | 1.39 |
| join | 664 | 1026 | 145 | 4.58 | 7.08 |

The quick tier has the full tier's ordering for every class and for `pairs` everywhere; the exceptions are the default
index's phrase and `count(*)`, which are behind pg_search at 218k articles (0.81x, 0.82x) and level or ahead at 150k
(0.96x, 1.14x). The heap recheck grows with the corpus, so the crossover depends on the size: with the same build and a
75,000 article quick tier the ratios were 1.22x and 1.35x, and with the 25,000 article tier of the first harness version
1.11x and 1.54x. Judge the default index's phrase and `count(*)` on the full tier. Index sizes at 150k: 33.5 MiB (sift),
192.2 MiB (`pairs`), 129.8 MiB (pg_search).

## Places: GeoNames cities500, 236,192 places (full tier)

Multi-field autocomplete as an application would run it: sift indexes the name section and the all-fields text with
`fields = 'name', prefix_index = 3`, pg_search indexes `edge_ngram(1, 15)` fields for both; every typed word is a
prefix. `sift.max_expansions` is set to 100000 for the database: at the default 512, prefixes of four or more letters
of common words (`pari*`, `san*`) raise the expansion error on this corpus. Single client, p50 latency in ms,
`pg_search / sift (ratio)`, 6 interleaved rounds of 4 s per cell (`bench/places.sh full`, load 19-49):

| class | top-20 | name boost x3 | filtered (`country_code = 'US'`) | count(*) | unordered (`population >= 100000`) |
|---|---:|---:|---:|---:|---:|
| p1 (1 letter) | 1.44 / 1.05 (1.4x) | 6.64 / 2.23 (3.0x) | 32.13 / 14.44 (2.2x) | 1.30 / 0.57 (2.3x) | 27.29 / 20.17 (1.4x) |
| p2 (2 letters) | 1.82 / 0.82 (2.2x) | 5.92 / 1.32 (4.5x) | 22.46 / 4.58 (4.9x) | 1.90 / 0.40 (4.8x) | 10.32 / 4.56 (2.3x) |
| p3 (3 letters) | 2.07 / 0.50 (4.1x) | 5.42 / 0.69 (7.9x) | 30.63 / 1.19 (25.7x) | 2.02 / 0.26 (7.7x) | 3.63 / 1.00 (3.6x) |
| p4 (4 letters) | 1.89 / 0.46 (4.1x) | 4.63 / 0.91 (5.1x) | 30.67 / 0.62 (49.5x) | 2.37 / 0.33 (7.2x) | 2.32 / 0.39 (5.9x) |
| w1 (whole word) | 2.07 / 0.26 (7.9x) | 5.15 / 0.32 (15.9x) | 24.63 / 0.33 (75.2x) | 3.43 / 0.27 (12.8x) | 2.92 / 0.25 (11.7x) |
| a2 (word, 2 letters) | 2.97 / 0.35 (8.4x) | 6.54 / 0.55 (12.0x) | 19.29 / 0.36 (52.9x) | 3.98 / 0.30 (13.2x) | 3.59 / 0.26 (13.7x) |
| a2s (word, 1 letter) | 3.25 / 0.55 (5.9x) | 6.07 / 0.98 (6.2x) | 25.48 / 0.54 (47.5x) | 3.71 / 0.34 (10.7x) | 3.11 / 0.30 (10.3x) |
| a3 (name, region, country) | 5.10 / 0.45 (11.4x) | 10.89 / 0.72 (15.2x) | 31.11 / 0.43 (72.7x) | 7.89 / 0.37 (21.5x) | 6.69 / 0.31 (21.5x) |
| x2 (name, country) | 3.74 / 0.38 (9.8x) | 8.24 / 0.60 (13.7x) | 27.08 / 0.42 (64.5x) | 5.89 / 0.34 (17.5x) | 4.90 / 0.31 (15.8x) |

Build: pg_search 5.0 s, 140 MiB; sift 2.4 s, 43 MiB. These runs used the build before the phrase recheck change below;
places queries are prefixes and ANDs and never reach the phrase recheck.

Cold cache (`bench/places.sh full cold`, 12 queries per cell), p50 / maximum in ms:

| workload | class | pg_search | sift |
|---|---|---:|---:|
| top-20 | p1 | 2.23 / 6.44 | 1.84 / 5.07 |
| top-20 | p2 | 2.73 / 21.77 | 1.54 / 10.72 |
| top-20 | a2 | 3.18 / 101.94 | 0.35 / 10.70 |
| top-20 | a3 | 3.87 / 8.52 | 0.37 / 2.66 |
| boost | p1 | 7.72 / 14.59 | 3.99 / 9.55 |
| boost | p2 | 5.84 / 20.06 | 1.80 / 2.81 |
| boost | a2 | 5.28 / 8.96 | 0.53 / 2.67 |
| boost | a3 | 7.67 / 9.62 | 0.62 / 1.12 |
| filter | p1 | 39.71 / 76.57 | 26.52 / 30.65 |
| filter | p2 | 36.72 / 68.46 | 12.73 / 28.50 |
| filter | a2 | 30.06 / 70.00 | 0.34 / 10.71 |
| filter | a3 | 30.03 / 65.39 | 0.40 / 6.16 |
| count | p1 | 2.39 / 3.37 | 0.80 / 7.61 |
| count | p2 | 2.47 / 4.42 | 0.34 / 0.85 |
| count | a2 | 5.16 / 11.93 | 0.12 / 0.41 |
| count | a3 | 8.64 / 16.07 | 0.17 / 0.34 |

sift is ahead in every cell. The closest are the one-letter prefixes (p1), where the result is a large share of the
corpus and both engines spend their time on ranking and fetching rows, and the unordered scan of p1/p2.

The quick tier (cities15000, 34,163 places) has the same winner as the full tier in all 24 cells they share, with
mostly smaller ratios (pg_search's latency grows with the corpus faster than sift's).

## Harness run time

| | before (review-text corpus) | now |
|---|---|---|
| per interleaved round, 3 engines | about 5 min 11 s for 6 classes at 10 s per cell, plus about 35 s for the join | 3 min 40 s for 7 classes including the join (8 s per cell and 2 s warm-up) |
| corpus and indexes | about 20 s corpus load per run | restore of the cached dump 8 s, each index built once (23 s for the three at 218k articles) |
| full articles run | not repeatable in one command | 22 min 51 s for 6 rounds (load 38-58), early stop when every cell is within 15% |
| quick articles run (150k) | | 9 min 22 s for 6 rounds (load 41-56) |
| places | | quick 8 min 12 s (5 rounds), full 36 min 31 s (6 rounds) |

Under this host's load no run reached the early-stop spread; on a quiet host the minimum of four rounds is about 15 min
(full) and 6 min (quick). Results are kept as three small tables per run in `bench/out/ab/`, and the scratch cluster is
removed when the script ends.
