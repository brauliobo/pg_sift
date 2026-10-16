# Benchmarks

sift (and sift with `phrase_index = pairs`) against ParadeDB pg_search 0.26.1, Timescale pg_textsearch 1.5.1 and rum 1.3.15 on two public
datasets, measured with `bench/bench run` (articles through `bench/steps/ab.sh`, places through `bench/steps/places.sh`; method,
datasets, licenses and checksums: `bench/README.md`). Every engine indexes the same rows with the same analysis (lowercase, no
stemming, no stopwords; pg_textsearch and rum through the PostgreSQL `simple` text search configuration) and answers the same
queries from the same cache state; cells are run one at a time with the engine order rotated every round, and the tables report
medians over the rounds. The engines, in the order of every table and chart, are listed in `bench/engines.tsv`. The numbers of
every table below are also in
[results/articles.tsv](results/articles.tsv) and [results/places.tsv](results/places.tsv), one row per measurement (columns:
`bench/README.md`), for charts.

Host: AMD Ryzen 7 9700X (8 cores, 16 hardware threads), 188 GB RAM, Linux 7.2, PostgreSQL 18.6, scratch data on tmpfs, engine
servers in a cgroup scope of 8 vCPUs and 32 GB, shared with other jobs. Versions of the full-tier session: sift built from commit
`dd3692043b5b`, pg_search from the ParadeDB 0.26.1 package, pg_textsearch 1.5.1 built from the v1.5.1 tag (commit `fa142d049d08`,
tarball sha256 pinned in `bench/versions.env`) with PGXS, rum 1.3.15 (tag 1.3.15, the latest, tarball sha256 pinned) with PGXS. The
full-tier tables come from one session in which all five engines were measured together, articles first and then places (48 minutes
including the build of the engines; a session that measured sift, `pairs`, pg_search and pg_textsearch alone, with the same sift
build, preceded it, and rum was added by measuring everything again so that every ratio comes from one session). The 1-minute load
average during the cells was 4.2-31 (median 16.5) for articles and 8.7-29 (median 15.4) for places, higher than in the preceding
session, so absolute numbers carry noise and the per-round ranges in the tables are wide; no round reached the 15% spread that ends
a run early. Compare engines within a table, not across tables or hosts. Index sizes and build times of every table are for one
build per engine. The 150k-article and 34k-place tiers further down are from an earlier session (sift `ac2b4bb`, load 4-30) and
have neither pg_textsearch nor rum.

## Articles: Simple English Wikipedia, 218,058 articles (full tier)

8 clients, 4 interleaved rounds of 6 s per cell (2 s warm-up), queries are 2-15 consecutive words of random articles (1000 each as
AND, OR and phrase); `bench/bench run --full --dataset articles --engines sift,sift_pairs,pg_search,pg_textsearch,rum --rounds 4-4
--duration 6`. Throughput in queries per second, median (minimum-maximum over the rounds), with the size of the index and the time of
its `CREATE INDEX` (rum also needs a stored `tsvector` column: 19.1 s to fill it, 437.7 MiB of table growth, neither counted in the
index column):

| engine | index (MiB) | build (s) | and (top-10) | or (top-10) | phrase (top-10) | mixed (and, or, phrase) | count(*) | filter (top-10, newest 2%) | join (top-10 with `categories`) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 47.9 | 1.5 | 8010 (5012-9448) | 1885 (1680-2143) | 1765 (1451-2432) | 2653 (2355-3195) | 1147 (842-1503) | 2670 (1946-2994) | 803 (514-875) |
| sift `pairs` | 267.2 | 7.8 | 8861 (8391-9928) | 1975 (1512-2150) | 7719 (6447-9623) | 4003 (3396-4849) | 5273 (4436-5813) | 3453 (2996-3964) | 1397 (1158-1662) |
| pg_search | 164.3 | 2.9 | 3216 (2657-3853) | 811 (707-1081) | 2725 (2138-3328) | 1490 (1146-1629) | 1630 (875-2040) | 2884 (2503-3215) | 186 (163-225) |
| pg_textsearch | 147.3 | 9.7 | 1472 (1365-1542) | 3254 (2385-3556) | 14 (9-21) | 29 (18-37) | 57 (28-66) | 36 (30-40) | 50 (22-61) |
| rum | 309.9 | 13.5 | 1324 (959-1455) | 10 (9-11) | 37 (32-41) | 22 (21-30) | 46 (42-47) | 23 (19-26) | 10 (8-14) |

Ratio of queries per second to pg_search (the baseline of every ratio in this document):

| engine | and | or | phrase | mixed | count(*) | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift / pg_search | 2.49 | 2.33 | **0.65** | 1.78 | **0.70** | 0.93 | 4.31 |
| sift `pairs` / pg_search | 2.76 | 2.44 | 2.83 | 2.69 | 3.23 | 1.20 | 7.49 |
| pg_textsearch / pg_search | **0.46** | **4.01** | **0.01** (0.0052) | **0.02** | **0.03** | **0.01** | **0.27** |
| rum / pg_search | **0.41** | **0.01** | **0.01** | **0.02** | **0.03** | **0.01** | **0.05** |

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

How rum is configured: a stored `tsvector` column (`to_tsvector('simple', body)`, a generated column, so the heap grows by
437.7 MiB and filling it takes 19.1 s on top of the 13.5 s `CREATE INDEX`) with a `rum_tsvector_ops` index, the same column gin
would index; adapter: `bench/engines/rum.sh`. A query is `WHERE tsv @@ tsquery ORDER BY tsv <=> tsquery LIMIT 10`, rum's own
ranking operator (`<=>` is rum's ts_rank-style distance from the positions it stores, not BM25).
Correctness: its match sets and `count(*)` equal the tsvector oracle for 200 substrings in the and, or, phrase and not modes
(`bench/bench check oracle rum 200`, 25,000 articles: 0 mismatches of 800 queries, the PG parser's tokens like gin and
pg_textsearch).

Cold cache (index, heap and TOAST evicted from shared buffers before every query; `EXPLAIN ANALYZE` execution time
of 12 queries per class, `bench/bench cold --dataset articles --tier full`), p50 / maximum in ms. The scratch data is on
tmpfs, so there is no disk behind the evicted pages: this is the cost of rebuilding the buffers from memory-backed files,
not of disk reads. This table is from an earlier session (sift `ac2b4bb`) and has no pg_textsearch or rum column.

| class | sift | sift `pairs` | pg_search |
|---|---:|---:|---:|
| phrase | 1.50 / 84.25 | 1.58 / 3.78 | 5.00 / 23.37 |
| count(*) | 1.18 / 659.97 | 0.62 / 2.20 | 2.84 / 17.94 |
| and | 1.51 / 3.98 | 1.41 / 5.60 | 2.93 / 4.29 |

Reading the numbers:

- AND, OR, mixed and the join are about 1.8-4.3 times faster than pg_search with the default index, which is also
  3.4 times smaller (47.9 against 164.3 MiB) and builds 1.9 times faster (1.5 against 2.9 s).
- Without `pairs`, a phrase is checked against the heap after the term intersection, and the check (decompressing and
  tokenizing the candidates) dominates when the words are common: phrase and `count(*)` are at 0.65x and 0.70x of
  pg_search (0.74x and 0.63x in the preceding session of the same build, 0.74x and 0.70x and 0.81x and 0.82x before; the
  per-round ranges overlap, so read it as 0.6-0.8x), and the cold
  maximum of the default index shows the same cost. `phrase_index = pairs` removes it and beats pg_search in every class,
  but its index is 5.6 times the default one and 1.63 times pg_search's (267.2 against 164.3 MiB; 1.4-1.6 times over the
  builds of the sessions), and it takes 7.8 s to build against 2.9 s for pg_search: sift with `pairs` is the larger index.
- The recheck used to be slower. Profiling the phrase class (perf, one client) put 38% of the CPU in the per-document
  term table that the recheck built before looking for the phrase; the recheck now scans the document's tokens and builds
  the table only for the other leaf kinds. The same measurement with fixed instruction counts: phrase 48.9M to 36.6M
  instructions per query (7.0 to 4.7 ms CPU), `count(*)` 46.0M to 33.9M (6.4 to 3.8 ms), mixed 39.4M to 34.2M, and 8.2M
  (unchanged) for AND. The interleaved run before the change measured phrase 743 against pg_search 1478 (0.50x) and
  `count(*)` 391 against 747 (0.52x). What is left of the phrase cost is tokenizing (30% of the CPU) and decompressing
  the TOASTed text (21%).
- The filter class is level with pg_search (0.93x with the default index, 1.20x with `pairs`; 0.95x and 1.22x in the
  preceding session, 0.99x and 1.16x before).
- pg_textsearch wins one class and loses the others. A bare ranked disjunction is its native query, answered by block-max
  WAND: 3254 queries per second, 4.0 times pg_search, 1.7 times sift and 1.6 times sift `pairs`, the fastest of the five.
  Its index is 147.3 MiB (0.9 times pg_search, 3.1 times sift's default index) and takes 9.7 s to build (3.3 times
  pg_search, 6.5 times sift; sift `pairs` takes 7.8 s). Everything that is not a bare disjunction goes through the
  tsquery filter and then scores the matches one by one as standalone scores (see the configuration above): AND runs at
  0.46x of pg_search, and phrase (14), mixed (29), `count(*)` (57), filter (36) and join (50) at 0.01-0.27x. The cost
  follows the number of matching articles (phrase was 118 queries per second on the 25,000 articles of the quick tier),
  which the 2-15 word queries of the trace make large; I did not profile it further. The extension's README says that
  combining the filter with the ranking is not yet one index scan.
- rum is behind pg_search in every class and the largest index: 309.9 MiB (1.9 times pg_search, 6.5 times sift's default
  index, 1.16 times sift `pairs`), 13.5 s for `CREATE INDEX` (4.7 times pg_search, 9 times sift) after 19.1 s to fill its
  stored `tsvector` column, which adds 437.7 MiB to the table (748 MiB in all, 15.6 times sift's index). AND, a two-to-fifteen
  word intersection answered from the index, runs at 1324 queries per second (0.41x of pg_search, 0.17x of sift). Every other
  class is two orders of magnitude lower: phrase 37, `count(*)` 46, mixed 22, filter 23, and OR and join at 10 queries per
  second (0.01x and 0.05x). I did not profile it; its `<=>` ordering and the `@@` recheck of the stored `tsvector`
  (positions are checked in the heap for phrases) both visit every match of these common-word queries.
- In short: sift's default index is the smallest and the quickest to build and is 1.8-4.3 times pg_search on AND, OR, mixed
  and join; `pairs` adds phrase (2.8 times), `count(*)` (3.2 times) and filter (1.2 times) for 5.6 times the index;
  pg_search beats the default index on phrase and `count(*)` at this size and is the slowest of the three on join;
  pg_textsearch wins only the bare ranked OR; rum wins nothing and is the largest index.

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
per cell (`bench/bench run --full --dataset places --engines sift,pg_search,pg_textsearch,rum --rounds 4-4 --duration 2`,
load 8.7-29). Index size, build time and p50 latency in ms of the top-20 workload per query class:

| engine | index (MiB) | build (s) | p1 | p2 | p3 | p4 | w1 | a2 | a2s | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 42.6 | 1.3 | 0.75 | 0.58 | 0.36 | 0.54 | 0.26 | 0.30 | 0.36 | 0.30 | 0.33 |
| pg_search | 129.5 | 3.0 | 1.19 | 1.28 | 1.48 | 2.27 | 1.71 | 1.85 | 1.55 | 3.85 | 2.85 |
| pg_textsearch | 68.3 | 2.4 | NA | NA | NA | NA | NA | NA | NA | NA | NA |
| rum | 94.9 | 2.9 | 124.49 | 17.90 | 6.43 | 1.62 | 0.52 | 11.90 | 95.28 | 10.38 | 10.81 |

All workloads, p50 latency in ms, `pg_search / sift (ratio)`:

| class | top-20 | name boost x3 | filtered (`country_code = 'US'`) | count(*) | unordered (`population >= 100000`) |
|---|---:|---:|---:|---:|---:|
| p1 (1 letter) | 1.19 / 0.75 (1.6x) | 4.39 / 2.10 (2.1x) | 13.64 / 7.79 (1.8x) | 0.77 / 0.33 (2.3x) | 16.75 / 12.59 (1.3x) |
| p2 (2 letters) | 1.28 / 0.58 (2.2x) | 3.46 / 0.80 (4.3x) | 7.28 / 3.30 (2.2x) | 1.01 / 0.30 (3.4x) | 4.89 / 2.67 (1.8x) |
| p3 (3 letters) | 1.48 / 0.36 (4.1x) | 2.77 / 0.45 (6.2x) | 10.37 / 0.63 (16.5x) | 1.11 / 0.22 (5.0x) | 1.75 / 0.59 (3.0x) |
| p4 (4 letters) | 2.27 / 0.54 (4.2x) | 2.36 / 0.56 (4.2x) | 6.26 / 0.44 (14.2x) | 1.21 / 0.20 (6.0x) | 1.66 / 0.38 (4.4x) |
| w1 (whole word) | 1.71 / 0.26 (6.6x) | 2.55 / 0.18 (14.2x) | 9.38 / 0.25 (37.5x) | 2.32 / 0.16 (14.5x) | 1.82 / 0.18 (10.1x) |
| a2 (word, 2 letters) | 1.85 / 0.30 (6.2x) | 4.21 / 0.35 (12.0x) | 8.24 / 0.32 (25.8x) | 3.06 / 0.19 (16.1x) | 1.91 / 0.19 (10.1x) |
| a2s (word, 1 letter) | 1.55 / 0.36 (4.3x) | 2.90 / 0.58 (5.0x) | 9.73 / 0.46 (21.2x) | 1.84 / 0.18 (10.2x) | 2.40 / 0.21 (11.4x) |
| a3 (name, region, country) | 3.85 / 0.30 (12.8x) | 5.94 / 0.42 (14.1x) | 7.28 / 0.28 (26.0x) | 4.12 / 0.23 (17.9x) | 3.56 / 0.24 (14.8x) |
| x2 (name, country) | 2.85 / 0.33 (8.6x) | 3.93 / 0.36 (10.9x) | 6.65 / 0.32 (20.8x) | 2.65 / 0.23 (11.5x) | 3.68 / 0.28 (13.1x) |

Places queries are prefixes and ANDs and never reach the phrase recheck.

rum on places: a stored `tsvector` column of the all-fields text (`simple`) with a `rum_tsvector_ops` index, in its own database so that
the column (60.5 MiB, 1.6 s to fill) does not enlarge the other engines' table; a typed prefix is a `:*` term of the tsquery, ranked
with `<=>`. It has no per-field boost, so the boost workload is NA. p50 latency in ms of the ranked workloads, next to sift:

| class | top-20: sift | rum | filtered top-20: sift | rum |
|---|---:|---:|---:|---:|
| p1 | 0.75 | 124.49 | 7.79 | 132.44 |
| p2 | 0.58 | 17.90 | 3.30 | 13.12 |
| p3 | 0.36 | 6.43 | 0.63 | 2.71 |
| p4 | 0.54 | 1.62 | 0.44 | 0.65 |
| w1 | 0.26 | 0.52 | 0.25 | 0.20 |
| a2 | 0.30 | 11.90 | 0.32 | 15.47 |
| a2s | 0.36 | 95.28 | 0.46 | 41.71 |
| a3 | 0.30 | 10.38 | 0.28 | 11.09 |
| x2 | 0.33 | 10.81 | 0.32 | 6.63 |

rum is within 2 times of sift on whole words (0.52 against 0.26 ms for top-20; 0.20 against 0.25 ms filtered, its only cell below
sift) and within 3 times on four-letter prefixes (1.6 against 0.54 ms, 0.65 against 0.44 ms), and 4-265 times slower on the other
classes: 124 ms for a one-letter prefix (165 times sift's 0.75 ms), 95 ms for a word and a one-letter prefix, 10-12 ms for the
multi-field queries. Against pg_search
it is faster on whole words (0.30x) and four-letter prefixes (0.71x) of the top-20 and on filtered whole words, four-letter and
three-letter prefixes, and behind it everywhere else. Its index is 94.9 MiB (2.2 times sift's, 0.73 times pg_search's) and builds in
2.9 s; the unordered and count workloads are in the table below.

pg_textsearch on places: a typed prefix is a `:*` term of a tsquery that its index filters, but `<@>` scores whole terms only: there
is no prefix scoring (no edge n-grams in a text search configuration) and no per-field boost, so the ranked workloads (top-20,
boost, filtered top-20) have no equivalent query and are NA rather than a ranking of different semantics. The unordered
workloads, which only need the match set, run on a `bm25` index of the all-fields text (in its own database, as its access
method has the name of pg_search's). p50 latency in ms:

| class | count(*): sift | pg_search | pg_textsearch | rum | unordered scan: sift | pg_search | pg_textsearch | rum |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| p1 | 0.33 | 0.77 | 495.00 | 38.01 | 12.59 | 16.75 | 563.04 | 49.69 |
| p2 | 0.30 | 1.01 | 93.45 | 8.14 | 2.67 | 4.89 | 98.22 | 11.30 |
| p3 | 0.22 | 1.11 | 9.13 | 2.83 | 0.59 | 1.75 | 9.57 | 2.83 |
| p4 | 0.20 | 1.21 | 1.41 | 0.43 | 0.38 | 1.66 | 1.61 | 0.61 |
| w1 | 0.16 | 2.32 | 1.14 | 0.18 | 0.18 | 1.82 | 1.22 | 0.21 |
| a2 | 0.19 | 3.06 | 3.02 | 5.53 | 0.19 | 1.91 | 2.31 | 6.05 |
| a2s | 0.18 | 1.84 | 11.12 | 34.64 | 0.21 | 2.40 | 11.82 | 46.44 |
| a3 | 0.23 | 4.12 | 0.90 | 7.64 | 0.24 | 3.56 | 0.99 | 6.13 |
| x2 | 0.23 | 2.65 | 0.90 | 5.24 | 0.28 | 3.68 | 0.93 | 3.59 |

pg_textsearch is level with pg_search on p4 and a2, 1.5-4.6 times faster than it on whole words and on the long multi-field
queries (w1, a3, x2: 0.9-1.2 against 1.8-4.1 ms; sift is 3.3-7.1 times faster than pg_textsearch there) and behind it on the
short prefixes, by orders of magnitude at one and two letters (495 ms against 0.33 ms for sift at one letter, 93 ms at two).
The index returns a superset of the matches of a prefix that is rechecked against the heap (3,287 rows removed for 34 matches
in a manual `EXPLAIN ANALYZE` of a two-prefix query), which I did not measure further. Its index is 68.3 MiB (1.6 times sift's,
0.53 times pg_search's) and builds in 2.4 s. rum on the same unordered workloads is within 2.2 times of sift on whole words and
four-letter prefixes (0.18-0.61 ms) and 30-220 times slower on a word with a one or two letter prefix (a2, a2s: 5.5-46 ms against 0.2 ms).

Cold cache (`bench/bench cold --dataset places --tier full`, 12 queries per cell, scratch data on tmpfs as above; previous
session, without pg_textsearch and rum), p50 / maximum in ms:

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

sift is ahead of pg_search in every warm cell of this session (45 of 45; smallest ratio 1.33x) and in every cold cell of an
earlier one. The closest are the one-letter prefixes (p1), where the result is a large share of the corpus and both engines
spend their time on ranking and fetching rows, and the unordered scan of p1/p2. The index is 3.0 times smaller (42.6 against
129.5 MiB; 142.9 MiB in the preceding session) and builds 2.3 times faster.

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
| full articles run | not repeatable in one command | 4 rounds of 6 s with five engines (sift, sift `pairs`, pg_search, pg_textsearch, rum): 28 min (load 4-31), 23 min without rum; an older session ran 6 rounds of 8 s with rum in the round: 31 min (load 16-46); early stop when every cell is within 15% |
| 150k articles run | | 8 min 47 s for 6 rounds, three engines (load 4-25) |
| places | | 150k tier 8 min 11 s (5 rounds), full 36 min 17 s (6 rounds of 4 s, two engines), full 14 min 43 s (4 rounds of 2 s, three engines, pg_textsearch NA in most cells; load 5-11), full 19 min 50 s (4 rounds of 2 s, four engines with rum; load 9-29) |

Under this host's load no run reached the early-stop spread; on a quiet host the minimum of four rounds is about 15 min
(full) and 6 min (150k tier). Results are kept as small tables per run (`cells.tsv`, `builds.tsv`, `summary.tsv`,
`machine.txt` with the CPU, engine versions and load) in the result directory, and the scratch cluster is removed when
the run ends.
