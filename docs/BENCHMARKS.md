# Benchmarks

sift against ParadeDB pg_search 0.26.1 (and sift with `phrase_index = pairs`) on two public datasets, measured with
`bench/ab.sh` and `bench/places.sh` (method, datasets, licenses and checksums: `bench/README.md`). Every engine
indexes the same rows with the same analysis (lowercase, no stemming, no stopwords) and answers the same
queries from the same cache state; cells are run one at a time with the engine order rotated every round, and
the tables report medians over the rounds.

Host: 16 hardware threads, 188 GB RAM, PostgreSQL 18.6, shared with other jobs. The 1-minute load average during
the measurements was 20-42 (articles) and 19-49 (places), so absolute numbers carry noise and no round ever reached
the 15% spread that ends a run early; compare engines within a table, not across tables or hosts.

## Articles: Simple English Wikipedia, 218,058 articles (full tier)

8 clients, 6 interleaved rounds of 8 s per cell (2 s warm-up), queries are 2-15 consecutive words of random
articles (1000 each as AND, OR and phrase); `bench/ab.sh full`. Throughput in queries per second, median
(minimum-maximum over the rounds):

| class | sift | sift `pairs` | pg_search | sift / pg_search | pairs / pg_search |
|---|---:|---:|---:|---:|---:|
| and (top-10) | 4201 (3767-5146) | 3986 (3744-5087) | 1699 (1437-1986) | 2.47 | 2.35 |
| or (top-10) | 1053 (867-1064) | 1031 (986-1095) | 440 (395-482) | 2.39 | 2.34 |
| phrase (top-10) | 743 (672-850) | 4569 (4090-4998) | 1478 (1082-1576) | **0.50** | 3.09 |
| mixed (and, or, phrase) | 1137 (1038-1225) | 2153 (1850-2287) | 654 (536-751) | 1.74 | 3.29 |
| count(*) | 391 (250-441) | 2460 (1635-2826) | 747 (450-900) | **0.52** | 3.29 |
| filter (top-10, 2% date range) | 1253 (1124-1394) | 1583 (1226-1723) | 1353 (872-1444) | **0.93** | 1.17 |
| join (top-10 with `categories`, region filter) | 398 (342-427) | 715 (637-849) | 101 (86-109) | 3.95 | 7.09 |

Index size and build time (one build per engine, parallel workers, same host load):

| index | build | size |
|---|---:|---:|
| sift | 2.3 s | 48 MB |
| sift `phrase_index = pairs` | 13.0 s | 267 MB |
| pg_search bm25 | 5.8 s | 164 MB |

A second build for the cold run took 3.7 s, 20.4 s and 9.3 s at a higher load; build times move with load, sizes do not.

Cold cache (index, heap and TOAST evicted from shared buffers and the OS cache before every query; `EXPLAIN ANALYZE`
execution time of 12 queries per class, `bench/ab.sh cold full`), p50 / maximum in ms:

| class | sift | sift `pairs` | pg_search |
|---|---:|---:|---:|
| phrase | 4.57 / 351.26 | 2.16 / 18.74 | 4.78 / 14.06 |
| count(*) | 1.36 / 1344.67 | 1.45 / 9.92 | 2.86 / 29.90 |
| and | 2.16 / 5.30 | 1.64 / 6.25 | 3.84 / 7.18 |

Reading the numbers:

- AND, OR, mixed and the join are faster than pg_search with the default index, and the index is three times smaller.
- Without `pairs`, a phrase is checked against the heap after the term intersection, and the check (tokenizing the
  candidate) dominates when the words are common: phrase and `count(*)` lose by half to pg_search, and the cold
  maximum of the default index shows the same cost (351 and 1345 ms against 14 and 30 ms). `phrase_index = pairs`
  removes it and wins every class, for 5.5 times the default index size (1.6 times pg_search's).
- The filter class is at parity (0.93) with the default index and ahead with `pairs` (1.17).

### Quick tier (75,000 articles)

`bench/ab.sh quick`: 3 s per cell, 6 rounds (load 29-53). Same classes, queries per second and ratio to pg_search:

| class | sift | sift `pairs` | pg_search | sift / pg_search | pairs / pg_search |
|---|---:|---:|---:|---:|---:|
| and | 4201 | 4246 | 1629 | 2.58 | 2.61 |
| or | 1561 | 1554 | 602 | 2.59 | 2.58 |
| phrase | 1044 | 4246 | 1347 | 0.78 | 3.15 |
| mixed | 1770 | 2865 | 969 | 1.83 | 2.96 |
| count(*) | 935 | 3255 | 1252 | 0.75 | 2.60 |
| filter | 1806 | 1899 | 1273 | 1.42 | 1.49 |
| join | 704 | 1212 | 193 | 3.65 | 6.28 |

The quick tier reproduces the full tier's ordering for every class except the filter of the default index, which is a
win at 75k (1.42) and parity at 218k (0.93). It needs this size: at 25,000 articles the same run showed the default
index ahead on phrase (1.11) and `count(*)` (1.54), because the heap recheck grows with the corpus. Index sizes at
75k: 18.6 MB (sift), 106.6 MB (`pairs`), 63.3 MB (pg_search).

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

Build: pg_search 5.0 s, 140 MB; sift 2.4 s, 43 MB.

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
| full articles run | not repeatable in one command | 22 min 43 s for 6 rounds (load 20-42), early stop when every cell is within 15% |
| quick articles run (75k) | | 9 min 14 s for 6 rounds (load 41-53) |
| places | | quick 8 min 12 s (5 rounds), full 36 min 31 s (6 rounds) |

Under this host's load no run reached the early-stop spread; on a quiet host the minimum of four rounds is about 15 min
(full) and 6 min (quick). Results are kept as three small tables per run in `bench/out/ab/`, and the scratch cluster is
removed when the script ends.
