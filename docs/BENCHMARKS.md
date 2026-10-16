# Benchmarks

sift (and sift with `phrase_index = pairs`) against ParadeDB pg_search 0.26.1 and Timescale pg_textsearch 1.5.1 on two public
datasets, measured with `bench/bench run` (articles through `bench/steps/ab.sh`, places through `bench/steps/places.sh`; method,
datasets, licenses and checksums: `bench/README.md`). Every engine indexes the same rows with the same analysis (lowercase, no
stemming, no stopwords; pg_textsearch through the PostgreSQL `simple` text search configuration) and answers the same queries
from the same cache state; cells are run one at a time with the engine order rotated every round, and the tables report
medians over the rounds. rum 1.3.15 is part of the harness (`--engines sift,sift_pairs,pg_search,pg_textsearch,rum`) but has
not been run for these tables yet: it is marked pending in the charts. The numbers of every table below are also in
[results/articles.tsv](results/articles.tsv) and [results/places.tsv](results/places.tsv), one row per measurement (columns:
`bench/README.md`), for charts.

Host: AMD Ryzen 7 9700X (8 cores, 16 hardware threads), 188 GB RAM, Linux 7.2, PostgreSQL 18.6, scratch data on tmpfs, engine
servers in a cgroup scope of 8 vCPUs and 32 GB, shared with other jobs. Versions of the full-tier session: sift built from commit
`dd3692043b5b`, pg_search from the ParadeDB 0.26.1 package, pg_textsearch 1.5.1 built from the v1.5.1 tag (commit `fa142d049d08`,
tarball sha256 pinned in `bench/versions.env`) with PGXS. The full-tier tables come from one session in which all four engines were
measured together, articles first and then places (38 minutes including the build of the engines). The 1-minute load average during
the cells was 8.8-25 (median 11.8) for articles and 4.8-10.7 (median 6.8) for places, so absolute numbers carry noise and the
per-round ranges in the tables are wide; no round reached the 15% spread that ends a run early. Compare engines within a table, not
across tables or hosts. Index sizes and build times of every table are for one build per engine. The 150k-article and 34k-place
tiers further down are from an earlier session (sift `ac2b4bb`, load 4-30) and do not include pg_textsearch.

## Articles: Simple English Wikipedia, 218,058 articles (full tier)

8 clients, 4 interleaved rounds of 6 s per cell (2 s warm-up), queries are 2-15 consecutive words of random articles (1000 each as
AND, OR and phrase); `bench/bench run --full --dataset articles --engines sift,sift_pairs,pg_search,pg_textsearch --rounds 4-4
--duration 6`. Throughput in queries per second, median (minimum-maximum over the rounds), with the size of the index and the time of
its `CREATE INDEX`:

| engine | index (MiB) | build (s) | and (top-10) | or (top-10) | phrase (top-10) | mixed (and, or, phrase) | count(*) | filter (top-10, newest 2%) | join (top-10 with `categories`) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 47.9 | 1.9 | 9084 (6053-10138) | 1947 (1487-2299) | 2117 (1893-2279) | 2885 (2530-3065) | 1186 (1107-1211) | 2783 (2540-3041) | 819 (669-898) |
| sift `pairs` | 267.2 | 9.4 | 9304 (6080-9950) | 2036 (1883-2145) | 10826 (9702-11531) | 4393 (4084-4719) | 4729 (4130-5741) | 3562 (3246-3909) | 1711 (1646-1830) |
| pg_search | 183.3 | 3.3 | 3713 (2993-4140) | 1029 (880-1103) | 2880 (2530-3263) | 1610 (1181-1700) | 1891 (1742-1995) | 2919 (2618-3159) | 202 (183-213) |
| pg_textsearch | 147.3 | 10.4 | 1624 (1390-1735) | 3387 (2468-3641) | 13 (10-17) | 42 (29-48) | 30 (23-40) | 37 (33-42) | 35 (25-80) |

Ratio of queries per second to pg_search (the baseline of every ratio in this document):

| engine | and | or | phrase | mixed | count(*) | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift / pg_search | 2.45 | 1.89 | **0.74** | 1.79 | **0.63** | 0.95 | 4.06 |
| sift `pairs` / pg_search | 2.51 | 1.98 | 3.76 | 2.73 | 2.50 | 1.22 | 8.48 |
| pg_textsearch / pg_search | **0.44** | **3.29** | **0.00** (0.0044) | **0.03** | **0.02** | **0.01** | **0.18** |

Index size and build time move with the build: pg_search's size depends on how its parallel workers split the segments
(164-185 MiB over the builds of the sessions), build times move with load; sift's sizes do not.

How pg_textsearch is configured: a `bm25` index with `text_config = 'simple'` on the body (the analysis of the other engines), the
database `default_text_search_config` set to `simple` (its `@@` filter parses the column with it) and `enable_seqscan = off` in its
database, which the extension's README suggests for getting its index plans; adapter: `bench/engines/pg_textsearch.sh`. A ranked
OR is `ORDER BY body <@> to_bm25query(terms, 'docs_fts') LIMIT 10`, which the index answers with block-max WAND. It has no AND,
phrase or NOT inside that scan: those are the tsquery `@@` filter through the same index, and the extension does not combine
filter and ranking in one scan. Written as `WHERE body @@ q ORDER BY body <@> ... LIMIT 10` the planner chooses a sequential
scan that tokenizes every document (3 s per query at 25,000 articles), so the harness writes the AND, phrase, filter and join
queries as the extension's documented plan: `FROM (SELECT * FROM docs WHERE body @@ q OFFSET 0) docs ORDER BY ...`, the filter
through the index and the standalone scores of its matches sorted. Without `enable_seqscan = off` the planner counts a
disjunction of common words with a sequential scan too: 0.25 queries per second for `count(*)` against 37.9 with it off
(a separate full-tier measurement, 8 clients, 8 s; AND is 1552 against 1200, within the noise).
Correctness: its match sets and `count(*)` equal the tsvector oracle for 200 substrings in the and, or, phrase and not modes
(`bench/bench check oracle pg_textsearch 200`, PG parser tokens like gin and rum, 25,000 articles). Its top-10 overlaps pgx-bm25's
BM25 by 0.88 (and), 0.97 (or) and 0.96 (phrase) against 0.997, 0.979 and 0.999 for sift (`check rank`, 200 substrings; a soft
check, it scores the PG parser's tokens and a bag of terms).

Cold cache (index, heap and TOAST evicted from shared buffers before every query; `EXPLAIN ANALYZE` execution time
of 12 queries per class, `bench/bench cold --dataset articles --tier full`), p50 / maximum in ms. The scratch data is on
tmpfs, so there is no disk behind the evicted pages: this is the cost of rebuilding the buffers from memory-backed files,
not of disk reads. This table is from the previous session (sift `ac2b4bb`) and has no pg_textsearch column.

| class | sift | sift `pairs` | pg_search |
|---|---:|---:|---:|
| phrase | 1.50 / 84.25 | 1.58 / 3.78 | 5.00 / 23.37 |
| count(*) | 1.18 / 659.97 | 0.62 / 2.20 | 2.84 / 17.94 |
| and | 1.51 / 3.98 | 1.41 / 5.60 | 2.93 / 4.29 |

Reading the numbers:

- AND, OR, mixed and the join are about 1.8-4.1 times faster than pg_search with the default index, which is also
  3.8 times smaller (47.9 against 183.3 MiB) and builds 1.7 times faster (2.4 times in the previous session).
- Without `pairs`, a phrase is checked against the heap after the term intersection, and the check (decompressing and
  tokenizing the candidates) dominates when the words are common: phrase and `count(*)` are at 0.74x and 0.63x of
  pg_search (0.74x and 0.70x in the previous session, 0.81x and 0.82x before; the per-round ranges overlap, so read it as
  0.6-0.8x), and the cold
  maximum of the default index shows the same cost. `phrase_index = pairs` removes it and wins every class, but its index
  is 5.6 times the default one and 1.46 times pg_search's (267.2 against 183.3 MiB; 1.4-1.6 times over the builds of
  the sessions), and it takes 9.4 s to build against 3.3 s for pg_search: sift with `pairs` is the larger index.
- The recheck used to be slower. Profiling the phrase class (perf, one client) put 38% of the CPU in the per-document
  term table that the recheck built before looking for the phrase; the recheck now scans the document's tokens and builds
  the table only for the other leaf kinds. The same measurement with fixed instruction counts: phrase 48.9M to 36.6M
  instructions per query (7.0 to 4.7 ms CPU), `count(*)` 46.0M to 33.9M (6.4 to 3.8 ms), mixed 39.4M to 34.2M, and 8.2M
  (unchanged) for AND. The interleaved run before the change measured phrase 743 against pg_search 1478 (0.50x) and
  `count(*)` 391 against 747 (0.52x). What is left of the phrase cost is tokenizing (30% of the CPU) and decompressing
  the TOASTed text (21%).
- The filter class is level with pg_search (0.95x with the default index, 1.22x with `pairs`; 0.99x and 1.16x in the
  previous session).
- pg_textsearch wins one class and loses the others. A bare ranked disjunction is its native query, answered by block-max
  WAND: 3387 queries per second, 3.3 times pg_search, 1.7 times sift and 1.7 times sift `pairs`, the fastest of the four.
  Its index is 147.3 MiB (0.8 times pg_search, 3.1 times sift's default index) and takes 10.4 s to build (3.2 times
  pg_search, 5.5 times sift; sift `pairs` takes 9.4 s). Everything that is not a bare disjunction goes through the
  tsquery filter and then scores the matches one by one as standalone scores (see the configuration above): AND runs at
  0.44x of pg_search, and phrase (13), mixed (42), `count(*)` (30), filter (37) and join (35) at 0.00-0.18x. The cost
  follows the number of matching articles (phrase was 118 queries per second on the 25,000 articles of the quick tier),
  which the 2-15 word queries of the trace make large; I did not profile it further. The extension's README says that
  combining the filter with the ranking is not yet one index scan.
- In short: sift's default index is the smallest and the quickest to build and is 1.8-4.1 times pg_search on AND, OR, mixed
  and join; `pairs` adds phrase (3.8 times), `count(*)` (2.5 times) and filter (1.2 times) for 5.6 times the index;
  pg_search beats the default index on phrase and `count(*)` at this size and is the slowest of the three on join;
  pg_textsearch wins only the bare ranked OR.

### 150,000 articles (`ab` tier)

`bench/bench run --dataset articles --tier ab`, earlier session: 3 s per cell, 6 rounds (load 4-25). Same classes, queries per second
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
0.63x). The heap recheck grows with the corpus, so the crossover depends on the size: with an earlier build and a
75,000 article tier the ratios were 1.22x and 1.35x, and with the 25,000 article tier of the first harness version 1.11x
and 1.54x. Judge the default index's phrase and `count(*)` on the full tier. The size ordering is the same as at 218k:
`pairs` is 192.2 MiB against 115.8 MiB for pg_search (1.66 times) and 33.5 MiB for the default index.

## Places: GeoNames cities500, 236,262 places (full tier)

Multi-field autocomplete as an application would run it: sift indexes the name section and the all-fields text with
`fields = 'name', prefix_index = 3`, pg_search indexes `edge_ngram(1, 15)` fields for both; every typed word is a
prefix. `sift.max_expansions` is set to 100000 for the database: at the default 512, prefixes of four or more letters
of common words (`pari*`, `san*`) raise the expansion error on this corpus. Single client, 4 interleaved rounds of 2 s
per cell (`bench/bench run --full --dataset places --engines sift,pg_search,pg_textsearch --rounds 4-4 --duration 2`, load
4.8-10.7). Index size, build time and p50 latency in ms of the top-20 workload per query class:

| engine | index (MiB) | build (s) | p1 | p2 | p3 | p4 | w1 | a2 | a2s | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 42.6 | 1.3 | 0.60 | 0.47 | 0.31 | 0.24 | 0.14 | 0.20 | 0.27 | 0.23 | 0.22 |
| pg_search | 142.9 | 2.8 | 0.78 | 0.94 | 1.06 | 1.09 | 1.16 | 1.53 | 1.35 | 2.54 | 1.93 |
| pg_textsearch | 68.3 | 2.3 | NA | NA | NA | NA | NA | NA | NA | NA | NA |

All workloads, p50 latency in ms, `pg_search / sift (ratio)`:

| class | top-20 | name boost x3 | filtered (`country_code = 'US'`) | count(*) | unordered (`population >= 100000`) |
|---|---:|---:|---:|---:|---:|
| p1 (1 letter) | 0.78 / 0.60 (1.3x) | 3.02 / 1.14 (2.6x) | 12.21 / 8.60 (1.4x) | 1.01 / 0.45 (2.2x) | 12.56 / 11.75 (1.1x) |
| p2 (2 letters) | 0.94 / 0.47 (2.0x) | 2.80 / 0.75 (3.7x) | 7.07 / 2.61 (2.7x) | 1.25 / 0.30 (4.2x) | 4.36 / 2.28 (1.9x) |
| p3 (3 letters) | 1.06 / 0.31 (3.4x) | 2.33 / 0.37 (6.3x) | 4.82 / 0.54 (8.9x) | 1.14 / 0.19 (6.0x) | 1.56 / 0.47 (3.3x) |
| p4 (4 letters) | 1.09 / 0.24 (4.5x) | 2.13 / 0.41 (5.2x) | 4.68 / 0.37 (12.6x) | 1.20 / 0.28 (4.3x) | 1.17 / 0.24 (4.9x) |
| w1 (whole word) | 1.16 / 0.14 (8.3x) | 2.24 / 0.17 (13.2x) | 4.80 / 0.18 (26.7x) | 1.64 / 0.18 (9.1x) | 1.48 / 0.15 (9.9x) |
| a2 (word, 2 letters) | 1.53 / 0.20 (7.6x) | 3.01 / 0.30 (10.0x) | 5.12 / 0.25 (20.5x) | 1.92 / 0.21 (9.1x) | 1.82 / 0.16 (11.4x) |
| a2s (word, 1 letter) | 1.35 / 0.27 (5.0x) | 2.72 / 0.49 (5.6x) | 5.12 / 0.31 (16.5x) | 1.82 / 0.21 (8.7x) | 1.63 / 0.17 (9.6x) |
| a3 (name, region, country) | 2.54 / 0.23 (11.0x) | 5.06 / 0.37 (13.7x) | 6.62 / 0.26 (25.5x) | 3.64 / 0.23 (15.8x) | 3.21 / 0.19 (16.9x) |
| x2 (name, country) | 1.93 / 0.22 (8.8x) | 3.66 / 0.32 (11.4x) | 5.89 / 0.25 (23.6x) | 2.62 / 0.22 (11.9x) | 2.54 / 0.19 (13.4x) |

Places queries are prefixes and ANDs and never reach the phrase recheck.

pg_textsearch on places: a typed prefix is a `:*` term of a tsquery that its index filters, but `<@>` scores whole terms only: there
is no prefix scoring (no edge n-grams in a text search configuration) and no per-field boost, so the ranked workloads (top-20,
boost, filtered top-20) have no equivalent query and are NA rather than a ranking of different semantics. The unordered
workloads, which only need the match set, run on a `bm25` index of the all-fields text (in its own database, as its access
method has the name of pg_search's). p50 latency in ms:

| class | count(*): sift | pg_search | pg_textsearch | unordered scan: sift | pg_search | pg_textsearch |
|---|---:|---:|---:|---:|---:|---:|
| p1 | 0.45 | 1.01 | 434.27 | 11.75 | 12.56 | 519.17 |
| p2 | 0.30 | 1.25 | 87.91 | 2.28 | 4.36 | 80.17 |
| p3 | 0.19 | 1.14 | 6.69 | 0.47 | 1.56 | 8.25 |
| p4 | 0.28 | 1.20 | 1.18 | 0.24 | 1.17 | 1.23 |
| w1 | 0.18 | 1.64 | 0.82 | 0.15 | 1.48 | 0.78 |
| a2 | 0.21 | 1.92 | 2.26 | 0.16 | 1.82 | 2.31 |
| a2s | 0.21 | 1.82 | 9.79 | 0.17 | 1.63 | 10.94 |
| a3 | 0.23 | 3.64 | 0.77 | 0.19 | 3.21 | 0.75 |
| x2 | 0.22 | 2.62 | 0.77 | 0.19 | 2.54 | 0.82 |

pg_textsearch is level with pg_search on p4 and a2, 2.0-4.7 times faster than it on whole words and on the long multi-field
queries (w1, a3, x2: 0.8 against 1.6-3.6 ms; sift is 3.4-5.2 times faster than pg_textsearch there) and behind it on the
short prefixes, by orders of magnitude at one and two letters (434 ms against 0.45 ms for sift at one letter, 88 ms at two).
The index returns a superset of the matches of a prefix that is rechecked against the heap (3,287 rows removed for 34 matches
in a manual `EXPLAIN ANALYZE` of a two-prefix query), which I did not measure further. Its index is 68.3 MiB (1.6 times sift's,
0.48 times pg_search's) and builds in 2.3 s.

Cold cache (`bench/bench cold --dataset places --tier full`, 12 queries per cell, scratch data on tmpfs as above; previous
session, without pg_textsearch), p50 / maximum in ms:

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

sift is ahead of pg_search in every warm cell of this session (45 of 45; smallest ratio 1.07x) and in every cold cell of the
previous one. The closest are the one-letter prefixes (p1), where the result is a large share of the corpus and both engines
spend their time on ranking and fetching rows, and the unordered scan of p1/p2. The index is 3.4 times smaller (42.6 against
142.9 MiB) and builds 2.2 times faster.

### 34,165 places (`ab` tier, cities15000)

`bench/bench run --dataset places --tier ab`, earlier session without pg_textsearch: 5 rounds of 2 s per cell (load 16-30). Index size and build time: sift 13.7 MiB
and 0.6 s, pg_search 27.8 MiB and 2.6 s; p50 latency of the top-20 workload in ms:

| engine | p1 | p3 | w1 | a2 | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|
| sift | 0.48 | 0.31 | 0.23 | 0.23 | 0.29 | 0.27 |
| pg_search | 0.71 | 0.79 | 0.99 | 1.20 | 1.73 | 1.33 |

The 34k tier has the same winner as the full tier of this session in all 24 cells they share (smallest ratio 1.3x), with
smaller ratios in 21 of them (pg_search's latency grows with the corpus faster than sift's). All cells are in `results/places.tsv`.

## Harness run time

| | before (review-text corpus) | now |
|---|---|---|
| per interleaved round, 3 engines | about 5 min 11 s for 6 classes at 10 s per cell, plus about 35 s for the join | about 3 min 30 s for 7 classes including the join (8 s per cell and 2 s warm-up; 4 min 50 s with rum in the round) |
| corpus and indexes | about 20 s corpus load per run | restore of the cached dump 8 s, each index built once (16 s for the three at 218k articles) |
| full articles run | not repeatable in one command | 4 rounds of 6 s with four engines (sift, sift `pairs`, pg_search, pg_textsearch): 23 min (load 9-25); the previous session ran 6 rounds of 8 s with rum in the round: 31 min (load 16-46); early stop when every cell is within 15% |
| 150k articles run | | 8 min 47 s for 6 rounds, three engines (load 4-25) |
| places | | 150k tier 8 min 11 s (5 rounds), full 36 min 17 s (6 rounds of 4 s, two engines), full 14 min 43 s (4 rounds of 2 s, three engines, pg_textsearch NA in most cells; load 5-11) |

Under this host's load no run reached the early-stop spread; on a quiet host the minimum of four rounds is about 15 min
(full) and 6 min (150k tier). Results are kept as small tables per run (`cells.tsv`, `builds.tsv`, `summary.tsv`,
`machine.txt` with the CPU, engine versions and load) in the result directory, and the scratch cluster is removed when
the run ends.
