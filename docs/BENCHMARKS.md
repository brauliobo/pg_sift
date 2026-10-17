# Benchmarks

sift and its two variants (`positions = off`, `phrase_index = pairs`) against ParadeDB pg_search 0.26.1, Timescale pg_textsearch 1.5.1,
rum 1.3.15 and pgx-bm25 1.0 on two public datasets, measured with `bench/bench all --full --serial` (articles through
`bench/steps/articles.sh`, places through `bench/steps/places.sh`; method, datasets, licenses and checksums: `bench/README.md`). Every engine
indexes the same rows with the same analysis (lowercase, no stemming, no stopwords; pg_textsearch and rum through the PostgreSQL `simple`
text search configuration, pgx-bm25 through a `simple_stem` dictionary that does not stem) and answers the same queries from the same cache
state. The tables report medians over 2-3 interleaved rounds with their standard errors. The engines, in the order of every table and
chart, are listed in `bench/engines.tsv`. The numbers of every table below are also in [results/articles.tsv](results/articles.tsv) and
[results/places.tsv](results/places.tsv), one row per measurement (columns: `bench/README.md`), for charts.

Host: AMD Ryzen 7 9700X (8 cores, 16 hardware threads), 188 GB RAM, Linux 7.2, PostgreSQL 18.6, scratch data on tmpfs. sift is built from
`cc30fc2b299d` (main when the session ran), pg_search from the ParadeDB 0.26.1 package, pg_textsearch 1.5.1 from the v1.5.1 tag (commit
`fa142d049d08`), rum 1.3.15 and pgx-bm25 1.0 with PGXS (tarball and commit checksums: `bench/versions.env`).

**The CPU budget is a parameter of a run** (`--cpus`, `--load-cpus`, `--serial`; `bench/README.md`), printed in the machine description and in
the header of every table and recorded as the `cpu_budget` rows of [results/](results/); the charts caption it. **Every table in this document
was measured with the same default budget: one cluster (`--serial`, the engines interleaved cell by cell with the order rotated every round)
on 8 server threads (the 8 least busy at the start: 4 whole cores) and 4 load generator threads, disjoint, 8 clients in the articles
cells, one client in the places cells.** Index sizes and build times are for one build per engine. Cells have 2 rounds and a third only when the
relative standard error after two is above 5%; a table flags a relative standard error above 10% with `!`, and with n = 2-3 the standard error
is itself estimated from two or three numbers: read it together with the minimum-maximum range, and do not read differences between cells
that overlap.

The host was shared. Two GPU inference services kept about two hardware threads busy throughout, a browser and other tools ran, and a second benchmark of
this repository (another session's, with its own scratch root) was running at times during the later part of the work, including the places session published here: it terminated one pgbench of that session by a stray `pkill`
(one cell, pg_search one-letter filtered top-20, lost a round; it was measured again alone, three rounds, and replaces the damaged cell) and sent a SIGINT to the newest pgbench once. Every raw result file of the sessions was audited afterwards for failed cells (`nan`, `error`, zero throughput),
and for cells whose rounds differ by more than 2.5 times: none is left. The articles session ran before that and, as far as I could see, with no other run of this project on the host, but I cannot exclude one. The load average of the whole host was 7-23 (median 17) during the articles cells (8 pgbench clients are part of it), 4.8-17 (median 11.5) during the places cells and 4-35 during the first places session. Round-to-round variation of the fast
engines is therefore large (the articles cells of sift have a relative standard error of 7.5% in the median and 15 of the 49 cells are above 10%) and the ratios below carry that noise. Compare engines within a table, not across
tables, tiers or hosts: **ratios depend on the CPU the engines get and on what else the host runs**.


## What is native, what is emulated, what is NA

Every cell of every table and chart has a number except where a table says `error`: every workload can be expressed on every engine, natively or by an
emulation that is documented here, and the emulations are checked against an exact SQL oracle (Correctness, below). Two engines have no native query
for some places workloads (a typed prefix ranked by BM25; a field boost), and pgx-bm25 refuses wildcards shorter than three letters.

| engine | articles | places workload | how it runs |
|---|---|---|---|
| sift, sift `positions = off`, sift `pairs` | all | all | native (`fields = 'name', prefix_index = 3`; the variants differ only in the reloption) |
| pg_search | all | all | native (`edge_ngram(1, 15)` fields, a typed prefix is a term) |
| pg_textsearch | all (ranked AND, phrase, mixed, filter, join through the tsquery filter and standalone scores) | count, unordered scan | native: the prefix tsquery `'gra':* & 'ho':*` through its `bm25` index (a tsquery `@@` filter) |
| pg_textsearch | | top-20, boost, filtered top-20 | **emulation**: `bm25` indexes on stored columns of the edge n-grams of the words |
| rum | all | top-20, filtered, count, unordered scan | native: prefix tsquery, `<=>` ranking |
| rum | | boost | **emulation**: a second tsvector with weights |
| pgx-bm25 | all (`join` works at this size; it is an `error` cell at the 25k quick tier) | all words of 3 letters or more | native: `bm25_wildcard` per word, `bm25_boost` for the name field; the wildcard expansion limit is raised to 100000 (the default 1000 made `san*` an error at 236,262 places) |
| pgx-bm25 | | words of 1-2 letters | **emulation inside the same index**: two stored columns hold the 1-2 letter n-grams of the words of the two sections and the word is a whole term there; the extension refuses a wildcard under three letters |


The pg_textsearch emulation exists because it has no prefix scoring (a text search configuration has no edge n-grams) and no per-field
boost. The harness stores, next to the native index and in the same database, `ng_text` and `ng_name`, generated columns holding the
edge n-grams (1-15 letters) of every word of the all-fields text and of the name field (`"grand ho"` becomes `g gr gra gran grand h ho`), the
analysis of pg_search's `edge_ngram(1, 15)`, and a `bm25` index on each. A typed query is then whole terms: the matches are the places
whose `ng_text` matches `plainto_tsquery(typed)` (a derived table, `OFFSET 0`, through the index) and they are ordered by the BM25 score of
the distinct n-grams of the typed words (pg_search scores each n-gram once). The boost is the weighted sum of two bm25 scores,
`score(ng_text) + 3 * score(ng_name)`, the name counting three times like the other engines' `name:{...}^3`. The score is
written `(ng_text <@> query) * 1` so that the planner scores the matches standalone: a bare `ORDER BY ng_text <@> query` over the `@@`
scan takes the index scan's current score, which that scan does not compute, and returns the matches unranked. This is an exact
BM25 ranking of all the matches, so its cost follows the number of matches: it is a faithful emulation of what the engine can express,
not a tuned one, and it is the reason for the p1 and p2 latencies below. The extra columns and indexes are not in the index-size and
build-time charts (they show the native index; the emulation has its own table below).

rum has no field boost either. The emulation is a second stored tsvector, `setweight(name words, 'A') || setweight(other fields,
'B')`, with its own `rum_tsvector_ops` index, ranked with `<=>`; rum's weight classes are fixed (A 1.0, B 0.4) so the name counts 2.5 times, the
closest to the x3 of the other engines. rum's `<=>` is a proximity ranking, not BM25, so the weighted ranking agrees with the BM25
oracle about as much as its plain ranking does (Correctness).

## Articles: Simple English Wikipedia, 218,058 articles (full tier)

8 clients, 2-3 interleaved rounds of 3 s per cell (a warm-up cell in the first round; cells whose relative standard error after two rounds is
above 5% get the third), queries are 2-15 consecutive words of random articles (1000 each as AND, OR and phrase); `bench/bench all --full --serial`.
Throughput in queries per second, median ± standard error (minimum-maximum over the rounds; `!` marks a relative standard error above 10%),
with the size of the index and the time of its `CREATE INDEX` (rum also needs a stored `tsvector` column: 19.0 s to fill it, 437.7 MiB of
table growth, neither counted in the index column):

| engine | index (MiB) | build (s) | table growth (MiB) | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 130.9 | 2.2 | 0.0 | 10279 ± 2081 ! (8641-15541) | 2567 ± 268 ! (2529-3351) | 8239 ± 1300 ! (8144-12092) | 4863 ± 766 ! (4143-6716) | 6976 ± 1387 ! (6817-11054) | 4067 ± 315 (3790-4843) | 1657 ± 26 (1631-1683) |
| sift `positions = off` | 52.5 | 1.7 | 0.0 | 10732 ± 1781 ! (9545-15382) | 2724 ± 248 (2493-3325) | 1999 ± 114 (1980-2331) | 3113 ± 300 (3024-3966) | 956 ± 136 ! (761-1229) | 3678 ± 362 (3385-4587) | 830 ± 62 (793-997) |
| sift `pairs` | 351.5 | 8.9 | 0.0 | 10387 ± 1418 ! (10361-14629) | 2665 ± 225 (2497-3241) | 8746 ± 1357 ! (8656-12773) | 5213 ± 549 ! (4876-6664) | 11308 ± 2214 ! (7974-15623) | 3638 ± 438 ! (3484-4868) | 1646 ± 59 (1586-1705) |
| pg_search | 183.6 | 3.1 | 0.0 | 2679 ± 334 ! (2533-3599) | 888 ± 64 (813-1030) | 2520 ± 213 (2341-3051) | 1370 ± 90 (1313-1606) | 1501 ± 173 ! (1258-1855) | 2278 ± 204 (2021-2719) | 186 ± 6.3 (180-192) |
| pg_textsearch | 147.3 | 10.0 | 0.0 | 6 ± 0.4 (6-7) | 2392 ± 245 ! (2156-2980) | 27 ± 1.4 (25-30) | 7 ± 0.4 (7-8) | 14 ± 0.9 (14-17) | 14 ± 0.9 (14-17) | 14 ± 0.1 (14-14) |
| rum | 309.9 | 14.9 | 437.7 | 959 ± 72 (913-1149) | 8 ± 0.0 (8-8) | 34 ± 1.3 (33-36) | 16 ± 0.0 (16-16) | 39 ± 1.6 (38-43) | 16 ± 0.3 (16-16) | 13 ± 0.3 (13-13) |
| pgx-bm25 | 204.4 | 11.7 | 0.0 | 28 ± 2.2 (25-33) | 22 ± 1.3 (19-23) | 18 ± 0.3 (18-19) | 19 ± 0.7 (18-21) | 18 ± 0.5 (17-18) | 20 ± 0.3 (20-21) | 18 ± 0.8 (17-19) |

Ratio of queries per second to pg_search (the baseline of every ratio in this document):

| engine | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift / pg_search | 3.84x | 2.89x | 3.27x | 3.55x | 4.65x | 1.79x | 8.91x |
| sift `positions = off` / pg_search | 4.01x | 3.07x | 0.79x | 2.27x | 0.64x | 1.61x | 4.46x |
| sift `pairs` / pg_search | 3.88x | 3.00x | 3.47x | 3.80x | 7.53x | 1.60x | 8.85x |
| pg_textsearch / pg_search | 0.00x | 2.69x | 0.01x | 0.00x | 0.01x | 0.01x | 0.08x |
| rum / pg_search | 0.36x | 0.01x | 0.01x | 0.01x | 0.03x | 0.01x | 0.07x |
| pgx-bm25 / pg_search | 0.01x | 0.02x | 0.01x | 0.01x | 0.01x | 0.01x | 0.10x |

Index size and build time move with the build: pg_search's size depends on how its parallel workers split the segments (161-185 MiB over the
builds of the sessions that went into this document; 183.6 MiB in this one), build times move with the host load; sift's sizes do not.

**The positions trade-off, sift against sift `positions = off`** (same session, queries per second, medians; see the table for the ranges):

| | index (MiB) | build (s) | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift (positions on, the default) | 130.9 | 2.2 | 10,279 | 2,567 | 8,239 | 4,863 | 6,976 | 4,067 | 1,657 |
| sift `positions = off` | 52.5 | 1.7 | 10,732 | 2,724 | 1,999 | 3,113 | 956 | 3,678 | 830 |
| positions on / off | 2.49x | 1.29x | 0.96x | 0.94x | 4.12x | 1.56x | 7.30x | 1.11x | 2.00x |

Word positions make the index 2.5 times larger and the build 30% slower and decide phrases from the index: phrase, mixed, `count(*)` and the join
are 1.6-7.3 times faster; AND, OR and the filtered top-10 do not change beyond the noise of the cells (their ranges overlap). Without them a phrase
is checked against the heap after the term intersection, which makes phrase (0.79x) and `count(*)` (0.64x) **slower than pg_search**; with them sift is ahead of
pg_search in every class.

How pg_textsearch is configured: a `bm25` index with `text_config = 'simple'` on the body (the analysis of the other engines), the
database `default_text_search_config` set to `simple` (its `@@` filter parses the column with it) and `enable_seqscan = off` in its
database, which the extension's README suggests for getting its index plans; adapter: `bench/engines/pg_textsearch.sh`. A ranked
OR is `ORDER BY body <@> to_bm25query(terms, 'docs_fts') LIMIT 10`, which the index answers with block-max WAND. It has no AND,
phrase or NOT inside that scan: those are the tsquery `@@` filter through the same index, and the extension does not combine
filter and ranking in one scan. Written as `WHERE body @@ q ORDER BY body <@> ... LIMIT 10` the planner chooses a sequential
scan that tokenizes every document (3 s per query at 25,000 articles), so the harness writes the AND, phrase, filter and join
queries as the extension's plan: `FROM (SELECT * FROM docs WHERE body @@ q OFFSET 0) docs ORDER BY (body <@> ...) * 1`, the filter
through the index and the standalone scores of its matches sorted. The `* 1` matters: without it the planner replaces the score by the
current score of the `@@` scan, which that scan does not compute, and the top-10 comes back in index order, not ranked (at the 150k tier, 53
of the first 200 AND queries have more than 10 matches and returned an arbitrary top-10). Scoring the matches ranks them and costs roughly
50-100 microseconds per match, which is why AND is 6 queries per second at the full tier. Without `enable_seqscan = off` the planner counts a
disjunction of common words with a sequential scan too: 0.25 queries per second for `count(*)` against 37.9 with it off (a separate full-tier
measurement of an earlier session, 8 clients, 8 s).
Correctness: its match sets and `count(*)` equal the tsvector oracle for 200 substrings in the and, or, phrase and not modes
(`bench/bench check oracle pg_textsearch 200`, PG parser tokens like gin and rum, 25,000 articles, earlier session); its top-10 overlap with pgx-bm25's BM25
is below (`check rank`, 200 substrings; a soft check: it scores the PG parser's tokens and a bag of terms).

How rum is configured: a stored `tsvector` column (`to_tsvector('simple', body)`, a generated column, so the heap grows by
437.7 MiB and filling it takes 19.0 s on top of the 14.9 s `CREATE INDEX`) with a `rum_tsvector_ops` index, the same column gin
would index; adapter: `bench/engines/rum.sh`. A query is `WHERE tsv @@ tsquery ORDER BY tsv <=> tsquery LIMIT 10`, rum's own
ranking operator (`<=>` is rum's ts_rank-style distance from the positions it stores, not BM25).
Correctness: its match sets and `count(*)` equal the tsvector oracle for 200 substrings in the and, or, phrase and not modes
(`bench/bench check oracle rum 200`, 25,000 articles, the PG parser's tokens like gin and pg_textsearch, earlier session).

How pgx-bm25 is configured: a `bm25_native` index on the body (`key_field = 'id'`, `language = 'simple'`, `stopwords = 'none'`; its analyzer resolves
`<language>_stem` in `pg_catalog`, so a `simple_stem` dictionary built on the `simple` template gives it the no-stemming analysis of the others), jsonb
queries through `@@@` (filter) and `&@@` (ranking), and `bm25_native.max_match_memory` raised to 2 GB (the extension materializes the whole match set of the queries that
WAND does not answer; the default 256 MB fails most queries of this trace); adapter: `bench/engines/bm25.sh`. It runs every class at 18-28 queries per second (0.01-0.10x
pg_search), with a flat profile that suggests a per-match cost like pg_textsearch's; I did not profile it.

Cold cache (index, heap and TOAST evicted from shared buffers before every query; `EXPLAIN ANALYZE` execution time of 12 queries per class,
`bench/bench cold --full --serial`), p50 / maximum in ms. The scratch data is on tmpfs, so there is no disk behind the evicted pages: this is the
cost of rebuilding the buffers from memory-backed files, not of disk reads:

| class | sift | sift `positions = off` | sift `pairs` | pg_search | pg_textsearch | rum | pgx-bm25 |
|---|---:|---:|---:|---:|---:|---:|---:|
| and | 0.88 / 3.57 | 0.96 / 2.23 | 0.80 / 3.23 | 2.89 / 7.43 | 2.23 / 79.31 | 1.05 / 4.97 | 267.95 / 462.90 |
| or | 1.94 / 4.22 | 1.90 / 6.91 | 1.74 / 7.33 | 5.34 / 10.12 | 1.64 / 6.90 | 283.51 / 978.39 | 232.01 / 403.52 |
| phrase | 1.12 / 2.22 | 0.82 / 23.59 | 1.03 / 1.43 | 2.37 / 7.86 | 1.44 / 474.42 | 130.67 / 373.05 | 374.42 / 769.00 |
| mixed | 2.07 / 3.36 | 1.36 / 23.93 | 1.07 / 2.89 | 3.11 / 17.42 | 2.88 / 419.64 | 239.69 / 784.24 | 267.39 / 540.20 |
| count | 0.83 / 1.54 | 0.83 / 27.03 | 0.81 / 2.14 | 2.93 / 12.18 | 2.80 / 434.46 | 173.39 / 241.18 | 371.27 / 526.68 |
| filter | 1.95 / 3.47 | 1.26 / 6.72 | 1.02 / 3.50 | 3.11 / 11.01 | 2.06 / 432.50 | 227.67 / 711.38 | 254.35 / 558.38 |
| join | 1.24 / 9.67 | 2.59 / 24.27 | 1.07 / 14.43 | 5.55 / 83.81 | 3.08 / 418.81 | 242.70 / 726.72 | 272.36 / 563.62 |

The p50 of sift is 0.8-2.6 ms in every class and variant, pg_search's 2.4-5.5 ms, pg_textsearch's 1.4-3.1 ms, rum's 1.0 ms for AND and 130-284 ms for every
other class, pgx-bm25's 230-375 ms. pg_textsearch's maximum is 418-475 ms in every class but AND and OR (79 and 7 ms), rum's up to 978 ms, pgx-bm25's up to 769 ms. sift's cold maxima are 1.5-9.7 ms with positions on (up to 14.4 ms with `pairs`); `positions = off` has the slow maxima of its heap recheck (phrase 23.6 ms, mixed 23.9, `count(*)` 27.0, join 24.3 ms) that positions remove.

Reading the numbers:

- **Sizes and builds.** sift's default index is 130.9 MiB, 1.40 times smaller than pg_search's 183.6 MiB and 0.89 times pg_textsearch's 147.3; with `positions = off` it is
  52.5 MiB, 3.5 times smaller than pg_search. The old claim of 47.9 MiB and 3.8x smaller was the index without positions (and before the 8K bitmap groups added 9.6%).
  `pairs` is 351.5 MiB, 1.9 times pg_search's and 2.7 times the default's. sift builds in 2.2 s (1.7 s without positions), pg_search in 3.1 s, `pairs` in 8.9 s,
  pg_textsearch in 10.0 s, pgx-bm25 in 11.7 s and rum in 14.9 s after 19.0 s to fill its stored column (a table that is 437.7 MiB larger, 748 MiB with the index).
  pg_search is within 1.4 times of sift on the build; the other three engines take 4.5-7 times as long (15 times with rum's column).
- **Throughput.** With positions on, sift is ahead of pg_search in every class: AND 3.84x, OR 2.89x, phrase 3.27x, mixed 3.55x, `count(*)` 4.65x, filtered top-10 1.79x, join 8.91x.
  These ratios carry the noise of a loaded host (sift's relative standard errors are up to 20%); the order of the engines does not change within the ranges of the rounds,
  but a ratio of 3.8 is not distinguishable from 3.0 or 4.5 in this session. `pairs` buys nothing beyond the noise over the default except `count(*)` (11,308 against 6,976 queries per second), for
  2.7 times the index and 4 times the build.
- **Where sift is not ahead.** The ranked OR is level with pg_textsearch, whose native query it is (block-max WAND): 2,567 ± 268 against 2,392 ± 245 queries per second, ranges 2,529-3,351 and 2,156-2,980,
  and 2.89 times pg_search. Without positions, phrase and `count(*)` are behind pg_search (0.79x and 0.64x), Larger cold maxima belong to the other engines (pg_textsearch up to 475 ms, rum up to 978 ms, pgx-bm25 up to 769 ms, against sift's 1.5-9.7 ms).
- **pg_textsearch** wins only the bare ranked OR (above). Everything that is not a bare disjunction goes through the tsquery filter and then scores the matches one by one (see the configuration above): AND runs at 6 queries
  per second and phrase (27), mixed (7), `count(*)` (14), filter (14) and join (14) at 0.00-0.08x of pg_search. The cost follows the number of matching articles, which the 2-15 word queries of the trace make large; I did
  not profile it further. The extension's README says that combining the filter with the ranking is not yet one index scan.
- **rum** is behind pg_search in every class: its best class is AND at 959 queries per second (0.36x of pg_search, 0.09x of sift), and every other class is two orders of magnitude lower: OR 8, phrase 34, `count(*)` 39,
  mixed 16, filter 16 and join 13. It is the largest index after `pairs` (309.9 MiB, 748 MiB with its column) and the slowest build. I did not profile it; its `<=>` ordering and the `@@` recheck of the stored `tsvector`
  (positions are checked in the heap for phrases) both visit every match of these common-word queries.
- **pgx-bm25** is 0.01-0.10x of pg_search in every class and builds in 11.7 s into a 204.4 MiB index (1.1 times pg_search's), the only engine of the field whose cold p50 is 230-370 ms in every class including AND.


## Places: GeoNames cities500, 236,262 places (full tier)

Multi-field autocomplete as an application would run it: sift (and its variants) index the name section and the all-fields text with
`fields = 'name', prefix_index = 3`, pg_search indexes `edge_ngram(1, 15)` fields for both, pgx-bm25 a four-column `bm25_native` index (below); every typed word is a
prefix. `sift.max_expansions` is set to 100000 for the database so that every typed prefix keeps scoring as the max over its terms: at the
default 512, prefixes of four or more letters of common words (`pari*`, `san*`) read as one term instead. Single client, 2-3 interleaved rounds of 2 s
per cell (`bench/bench run --dataset places --tier full --serial`, host load 5-17). Every engine lives in a database of its own, a copy of the loaded table
(the extra columns of rum, pg_textsearch and pgx-bm25 do not enlarge the others' table; no engine pays for another's hooks), and every copy starts from a vacuumed table so that
the visibility map is complete (a harness bug fixed for this session: with only an `ANALYZE` of the template the first engine's `count(*)` read the heap of the pages that were not all-visible and
measured 2-3 times slower than the same index in the next database). Index size, build time and p50 latency in ms of the top-20 workload per
query class (pg_textsearch is the emulation described above, pgx-bm25 is native for words of 3 letters or more and emulated inside its index for 1-2 letters). Single-client cells under 0.3 ms
are near the resolution of the measurement; the standard errors say how much a cell moved between the rounds, and the differences between the sift variants (0.81x-1.30x, median 1.0x) are that noise, not an effect:

| engine | index (MiB) | build (s) | p1 | p2 | p3 | p4 | w1 | a2 | a2s | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 50.2 | 1.4 | 0.26 ± 0.012 | 0.23 ± 0.001 | 0.24 ± 0.004 | 0.27 ± 0.004 | 0.17 ± 0.006 | 0.16 ± 0.001 | 0.17 ± 0.004 | 0.20 ± 0.002 | 0.20 ± 0.014 |
| sift `positions = off` | 44.4 | 1.4 | 0.24 ± 0.007 | 0.23 ± 0.002 | 0.26 ± 0.018 | 0.26 ± 0.004 | 0.16 ± 0.015 | 0.18 ± 0.020 ! | 0.16 ± 0.002 | 0.20 ± 0.010 | 0.21 ± 0.006 |
| sift `pairs` | 83.3 | 2.3 | 0.24 ± 0.003 | 0.21 ± 0.011 | 0.26 ± 0.002 | 0.27 ± 0.015 | 0.15 ± 0.011 | 0.18 ± 0.004 | 0.17 ± 0.013 | 0.19 ± 0.002 | 0.21 ± 0.009 |
| pg_search | 142.0 | 2.9 | 0.85 ± 0.022 | 1.04 ± 0.025 | 1.17 ± 0.026 | 1.22 ± 0.029 | 1.30 ± 0.011 | 1.69 ± 0.025 | 1.47 ± 0.004 | 2.68 ± 0.061 | 2.06 ± 0.059 |
| pg_textsearch | 68.3 | 2.4 | 1002.33 ± 4.91 | 404.19 ± 4.60 | 14.83 ± 0.56 | 4.45 ± 0.038 | 2.80 ± 0.11 | 2.72 ± 0.10 | 2.63 ± 0.008 | 2.83 ± 0.026 | 2.85 ± 0.13 |
| rum | 94.9 | 2.7 | 107.99 ± 4.44 | 14.25 ± 0.11 | 4.42 ± 0.23 | 1.22 ± 0.008 | 0.31 ± 0.004 | 9.85 ± 0.17 | 49.60 ± 2.03 | 10.29 ± 0.075 | 5.73 ± 0.19 |
| pgx-bm25 | 103.3 | 3.2 | 33.71 ± 1.84 | 6.67 ± 0.31 | 72.26 ± 0.69 | 43.01 ± 2.29 | 4.75 ± 0.43 | 9.70 ± 0.37 | 26.25 ± 0.86 | 24.34 ± 0.86 | 15.42 ± 0.55 |

Extra columns and indexes of the emulated workloads, built after the native index in the same database (not in the table above):

| engine | index (MiB) | build (s) | column fill (s) | table growth (MiB) |
|---|---:|---:|---:|---:|
| pg_textsearch | 364.5 | 11.8 | 9.5 | 179.3 |
| rum | 95.0 | 2.8 | 5.0 | 61.9 |

pg_textsearch needs 433 MiB in all for its ranked prefix workloads (68.3 native, 364.5 for the n-gram indexes; plus 179.3 MiB of stored columns), 3.1 times pg_search's 142.0 MiB.
pgx-bm25's four stored columns grow the table by 54.3 MiB (4.2 s to fill) and are not in its 103.3 MiB; rum's native `tsvector` column grows it by 60.4 MiB (1.6 s).

p50 latency in ms of every workload, with the ratio to sift (workloads: top-20, name boost x3, filtered `country_code = 'US'`, `count(*)`, unordered
`population >= 100000`; places queries are prefixes and ANDs and never reach the phrase recheck):

**top-20**

| class | sift | sift `positions = off` | sift `pairs` | pg_search | pg_textsearch | rum | pgx-bm25 |
|---|---:|---:|---:|---:|---:|---:|---:|
| p1 | 0.26 ± 0.012 | 0.24 ± 0.007 (0.92x) | 0.24 ± 0.003 (0.91x) | 0.85 ± 0.022 (3.28x) | 1002.33 ± 4.91 (3855.11x) | 107.99 ± 4.44 (415.36x) | 33.71 ± 1.84 (129.66x) |
| p2 | 0.23 ± 0.001 | 0.23 ± 0.002 (0.97x) | 0.21 ± 0.011 (0.90x) | 1.04 ± 0.025 (4.44x) | 404.19 ± 4.60 (1730.99x) | 14.25 ± 0.11 (61.01x) | 6.67 ± 0.31 (28.55x) |
| p3 | 0.24 ± 0.004 | 0.26 ± 0.018 (1.08x) | 0.26 ± 0.002 (1.06x) | 1.17 ± 0.026 (4.81x) | 14.83 ± 0.56 (60.76x) | 4.42 ± 0.23 (18.09x) | 72.26 ± 0.69 (296.13x) |
| p4 | 0.27 ± 0.004 | 0.26 ± 0.004 (0.96x) | 0.27 ± 0.015 (1.01x) | 1.22 ± 0.029 (4.56x) | 4.45 ± 0.038 (16.64x) | 1.22 ± 0.008 (4.57x) | 43.01 ± 2.29 (160.77x) |
| w1 | 0.17 ± 0.006 | 0.16 ± 0.015 (0.94x) | 0.15 ± 0.011 (0.88x) | 1.30 ± 0.011 (7.62x) | 2.80 ± 0.11 (16.49x) | 0.31 ± 0.004 (1.83x) | 4.75 ± 0.43 (27.94x) |
| a2 | 0.16 ± 0.001 | 0.18 ± 0.020 ! (1.11x) | 0.18 ± 0.004 (1.13x) | 1.69 ± 0.025 (10.35x) | 2.72 ± 0.10 (16.62x) | 9.85 ± 0.17 (60.25x) | 9.70 ± 0.37 (59.35x) |
| a2s | 0.17 ± 0.004 | 0.16 ± 0.002 (0.97x) | 0.17 ± 0.013 (1.06x) | 1.47 ± 0.004 (8.85x) | 2.63 ± 0.008 (15.89x) | 49.60 ± 2.03 (299.70x) | 26.25 ± 0.86 (158.61x) |
| a3 | 0.20 ± 0.002 | 0.20 ± 0.010 (0.98x) | 0.19 ± 0.002 (0.96x) | 2.68 ± 0.061 (13.43x) | 2.83 ± 0.026 (14.19x) | 10.29 ± 0.075 (51.57x) | 24.34 ± 0.86 (121.99x) |
| x2 | 0.20 ± 0.014 | 0.21 ± 0.006 (1.01x) | 0.21 ± 0.009 (1.05x) | 2.06 ± 0.059 (10.13x) | 2.85 ± 0.13 (14.02x) | 5.73 ± 0.19 (28.22x) | 15.42 ± 0.55 (75.95x) |

**name boost x3**

| class | sift | sift `positions = off` | sift `pairs` | pg_search | pg_textsearch | rum | pgx-bm25 |
|---|---:|---:|---:|---:|---:|---:|---:|
| p1 | 0.32 ± 0.039 ! | 0.35 ± 0.026 (1.08x) | 0.33 ± 0.022 (1.04x) | 3.00 ± 0.021 (9.34x) | 3107.60 ± 28.75 (9681.00x) | 113.84 ± 5.29 (354.64x) | 35.17 ± 0.32 (109.58x) |
| p2 | 0.46 ± 0.002 | 0.45 ± 0.047 ! (0.98x) | 0.46 ± 0.011 (1.00x) | 2.98 ± 0.034 (6.46x) | 257.32 ± 1.21 (558.17x) | 13.60 ± 0.025 (29.51x) | 6.69 ± 0.14 (14.50x) |
| p3 | 0.33 ± 0.014 | 0.34 ± 0.012 (1.03x) | 0.36 ± 0.011 (1.11x) | 2.59 ± 0.024 (7.92x) | 21.03 ± 0.066 (64.30x) | 4.35 ± 0.21 (13.30x) | 72.62 ± 1.56 (222.07x) |
| p4 | 0.41 ± 0.019 | 0.37 ± 0.027 (0.90x) | 0.39 ± 0.020 (0.95x) | 2.36 ± 0.11 (5.79x) | 5.71 ± 0.045 (13.99x) | 1.06 ± 0.051 (2.60x) | 41.48 ± 0.077 (101.68x) |
| w1 | 0.17 ± 0.001 | 0.17 ± 0.003 (1.00x) | 0.17 ± 0.005 (1.00x) | 2.43 ± 0.015 (14.23x) | 2.84 ± 0.025 (16.66x) | 0.27 ± 0.010 (1.60x) | 4.83 ± 0.47 (28.31x) |
| a2 | 0.23 ± 0.010 | 0.22 ± 0.008 (0.98x) | 0.22 ± 0.006 (0.95x) | 3.28 ± 0.066 (14.37x) | 2.84 ± 0.039 (12.44x) | 9.80 ± 0.11 (42.98x) | 9.34 ± 0.33 (40.96x) |
| a2s | 0.22 ± 0.004 | 0.21 ± 0.002 (0.96x) | 0.22 ± 0.006 (0.98x) | 2.89 ± 0.024 (13.15x) | 2.85 ± 0.077 (12.94x) | 44.34 ± 0.94 (201.53x) | 26.98 ± 1.84 (122.61x) |
| a3 | 0.25 ± 0.007 | 0.25 ± 0.002 (0.99x) | 0.26 ± 0.003 (1.04x) | 5.61 ± 0.20 (22.07x) | 2.87 ± 0.059 (11.31x) | 9.91 ± 0.38 (39.02x) | 23.43 ± 0.012 (92.24x) |
| x2 | 0.24 ± 0.009 | 0.24 ± 0.004 (0.99x) | 0.23 ± 0.008 (0.96x) | 4.00 ± 0.069 (16.75x) | 2.90 ± 0.062 (12.12x) | 5.42 ± 0.24 (22.69x) | 14.98 ± 1.29 (62.66x) |

**filtered top-20**

| class | sift | sift `positions = off` | sift `pairs` | pg_search | pg_textsearch | rum | pgx-bm25 |
|---|---:|---:|---:|---:|---:|---:|---:|
| p1 | 5.85 ± 0.69 ! | 6.94 ± 0.49 (1.19x) | 6.85 ± 0.13 (1.17x) | 12.82 ± 0.63 (2.19x) | 238.21 ± 3.44 (40.69x) | 101.32 ± 0.47 (17.31x) | 32.80 ± 1.90 (5.60x) |
| p2 | 1.61 ± 0.12 | 1.96 ± 0.084 (1.22x) | 1.65 ± 0.016 (1.03x) | 5.53 ± 0.11 (3.43x) | 73.19 ± 3.43 (45.46x) | 13.69 ± 0.17 (8.50x) | 7.51 ± 0.19 (4.67x) |
| p3 | 0.51 ± 0.057 ! | 0.47 ± 0.017 (0.91x) | 0.51 ± 0.012 (1.00x) | 5.54 ± 0.008 (10.85x) | 7.12 ± 0.075 (13.95x) | 3.49 ± 0.077 (6.84x) | 73.25 ± 2.57 (143.62x) |
| p4 | 0.31 ± 0.009 | 0.28 ± 0.009 (0.93x) | 0.29 ± 0.009 (0.96x) | 4.54 ± 0.34 (14.82x) | 2.91 ± 0.080 (9.50x) | 0.65 ± 0.032 (2.12x) | 41.31 ± 0.092 (135.00x) |
| w1 | 0.16 ± 0.002 | 0.17 ± 0.002 (1.04x) | 0.15 ± 0.002 (0.95x) | 4.95 ± 0.14 (30.47x) | 2.41 ± 0.029 (14.86x) | 0.24 ± 0.009 (1.46x) | 4.77 ± 0.035 (29.34x) |
| a2 | 0.21 ± 0.030 ! | 0.19 ± 0.017 (0.89x) | 0.20 ± 0.003 (0.94x) | 5.28 ± 0.35 (25.38x) | 2.46 ± 0.029 (11.83x) | 9.57 ± 0.018 (45.99x) | 9.83 ± 0.025 (47.26x) |
| a2s | 0.18 ± 0.001 | 0.19 ± 0.004 (1.01x) | 0.20 ± 0.026 ! (1.06x) | 5.55 ± 0.76 ! (30.24x) | 2.53 ± 0.018 (13.78x) | 42.81 ± 1.57 (233.29x) | 29.80 ± 2.02 (162.37x) |
| a3 | 0.21 ± 0.007 | 0.20 ± 0.002 (0.96x) | 0.20 ± 0.001 (0.94x) | 6.11 ± 0.36 (29.01x) | 2.42 ± 0.006 (11.52x) | 10.10 ± 0.033 (47.99x) | 23.80 ± 0.18 (113.09x) |
| x2 | 0.19 ± 0.001 | 0.20 ± 0.009 (1.04x) | 0.20 ± 0.022 ! (1.06x) | 5.76 ± 0.24 (30.32x) | 2.40 ± 0.011 (12.65x) | 5.47 ± 0.23 (28.79x) | 15.15 ± 0.43 (79.73x) |

**count(*)**

| class | sift | sift `positions = off` | sift `pairs` | pg_search | pg_textsearch | rum | pgx-bm25 |
|---|---:|---:|---:|---:|---:|---:|---:|
| p1 | 0.13 ± 0.001 | 0.12 ± 0.001 (0.97x) | 0.12 ± 0.002 (0.96x) | 0.68 ± 0.006 (5.39x) | 454.90 ± 14.27 (3624.68x) | 35.78 ± 0.19 (285.12x) | 51.05 ± 1.64 (406.78x) |
| p2 | 0.14 ± 0.056 ! | 0.14 ± 0.001 (0.99x) | 0.14 ± 0.002 (0.99x) | 1.01 ± 0.008 (7.37x) | 97.85 ± 0.57 (714.24x) | 9.88 ± 0.53 (72.14x) | 10.41 ± 2.84 ! (76.01x) |
| p3 | 0.12 ± 0.002 | 0.15 ± 0.040 ! (1.30x) | 0.12 ± 0.044 ! (1.00x) | 1.12 ± 0.41 ! (9.74x) | 6.28 ± 1.06 ! (54.60x) | 1.82 ± 0.12 (15.84x) | 73.49 ± 2.16 (639.01x) |
| p4 | 0.14 ± 0.009 | 0.13 ± 0.005 (0.96x) | 0.14 ± 0.022 ! (1.01x) | 1.25 ± 0.021 (9.09x) | 1.38 ± 0.043 (10.02x) | 0.50 ± 0.028 (3.63x) | 42.73 ± 0.15 (309.64x) |
| w1 | 0.11 ± 0.007 | 0.12 ± 0.003 (1.07x) | 0.13 ± 0.001 (1.12x) | 1.78 ± 0.029 (15.86x) | 0.79 ± 0.005 (7.03x) | 0.20 ± 0.012 (1.81x) | 4.97 ± 0.035 (44.34x) |
| a2 | 0.13 ± 0.005 | 0.14 ± 0.006 (1.07x) | 0.15 ± 0.002 (1.16x) | 1.97 ± 0.082 (15.41x) | 2.09 ± 0.043 (16.32x) | 4.67 ± 0.22 (36.45x) | 9.47 ± 0.51 (73.98x) |
| a2s | 0.14 ± 0.003 | 0.13 ± 0.010 (0.96x) | 0.13 ± 0.004 (0.95x) | 1.79 ± 0.048 (13.09x) | 9.71 ± 0.25 (70.89x) | 38.70 ± 1.73 (282.50x) | 26.70 ± 0.98 (194.92x) |
| a3 | 0.16 ± 0.013 | 0.15 ± 0.004 (0.93x) | 0.15 ± 0.009 (0.93x) | 3.81 ± 0.081 (23.26x) | 0.82 ± 0.024 (4.99x) | 4.79 ± 0.35 (29.21x) | 24.21 ± 1.01 (147.62x) |
| x2 | 0.15 ± 0.006 | 0.14 ± 0.009 (0.93x) | 0.16 ± 0.005 (1.05x) | 2.72 ± 0.065 (17.82x) | 0.76 ± 0.031 (5.01x) | 2.72 ± 0.098 (17.84x) | 15.17 ± 0.84 (99.48x) |

**unordered scan**

| class | sift | sift `positions = off` | sift `pairs` | pg_search | pg_textsearch | rum | pgx-bm25 |
|---|---:|---:|---:|---:|---:|---:|---:|
| p1 | 11.15 ± 0.37 | 11.72 ± 1.43 ! (1.05x) | 12.56 ± 0.28 (1.13x) | 16.10 ± 0.30 (1.44x) | 451.23 ± 4.43 (40.48x) | 37.58 ± 0.11 (3.37x) | 61.85 ± 7.19 ! (5.55x) |
| p2 | 2.60 ± 0.22 | 2.84 ± 0.038 (1.10x) | 2.10 ± 0.29 ! (0.81x) | 4.12 ± 0.48 ! (1.59x) | 94.97 ± 0.60 (36.58x) | 10.02 ± 0.42 (3.86x) | 12.20 ± 0.27 (4.70x) |
| p3 | 0.42 ± 0.024 | 0.44 ± 0.037 (1.06x) | 0.45 ± 0.024 (1.06x) | 1.78 ± 0.081 (4.25x) | 6.51 ± 0.18 (15.57x) | 1.77 ± 0.17 (4.22x) | 72.24 ± 2.81 (172.82x) |
| p4 | 0.19 ± 0.001 | 0.20 ± 0.009 (1.02x) | 0.20 ± 0.004 (1.02x) | 1.19 ± 0.035 (6.19x) | 1.35 ± 0.066 (7.02x) | 0.51 ± 0.023 (2.66x) | 41.85 ± 0.26 (216.82x) |
| w1 | 0.12 ± 0.006 | 0.12 ± 0.002 (0.98x) | 0.12 ± 0.002 (1.00x) | 1.58 ± 0.006 (12.68x) | 0.80 ± 0.043 (6.38x) | 0.20 ± 0.010 (1.58x) | 4.97 ± 0.16 (39.79x) |
| a2 | 0.14 ± 0.000 | 0.14 ± 0.005 (1.04x) | 0.14 ± 0.006 (1.03x) | 1.82 ± 0.050 (13.20x) | 1.94 ± 0.054 (14.07x) | 5.02 ± 0.13 (36.37x) | 9.34 ± 0.43 (67.65x) |
| a2s | 0.14 ± 0.004 | 0.15 ± 0.005 (1.06x) | 0.15 ± 0.006 (1.06x) | 1.70 ± 0.038 (12.04x) | 9.74 ± 0.10 (69.10x) | 41.44 ± 1.37 (293.89x) | 28.29 ± 1.96 (200.62x) |
| a3 | 0.16 ± 0.004 | 0.16 ± 0.006 (1.00x) | 0.15 ± 0.015 (0.97x) | 3.47 ± 0.17 (22.24x) | 0.81 ± 0.050 (5.21x) | 4.72 ± 0.056 (30.23x) | 23.16 ± 0.52 (148.48x) |
| x2 | 0.15 ± 0.001 | 0.15 ± 0.002 (1.00x) | 0.15 ± 0.002 (1.02x) | 2.51 ± 0.012 (16.49x) | 0.96 ± 0.032 (6.33x) | 2.73 ± 0.14 (17.93x) | 14.79 ± 0.37 (97.29x) |

Reading the numbers:

- sift is ahead of every other engine in every one of the 45 cells of this session. Against pg_search the ratio is 1.4x to 30x (median 10.9x; the closest cells are the unordered scans of the one- and two-letter prefixes, 1.4x and 1.6x,
  where the result is a large share of the corpus and both engines spend their time fetching rows); against rum 1.5x to 415x (median 28x; the closest are the whole-word cells, 1.5-1.8x: 0.31 against 0.17 ms); against pgx-bm25 4.7x to 639x (median 100x)
  and against the pg_textsearch emulation 5.0x to 9,700x (median 15x; it is faster than pg_search in 16 of the 45 cells, mostly filtered, multi-word and whole-word queries). The index is 2.8 times smaller than pg_search's (50.2 against 142.0 MiB) and builds 2.1 times faster (1.4 against 2.9 s); the default index is the
  smallest of the native indexes but `positions = off` (44.4 MiB) is smaller still.
- sift `positions = off` and `pairs` are not needed by prefix queries (none of these reaches a phrase): their latencies are 0.81x-1.30x of sift's cell by cell (median 1.0x), which is the noise of the session, and they cost 0.9x
  (`positions = off`) and 1.7x (`pairs`) of the index.
- pg_textsearch (emulation) is behind pg_search on the top-20 in every class and by orders of magnitude on the short prefixes: top-20 1.0 s at one letter, 404 ms at two, 14.8 ms at three and 2.6-4.5 ms from four letters on (sift 0.16-0.27 ms). The exact BM25
  ranking scores every match, and a one-letter prefix matches a large share of the places. Its native `count(*)` and scan are the prefix tsquery: 455 ms for `count(*)` of a one-letter prefix, 0.8-9.7 ms for words and the multi-word classes.
- rum (native top-20, filtered, count, scan; emulated boost) is within 2-5 times of sift on whole-word and four-letter queries (whole-word top-20 0.31 against 0.17 ms, four letters 1.22 against 0.27 ms) and tens to hundreds of times slower on the multi-word and short-prefix classes:
  108 ms for a one-letter prefix (415 times sift's 0.26 ms), 50 ms for a word and a one-letter prefix. It is faster than pg_search in 12 of the 45 cells (whole words, four-letter prefixes, some filtered queries) but never faster than sift.
  Its index is 94.9 MiB (1.9 times sift's, 0.67 times pg_search's).
- pgx-bm25 is 4.7x-639x behind sift: 34 ms for a one-letter prefix, 72 ms for a three-letter prefix, 43 ms for four letters, 5 ms for a whole word (with the wildcard limit raised, see above); it is faster than pg_search in 1 of the 45 cells.
- The name boost costs pg_search 1.9-3.5 times its plain top-20 (3.00 against 0.85 ms at one letter); sift's boost costs 1.0-2.0 times its top-20 (the one-letter prefix 0.32 against 0.26 ms, the two-letter prefix 0.46 against 0.23 ms).

Cold cache (`bench/bench cold --full --dataset places --serial`, 12 queries per cell, scratch data on tmpfs as above), p50 / maximum in ms; all seven engines in one session:

| workload | class | sift | sift `positions = off` | sift `pairs` | pg_search | pg_textsearch | rum | pgx-bm25 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| top | p1 | 0.51 / 0.61 | 0.56 / 0.77 | 0.60 / 0.86 | 1.94 / 2.28 | 2023.78 / 5502.79 | 95.91 / 339.00 | 38.37 / 116.50 |
| top | p2 | 0.52 / 0.84 | 0.49 / 0.96 | 0.52 / 2.04 | 2.04 / 3.07 | 400.74 / 793.73 | 17.73 / 38.70 | 10.01 / 16.60 |
| top | a2 | 0.19 / 2.12 | 0.18 / 2.32 | 0.19 / 2.38 | 1.98 / 2.59 | 0.73 / 10.53 | 9.21 / 46.07 | 14.55 / 486.47 |
| top | a3 | 0.25 / 0.45 | 0.23 / 0.38 | 0.24 / 0.60 | 2.90 / 3.83 | 0.99 / 1.24 | 12.43 / 17.41 | 32.65 / 3005.16 |
| boost | p1 | 0.72 / 1.11 | 0.70 / 2.35 | 0.74 / 1.13 | 3.65 / 7.20 | 3258.03 / 8008.28 | 97.41 / 256.46 | 45.10 / 151.56 |
| boost | p2 | 0.88 / 1.16 | 0.81 / 1.18 | 1.02 / 1.33 | 4.14 / 5.75 | 638.98 / 1337.88 | 17.63 / 29.74 | 10.29 / 16.21 |
| boost | a2 | 0.27 / 6.38 | 0.39 / 9.24 | 0.34 / 7.59 | 3.38 / 5.18 | 1.27 / 16.04 | 9.60 / 49.45 | 18.23 / 509.45 |
| boost | a3 | 0.31 / 0.81 | 0.46 / 1.19 | 0.35 / 0.87 | 5.19 / 6.04 | 1.51 / 1.86 | 11.63 / 15.26 | 43.03 / 3477.00 |
| filter | p1 | 11.51 / 15.42 | 14.26 / 19.22 | 11.74 / 18.97 | 16.69 / 21.89 | 362.20 / 1041.79 | 101.56 / 256.87 | 60.56 / 167.06 |
| filter | p2 | 6.14 / 11.30 | 6.94 / 19.16 | 7.03 / 12.12 | 9.98 / 14.25 | 66.86 / 106.12 | 17.48 / 36.69 | 13.01 / 20.65 |
| filter | a2 | 0.28 / 2.48 | 0.24 / 2.72 | 0.20 / 2.80 | 6.86 / 7.46 | 0.45 / 1.83 | 9.29 / 31.86 | 19.91 / 1799.08 |
| filter | a3 | 0.24 / 0.47 | 0.26 / 0.40 | 0.25 / 0.54 | 8.17 / 8.84 | 0.56 / 1.46 | 11.70 / 17.02 | 29.83 / 4586.19 |
| count | p1 | 0.16 / 0.38 | 0.15 / 0.19 | 0.15 / 0.20 | 0.93 / 1.40 | 497.87 / 1275.88 | 55.84 / 70.23 | 92.54 / 235.72 |
| count | p2 | 0.09 / 0.13 | 0.09 / 0.13 | 0.10 / 0.14 | 1.47 / 2.27 | 114.47 / 233.62 | 12.87 / 22.57 | 24.82 / 39.38 |
| count | a2 | 0.11 / 0.94 | 0.08 / 0.83 | 0.12 / 1.07 | 2.38 / 3.15 | 1.54 / 9.32 | 4.93 / 19.91 | 18.25 / 570.97 |
| count | a3 | 0.10 / 0.17 | 0.13 / 0.21 | 0.12 / 0.19 | 4.37 / 8.56 | 0.58 / 2.23 | 6.44 / 10.00 | 26.64 / 3244.99 |

These are execution times (`EXPLAIN ANALYZE`), without the planning and the query lookup that the warm p50 includes, so they are not comparable with the tables above. sift is ahead of pg_search in all 16 cold cells
of the table (the closest, the one-letter filtered top-20, 11.5 against 16.7 ms). pgx-bm25's cold maxima reach 3.0-4.6 s in the cells of the 3-letter-plus classes (a3).

### Earlier session: the smaller tiers (not repeated)

These tiers (150,000 and 25,000 articles, 34,165 places) were measured in an earlier session with sift `cd2fb81d18d6` (before word positions were the default, so its columns are what is `positions = off` today), five engines (without pgx-bm25 and the variants), `--serial` on two pinned CPUs of a loaded host, 4-6 rounds. **They were not repeated for this session** and carry the limits of that one; they are kept, with their rows in [results/](results/) (tiers `ab` and `quick`), because they show the ordering at smaller sizes. At 218,058 articles the default index (positions on) is ahead of pg_search in every class (above); at 150,000 articles the index of that build was behind on `count(*)` and phrase and ahead at 25,000, so the margins depend on the size of the corpus.

#### 150,000 articles (`ab` tier)

`bench/bench run --dataset articles --tier ab --workers 2`: 3 s per cell, 6 rounds (load 13-30), all five engines in one session.
Queries per second (minimum-maximum over the rounds); rum's column fills in 14.9 s and adds 300.2 MiB to the table:

| engine | index (MiB) | build (s) | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 33.5 | 2.0 | 3,388 (2,801-3,615) | 789 (694-908) | 759 (720-922) | 971 (834-1,369) | 581 (463-818) | 1,099 (819-1,350) | 268 (222-354) |
| sift `pairs` | 192.2 | 9.1 | 3,165 (2,823-3,579) | 775 (653-877) | 2,882 (2,481-3,796) | 1,642 (1,229-1,899) | 2,978 (2,424-3,750) | 1,306 (1,005-1,577) | 483 (396-685) |
| pg_search | 112.0 | 5.4 | 1,263 (1,108-1,547) | 648 (476-850) | 1,135 (857-1,269) | 841 (636-1,001) | 952 (880-1,017) | 1,100 (803-1,244) | 81 (63-100) |
| pg_textsearch | 105.6 | 10.7 | 8 (7-20) | 1,207 (1,078-1,372) | 6 (4-11) | 11 (7-15) | 23 (13-45) | 53 (16-94) | 36 (7-66) |
| rum | 219.2 | 19.5 | 591 (455-610) | 6 (5-6) | 21 (19-23) | 11 (9-15) | 18 (14-22) | 11 (8-15) | 8 (4-9) |

| engine | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift / pg_search | 2.68 | 1.22 | 0.67 | 1.15 | 0.61 | 1.00 | 3.29 |
| sift `pairs` / pg_search | 2.51 | 1.20 | 2.54 | 1.95 | 3.13 | 1.19 | 5.94 |
| pg_textsearch / pg_search | 0.01 | 1.86 | 0.01 | 0.01 | 0.02 | 0.05 | 0.44 |
| rum / pg_search | 0.47 | 0.01 | 0.02 | 0.01 | 0.02 | 0.01 | 0.09 |

The 150k tier has the full tier's ordering for every class and the wider margins of a smaller corpus: the default index's `count(*)`
(0.61x) and phrase (0.67x) are behind pg_search at both sizes. The heap recheck grows with the corpus, so the crossover depends on
the size (with an earlier build and a 75,000 article tier the phrase and `count(*)` ratios were 1.22x and 1.35x). Judge the
default index's phrase and `count(*)` on the full tier. The size ordering is the same as at 218k: `pairs` is 192.2 MiB against 112.0 MiB
for pg_search (1.7 times) and 33.5 MiB for the default index.

#### 25,000 articles (`quick` tier)

`bench/bench run --tier quick --workers 2` (the default of `bench/bench run`): 1 s per cell, 2 rounds (load 17-35), so the ranges are
wide; it shows the ordering, not the ratios.

| engine | index (MiB) | build (s) | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 6.8 | 0.5 | 4,260 (3,229-5,291) | 1,774 (1,485-2,064) | 1,725 (1,573-1,877) | 2,256 (1,743-2,769) | 2,029 (1,656-2,402) | 3,021 (2,961-3,081) | 868 (664-1,073) |
| sift `pairs` | 40.3 | 2.8 | 3,813 (2,674-4,951) | 1,818 (1,478-2,157) | 3,938 (3,028-4,847) | 3,044 (2,982-3,106) | 4,611 (3,604-5,619) | 2,639 (1,966-3,312) | 350 (328-372) |
| pg_search | 23.6 | 1.4 | 1,206 (968-1,445) | 1,107 (825-1,390) | 1,114 (842-1,387) | 1,328 (1,136-1,519) | 1,341 (1,236-1,445) | 1,137 (847-1,428) | 479 (467-492) |
| pg_textsearch | 22.5 | 4.0 | 47 (10-85) | 3,943 (2,643-5,244) | 56 (42-70) | 93 (88-97) | 146 (144-149) | 163 (131-194) | 106 (100-112) |
| rum | 47.2 | 5.3 | 1,878 (1,379-2,377) | 28 (22-33) | 91 (83-98) | 56 (42-70) | 113 (106-120) | 41 (35-48) | 71 (49-94) |

| engine | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift / pg_search | 3.53 | 1.60 | 1.55 | 1.70 | 1.51 | 2.66 | 1.81 |
| sift `pairs` / pg_search | 3.16 | 1.64 | 3.53 | 2.29 | 3.44 | 2.32 | 0.73 |
| pg_textsearch / pg_search | 0.04 | 3.56 | 0.05 | 0.07 | 0.11 | 0.14 | 0.22 |
| rum / pg_search | 1.56 | 0.02 | 0.08 | 0.04 | 0.08 | 0.04 | 0.15 |

At this size rum's AND (a stored `tsvector` intersection) is 1.56 times pg_search, and every engine's ranking of the other classes is
as at the larger tiers.

#### 34,165 places (`ab` tier, cities15000)

`bench/bench run --dataset places --tier ab --workers 2`: 5 rounds of 2 s per cell (load 19-45), all five engines in one session, p50 latency in ms of the
top-20 workload (the six classes of the quick mode; pg_textsearch is the emulation):

| engine | index (MiB) | build (s) | p1 | p3 | w1 | a2 | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 13.7 | 2.1 | 0.46 | 0.29 | 0.24 | 0.27 | 0.33 | 0.34 |
| sift `pairs` | 24.0 | 2.9 | 0.54 | 0.31 | 0.24 | 0.26 | 0.30 | 0.29 |
| pg_search | 27.8 | 11.5 | 0.97 | 1.72 | 1.79 | 1.80 | 3.45 | 2.04 |
| pg_textsearch | 21.5 | 2.9 | 1,135 | 26.84 | 9.26 | 6.94 | 7.19 | 7.51 |
| rum | 30.1 | 1.6 | 63.52 | 6.12 | 0.40 | 14.21 | 10.59 | 9.62 |

sift is ahead of pg_search in all 24 cells of the tier (1.9x-22x). The pg_textsearch emulation costs
1.1 s at one letter at this size (8.8 s at 236k places): its cost follows the matches. The `quick` tier (the same 34,165 places, 2 rounds of 1 s, load 21-26; sift is ahead in 24 of 24 cells
there too, 1.1x-21x) has the same ordering:

| engine | index (MiB) | build (s) | p1 | p3 | w1 | a2 | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 13.7 | 3.3 | 2.41 | 1.36 | 0.34 | 0.78 | 1.02 | 1.23 |
| sift `pairs` | 24.0 | 6.8 | 2.23 | 1.58 | 0.34 | 1.15 | 1.46 | 0.23 |
| pg_search | 27.8 | 17.8 | 2.70 | 3.80 | 4.26 | 5.41 | 12.94 | 7.75 |
| pg_textsearch | 21.5 | 5.0 | 4,594 | 94.49 | 24.53 | 20.96 | 18.12 | 11.34 |
| rum | 30.1 | 8.1 | 94.50 | 27.40 | 2.30 | 29.78 | 37.88 | 10.77 |

All cells of these tiers are in `results/places.tsv`.


## Regression check against v1.1.0

`bench/bench regress --run --tier full --ref v1.1.0 --engines sift,sift_nopos,sift_ref` with the matrix widened to every class and workload (articles: `and or phrase mixed count filter join`; places: all
nine classes with `top boost filter count scan`), full scale, sift, sift `positions = off` and the reference `sift_ref` (tag v1.1.0, commit `0a7a59c51c6b`, built under the extension name `pg_sift_ref`) in three lanes at the same time
(2 server CPUs and 1 load generator CPU each, the CPUs rotated every round), 3-5 rounds. It compares 112 cells (56 per engine: 7 articles throughputs, 45 places p50 latencies, `CREATE INDEX` time, index size); a cell is a
REGRESSION when the new value is worse by more than 5% and by more than 2 combined standard errors (single measurements such as builds and cold-cache cells: 15% relative error assumed).
The cold-cache p50 of the 46 cells of both variants was compared in a second run (`bench/bench cold --tier full --engines sift,sift_nopos,sift_ref`).

Three cells are flagged, the same three in every run (the first, a repeat capped at 3 rounds, and a repeat with 5 rounds that was started before the limit of 3 was set): all three are index sizes, which are deterministic.

| dataset | engine | metric | new | v1.1.0 | new / ref | verdict |
|---|---|---|---:|---:|---:|---|
| articles | sift (positions on) | index MiB | 130.9 | 47.9 | 2.73x | expected: word positions are stored by default |
| places | sift (positions on) | index MiB | 50.2 | 42.6 | 1.18x | expected: word positions are stored by default |
| articles | sift `positions = off` | index MiB | 52.5 | 47.9 | 1.10x | **real**, deliberate: bitmap groups from 8K postings |

The third is the only one that is not the documented positions trade-off. Cause, found by building the reference at the commit before and at the commit that changed the threshold (`--ref b642eb3~1` gives 47.9 MiB,
`--ref b642eb3` gives 52.5 MiB, equal to the index of today's `positions = off` to the digit): commit `b642eb3` "index: bitmap postings from 8K documents instead of 64K" makes terms of 8,192 to 65,535 postings bitmap groups with block bounds
instead of Elias-Fano lists, which the commit message prices at +9.6% index (50.2 to 55.1 MB at 218k articles) for ranked AND 0.85 to 0.66 ms and ranked OR 3.78 to 3.07 ms. The measurement agrees: against v1.1.0 `positions = off` is
**1.6x faster on AND, 1.9x on OR, 1.5x on mixed, 1.8x on the filtered top-10**, and level on `count(*)` (0.99x) and phrase (1.13x). It is therefore not a defect of the build but a size/speed trade-off that has no option: the 8K threshold is
compiled in (16K would be 52.2 MB for 0.79 ms in the commit message's own numbers). If a 10% smaller `positions = off` index matters more than 1.6x on ranked AND, the proposed fix is to make the threshold a reloption; no change was made on this branch.

Nothing else is flagged: no throughput, latency, cold-cache p50 or build-time cell of either variant is worse than v1.1.0 beyond its noise (52 cells improved, 57 within the thresholds in the repeat; in the cold run 9 improved and 37 are within the thresholds).
The improvements are the ones the changelog lists: against v1.1.0, with positions on, `count(*)` 10.6x, phrase 5.6x, join 2.9x, mixed 2.6x, OR 1.9x, filter 1.9x, AND 1.6x; places one- and two-letter prefix top-20 and boost 1.7-3.5x lower latency
(cold p1 `top` 1.17 to 0.47 ms, `boost` 1.89 to 0.65 ms).

Cells that are above the reference by more than 5% but inside the noise at 3 rounds, so **undecided** (not repeated, by the cap of 3 rounds): `CREATE INDEX` of sift on the articles (4.3 against 3.3 s, 1.30x, single measurement; expected with positions on) and on the places (2.5 against 2.3 s),
and seven places latencies at 0.17-0.49 ms, whose combined error is 7-16% of a value that is itself below the resolution of a single-client p50 on this host: sift p4 `boost` 1.07x, w1 `filter` 1.08x; `positions = off` a2 `top` 1.07x, p2 `scan` 1.08x, p4 `filter` 1.07x, w1 `count` 1.08x, w1 `top`
1.19x (0.179 against 0.150 ms). None of them is consistent across the runs (the first run measured `positions = off` w1 `top` at 1.06x).


## Correctness

`bench/bench check oracle|rank ENGINE` checks the articles engines against a tsvector oracle (match sets, `count(*)`) and pgx-bm25's BM25
(soft top-10 overlap); `bench/bench check --dataset places [oracle|rank] [N]` checks the places engines against an exact oracle in SQL
(`bench/sql/places_oracle.sql`), N queries per class (30: 270 queries):

- match set: the places returned for a typed query must be those with every typed word as a prefix of a word of the all-fields text, with
  the engine's own idea of a word (pg_search's tokenizer, sift's accent folding, the PostgreSQL parser for rum and pg_textsearch's native
  index, alphanumeric runs for the n-gram text of the pg_textsearch emulation);
- top-20, boost, filtered top-20: BM25 over the edge n-grams (k1 = 1.2, b = 0.75, the typed words' distinct n-grams, boost = all-fields
  score + 3 x name score, ties at rank 20 accepted): the share of the returned places that the oracle accepts as a top-20, averaged over the queries
  (`agree`; `exact` counts the queries at 1.0), and the returned places that are not matches (`wrong`).

### Articles (25,000-article quick tier, 200 substrings, this session)

- `bench/bench check oracle sift 200`, the same for `sift_nopos` and `sift_pairs` (and, or, phrase, not modes): **the same two mismatches for the three, no others**, which are the known baseline:
  query 167 (`or`: `h pugmire wilum`, the oracle has 481 matches, sift 480, document 21851 missing) and query 255 (`phrase`: `within the`, 367 against 371, four extra documents 1498, 2627, 2793, 17583).
  I did not investigate these two again.
- `bench/bench check rank sift 200` (top-10 overlap with pgx-bm25's BM25, a soft check): and 0.997, or 0.979, phrase 0.999 (the phrase top-10 differs in size for one query), identical to the baseline.
  Earlier session, other engines (match sets and `count(*)` of pg_textsearch and rum equal the tsvector oracle in all four modes, 0 mismatches):

| engine | and | or | phrase |
|---|---:|---:|---:|
| sift | 0.997 | 0.979 | 0.999 |
| pg_textsearch | 0.993 | 0.967 | 0.998 |
| rum | 0.916 | 0.180 | 0.964 |

pg_textsearch's AND overlap was 0.88 before its score was written `(... <@> ...) * 1` (the unranked top-10 described above).

### Places, `bench/bench check --dataset places 30` (`agree` = Jaccard index of the match sets for `match`, share of accepted places for the ranked workloads; `exact` of 270 queries at 1.0)

| engine | match exact / agree | top-20 exact / agree | boost exact / agree | filtered top-20 exact / agree |
|---|---|---|---|---|
| sift, 34,165 places (this session) | 228 / 0.985 | 154 / 0.826 | 152 / 0.807 | 194 / 0.929 |
| sift, 236,262 places (this session) | 211 / 0.992 | 141 / 0.750 | 141 / 0.699 | 173 / 0.861 |
| pg_search, 34,165 places (earlier session) | 268 / 1.000 | 202 / 0.982 | 192 / 0.972 | 231 / 0.992 |
| pg_textsearch emulation (earlier session) | 270 / 1.000 | 201 / 0.982 | 213 / 0.984 | 232 / 0.992 |
| rum (earlier session) | 270 / 1.000 | 154 / 0.759 | 154 / 0.720 | 193 / 0.876 |

The sift row at 34,165 places is identical to the earlier session's (the wide-prefix fix changed nothing here, as the diagnosis below shows: the differences never came from prefix expansion). Per class at 34,165 places the match agreement is 1.000 everywhere except `a3`
0.867 (310 returned places in all that the oracle does not return), `p2` 0.999 and `p3` 0.998; at 236,262 places `a3` 0.933 (380), `p2` 0.999, the rest 1.000.

**The remaining match differences are not bugs; they are differences between sift's analyzer and the oracle** (diagnosed on the 34,165-place run, all 270 queries, by comparing sift's match set with the oracle's for every query):

1. **`a3` (0.867, 310 places): the oracle does not fold the typed words.** Four of the 30 `a3` queries have an accented typed word (`haessleholm skåne sweden`, `san méxico mexico`, ...). sift folds both the documents and the query (`skåne` finds `skane`, as `résumé` finds `resume`); the oracle folds
   the documents with `unaccent` and compares the typed word as typed, so it returns 0 places where sift returns 1 and 163. With the typed words unaccented the oracle agrees with sift on all 30 `a3` queries and on every other `a2`, `a2s`, `w1`, `x2` and `p4` query (agree 1.000).
2. **After folding the typed words, 38 of the 270 queries (the 1-3 letter prefixes `p1`-`p3`) still differ by 116 (query, place) pairs**: 114 places the oracle returns and sift does not, 2 the other way. All of them are word-segmentation or folding differences:
   - 87 pairs: the place has a modifier letter inside a word (U+02BB and U+02BC, the okina and apostrophe letters of transliterations such as `Tʻakanrok`). sift keeps it as a letter of the word (`tʻakanrok`), the oracle splits at it (`t`, `akanrok`) and finds a word starting with `a`.
   - 21 pairs: a zero-width non-joiner or joiner (U+200C, U+200D) inside a word (`jank‌pwr` in Persian and Indic names). sift joins the parts into one token (`jankpwr`), as the Unicode word-break rules do; the oracle splits there and finds the word `pwr`.
   - 6 pairs: IPA letters in transcriptions (`[ɕrudˈmjɛɕtɕɛ]`, `[auˈɡustuf]`) that PostgreSQL's `unaccent` maps to Latin letters (`ɕ` to `c`, `ɡ` to `g`) and sift's accent folding does not.
   - 2 pairs (the places sift returns and the oracle does not): `老橋鎮CDP`, a CJK run followed by Latin letters, which sift breaks into a word `cdp` and the oracle keeps as one alphanumeric run.
   Each of these is a choice of the analyzer (sift follows the Unicode word boundaries and folds with its own table); none is a wrong result of the prefix expansion, and none is in a class with more than the three shortest prefixes. I did not repeat the diagnosis at 236,262 places.

The ranked workloads differ for another reason: sift does not rank by n-grams. A typed prefix is expanded to the indexed words and scored as them (the max over the terms), so its top-20 is another ranking than the n-gram BM25 oracle's, most visibly on the 1-2 letter prefixes whose matches are a large share of the corpus
(34,165 places: `p1` 0.458, `p2` 0.628 for the top-20; at 236,262 places 0.392 and 0.432, boost 0.198 and 0.275); from four letters on it agrees at 0.72-0.89. **This is the one place where pg_search and the pg_textsearch emulation, which rank BM25 over the same n-grams as the oracle (0.97-0.99), agree better than sift.**
rum's `<=>` is a proximity ranking, not BM25: 0.23 at one letter, 0.93 on whole words (earlier session); its match sets are exact. The oracle itself is sound: pg_search agrees at 0.97-0.99 and the pg_textsearch emulation, which is the same computation in an extension, at 0.98 with all match sets exact.


## Method of the harness and its run time

`bench/bench run` is one pipeline for every engine and both datasets. The tables of this document (a `--serial` run on the default budget of 8 server and 4 load generator CPUs) and a default run differ in how the cells are scheduled, not in
what is measured:

- **Lanes.** By default every engine has a cluster of its own (a lane), pinned to an equal share of the server CPUs with its load generators on an equal share of the load CPUs
  (default budget: the 8 least busy hardware threads for the servers and 4 for the load generators, disjoint, whole cores; `--cpus`, `--load-cpus`). All engines are measured at the same time, one class at a
  time, so a drift of the host hits them alike, and in round r the engine k takes the CPUs of the lane (k + r) mod n, so a core that the host loads does not stay with one engine (without
  the rotation a comparison of sift with a build of nearly the same code showed differences of up to 14% in single cells that their standard errors did not explain). `--serial` is the earlier method:
  one cluster, the engines one after the other, the order rotated every round.
- **Cache.** The built lanes (the database with its index, in a stopped data directory) are cached by a hash of the adapter, the extension binary, the corpus, the harness, PostgreSQL and the CPU count of the lane.

The published tables of this document were measured with the previous cell method (a fixed short time per cell, 2-3 rounds, the standard error from the spread of the rounds); the next two bullets describe the current method of the harness.

- **A cell and when it stops.** A cell is one class (articles: 8 clients, queries per second) or one workload on one class (places: one client, p50 and p95 of the transaction latency, which includes the lookup of the
  query text) on one engine in one round. It is not measured for a fixed time: `steps/cell.py` runs pgbench in chunks that log every transaction and stops once the cell has at least `--min-time` (0.25 s) and
  `--min-samples` transactions (3 for places, 1 per client for articles) **and** its relative standard error is at most `--rse` (5%), or after `--max-time` (quick 1 s; full: articles 2 s, places 1 s) whichever comes first.
  A fast cell therefore takes the minimum, a slow one the maximum. A chunk is a count of transactions per client (`pgbench -t`, the duration option takes whole seconds only) when it is short and a client runs
  many transactions in it, whole seconds (`-T`) when it is long or slow; the first transaction of every client of a short chunk is dropped (a backend that has just started pays for its caches in its first query), a cell has
  no other warm-up (the relations are prewarmed after the build), and the statements are seeded alike for every engine (chunk k uses `--random-seed=7+k`). When fewer than `1 / rse^2` = 400 queries fit in the maximum
  (the batch means below need that many for 5% on a trace of unit coefficient of variation) the cell is measured for the whole maximum in one chunk: no precision is within reach, so the time is not split into chunks that
  each end with idle clients waiting for the slowest statement. What remains of a slow cell is the statements still running when the time is up (several seconds for the engines with seconds per query).
- **Standard error.** Within a round: queries per second by batch means (the transactions of each client are cut into consecutive batches of at least 5; the rate of a batch is the clients' transactions over their
  pooled seconds, so a few slow statements weigh what they last; the error is the standard deviation of the batch rates over the square root of their number, with at least 4 batches, or, with fewer, the standard error
  of the mean latency, sd / (mean sqrt(n)), whichever is larger); the p50 by order statistics, distribution free: half the distance between the n/2 - sqrt(n)/2 and n/2 + sqrt(n)/2 ranked latencies (the 68% interval
  of the median). The standard error of a cell is the larger of the sample standard deviation of its rounds over `sqrt(n)` and the error the rounds' own errors leave, `sqrt(sum se^2) / n`; the value is the median
  of the rounds. The within-round error matters: the rounds replay the same seeded statements, so the spread of the rounds does not show which statements of a heavy-tailed trace a cell happened to run, and the earlier
  standard errors (the spread alone) were too small for such cells (and for the slow engines, which run few statements) by a large factor. Tables show `median ± stderr (min-max)`, charts one standard error as an error bar,
  and `!` flags a relative standard error above 10%.
- **Rounds.** 2-3 (3 is the most, more runs with a warning): after the second round only the cells with a relative standard error above 5% are measured again, except the cells whose own error per round is so large that three
  rounds cannot bring them to 5% (a cell limited by time: a repetition costs as much as the first measurement and moves the error by a factor of 0.8 at best). `bench regress` runs 3 rounds.
- **Cold cache.** Per engine and class up to 12 queries, each after the heap, TOAST and index files of the table were evicted (per relation, `pg_buffercache_evict_relation`, and `posix_fadvise` unless the data directory
  is on tmpfs, where there is no cache other than the shared buffers), the lanes in parallel. At least 5 queries run, then only while the cell has used less than 3 s: a query of seconds (the pg_textsearch emulation of the
  one-letter ranked prefixes) is not run twelve times; `cold.tsv` has the count.
- **pgx-bm25** (`bm25`) runs in every table of a run. Articles: `and`, `or`, `phrase`, `mixed`, `count`, `filter` are native (jsonb queries through `@@@` / `&@@`); `join` works at the full tier (18 queries per second) and is an `error` cell at the 25k quick tier, where
  the extension refuses the plan of the jsonb `@@@` filter above a join (I did not investigate why the planner chooses differently); `fuzzy` is unsupported (NA). Places: `top`, `filter`, `count`, `scan` and the name boost (`bm25_boost`) are native for words of 3 letters or more
  (`bm25_wildcard`; the extension refuses a wildcard under 3 letters, and refuses one that expands to more than `bm25_native.wildcard_max_expansions` = 1000 terms, which `san*` does at 236,262 places: the adapter raises it to 100000, a fix made for this session); 1-2 letter words are emulated in the same
  index with stored edge n-gram columns, reported as part of its index.
- **Visibility map.** Every places database is a copy of a vacuumed template (`VACUUM (FREEZE, ANALYZE)` once, before the copies). Before this session only the per-engine `VACUUM` ran, and in a freshly loaded cluster its first run left 13-56% of the pages
  not all-visible, so `count(*)` of the first engine read the heap (sift measured 0.22-0.52 ms for a one-letter prefix next to 0.12 ms for the same index in the next database, cold 10 ms against 0.13 ms). The articles database is copied from a vacuumed template and was not affected (21,456 of 21,456 pages all-visible in every engine's database).


### Run time

Wall time of the quick tier on the same host, in the same state (load average 3-12 from other jobs), measured by the old pipeline (one cluster under a quota of 8 CPUs, five engines, cells one engine after the other, 1 s cells) and by the new one
(default budget of 8 server and 4 load generator CPUs, concurrent lanes; six engines, pgx-bm25 added):

| phase, quick tier | before (5 engines) | now (6 engines) |
|---|---|---|
| articles: corpus restore and index builds | 10 s | 4 s with some lanes cached, 6 s from scratch (5 engines) |
| articles: 7 classes, 2 rounds | 170 s | 44 s |
| places: setup and index builds | 14 s | 11 s (1 s cached) |
| places: 24 cells per round, 2 rounds | 246 s | 56 s |
| both datasets | 440 s | 117 s |
| cold-cache tables, both datasets | not measured at the quick tier | 53 s |
| `regress --run` (sift against sift_ref, both datasets, 3 rounds) | not available | 60-70 s of measurements |
| ab tier, articles build phase (150k articles, six engines, one CPU each) | one after the other: the sum of the builds, about 86 s of CREATE INDEX and column fill, plus the restores | 33 s (restores and builds at the same time; the slowest engine, rum with its column, takes 28 s; 1 s once cached) |

What was measured to get there, and what was not taken:

| idea | result |
|---|---|
| engines concurrently, one lane each, with `--serial` as the fallback | **taken.** Same cells, 3 engines (sift, pg_search, pg_textsearch), 1 server CPU each: 21-27 s concurrent against 45-51 s serial, and the builds 3 times faster. Cost: the absolute numbers differ (below). |
| cache of built lanes | **taken.** The ab articles builds took 33 s and 1 s the second time; the lane directories are large (12 GB for the experiments of this change), `bench clean --all` removes them. |
| shorter cells and 2-3 rounds with stopping at a relative standard error of 5% | **taken** (it is the default of the tiers; quick has 2 rounds of 1 s). |
| prewarm after the build | **taken** (the first cell of a group no longer pays the reads; the warm-up cell stays). |
| per-relation cold eviction in all lanes at once | **taken.** 53 s for both datasets and six engines; the number of cold queries per cell stays 12 (cutting it would widen the maximum and p50 of the cell for no gain in time: the cost is the heavy engines' query, not the count). |
| fixed-seed sample of the trace per class (`--sample N`) | **rejected as the default, kept as an option.** The cells are limited by time, not by the number of statements, so a sample saves no time. It also changes the numbers: with 300 of the 1000 statements of a class, sift `count` ran at 1657 qps against 1020 on the whole trace and pg_textsearch `phrase` at 52 against 14 (heavy statements decide those means), well outside the standard errors. |
| articles and places sessions at once (`--parallel`) | **rejected.** They would split the CPU budget between two sessions; with the lanes of one session already using it, there is nothing to gain and every cell would move. |
| cold-cache session at the same time as the warm ones | **rejected.** The evictions would perturb the warm cells; it runs after them. |

Validation of the concurrent method (3 engines, classes `and`, `phrase`, `count`, quick tier, one server CPU and one load CPU per lane, medians of 2 rounds, queries per second; the whole trace in both):

| engine | class | concurrent lanes | `--serial` one cluster | ratio |
|---|---|---:|---:|---:|
| sift | and | 2626 ± 268 | 3192 ± 1.5 | 0.82 |
| sift | phrase | 930 ± 3.7 | 965 ± 12 | 0.96 |
| sift | count | 1020 ± 29 | 1018 ± 34 | 1.00 |
| pg_search | and | 873 ± 94 | 1102 ± 34 | 0.79 |
| pg_search | phrase | 886 ± 17 | 1036 ± 4.7 | 0.86 |
| pg_search | count | 843 ± 30 | 1075 ± 21 | 0.78 |
| pg_textsearch | and / phrase / count | 25 / 14 / 123 | 26 / 14 / 132 | 0.96 / 1.0 / 0.93 |

Running the engines at the same time lowers the absolute numbers of the fast engines by up to 20% (probably the memory bandwidth, the caches and the clock of the package that the lanes share; I did not isolate it), pg_search more than sift, so a ratio can move by as
much as 25% (sift `count` over pg_search: 1.21 concurrent, 0.95 serial; `and`: 3.0 and 2.9; `phrase`: 1.05 and 0.93). For that reason **the tables of this document were measured with `--serial`**; compare engines within one run and
use `--serial` (with `--cpus` set to the share of one lane to compare like with like) for numbers that are quoted. The regression check below is the one use of the concurrent mode at full scale.

The regression check (`bench/bench regress`) puts sift and sift_ref in the same budget and rotates the CPUs, which is what makes it usable at 3 rounds: the control run of the same commit as reference (`--ref` of the commit
itself) gave 0 regressions in 18 cells, the run with `sift.simd = 'scalar'` applied to the new sift only (a deliberate slowdown) reported five regressions (0.68x-0.94x of the reference) and exit status 1 (the deliberate slowdown was applied through a temporary hook in the adapter that is not committed).
The full-scale check against v1.1.0 is its own section above; before it, four quick runs flagged 1, 0, 1 and 2 cells, not the same ones, which is the noise level of a quick run: a flagged cell of a quick run is a reason to repeat, not a verdict.

### Measured run time of the published session

`bench/bench all --full --serial` (host load as above), 3069 s in all: articles (the `articles-full` phases: 1 s dump, 79 s corpus restore and the seven index builds one after the other, 842 s of cells, 923 s together), cold-cache tables
437 s (articles 183 s, places 254 s), places 2630 s of cells in that first session (it contained the 24 failing pgx-bm25 cells). The places tiers published here are from a second session after the two harness fixes:
1639 s of cells (27 min; restore and the builds of the seven engines and two emulations 55 s) and 260 s of cold-cache cells, plus three rounds of the one cell that a stray `pkill` of another job damaged. The published tiers together took
about 51 minutes of wall time (articles 923 s and cold 183 s, places 1,695 s and cold 260 s), against about 2 hours for the earlier published session on the 2-CPU budget. The concurrent default (`bench/bench all --full` without `--serial`) was not run at full scale (it was run at the quick tier, 124 s, and the regression check at the full tier, 355 s of cells for the 112 cells of the check).

#### `--serial`: one engine measured at a time

`--serial` is the method of the published numbers (one cluster, the engines interleaved cell by cell), so its cells cannot overlap; the time was cut by measuring each cell only as long as it needs and by not building again. Measured on this host at
load average 8-14 from other sessions, on 2 server and 2 load generator CPUs, the same engines (six, with pgx-bm25), the same classes, variants and rounds. The reference is the earlier run of the old method, `-T 3` per articles cell and `-T 2` per places cell, 1 warm-up round,
3 rounds, load average 12-23:

| phase | before | now | what changed |
|---|---:|---:|---|
| articles full, cells | 842 s | 518 s | cells stop at 5% or at the cap, no warm-up round for the Postgres engines, the third round only for the cells that can still gain (10 s instead of 240 s) |
| places full, cells | 1644 s | 574 s | 819 cells of a fixed 2 s: 518 of the cell runs now take under 0.5 s (149 s in all), 186 run the 1-2 s of the cap (216 s) |
| both, cells | 2486 s (41 min) | 1092 s (18 min) | 2.3 times faster; the target of 15 min is not met on this host (below) |
| cold-cache tables | 76 s + 185 s | about 43 s + 89 s (projected from the quick tier, measured 14 s and 40 s against 25 s and 84 s) | one process per cold query, 5 to 12 queries and 3 s per cell |
| builds for the cold session | 105 s + 68 s | a few seconds | the cluster of the serial session is cached whole and restored |
| quick tier, articles / places cells | 171 s / 348 s | 125 s / 210 s | |

More than half of what is left of the articles time is the drain of pg_textsearch (the run used a 1 s cap; the default 2 s adds about a second to each slow cell): its six classes spend 298 of the 513 s of the cells, because the trace has statements of 8 to 12 s (`and`, `phrase`, `join`, `mixed`) that a `-T` window of 1-2 s lets finish.
That wall time cannot be cut without changing what is measured (a statement timeout or a cancel censors exactly the statements that decide the mean; pgbench ignores SIGINT until its transaction ends, and its log is only written at exit).
The cells of those classes have a relative standard error of 30-50% in a round whatever the time, which the tables show with `!`.

Validation, interleaved in one session so the host noise is common (quick tier, 3 rounds, the old `-T 1` cell and the new cell alternating): places 168 cells, 156 of them within 2 combined standard errors of the old value and 164 within 3 (largest 5.9, median 0.59,
mean ratio new/old 1.02, median relative difference 7%); articles 48 cells, 37 within 2 and 42 within 3 (largest 5.3, median 1.0). The large differences are all heavy-tailed cells (pg_textsearch, rum `filter` and `mixed`, sift_nopos `join` and `count`) where the old `-T 1`
value includes the drain of the statements in flight and is biased low (mean ratio 1.17), and its standard error, from fixed-seed rounds, ignores the sampling error of the statements. Against 3 seeds of 8 s per cell (the best estimate of the
truth), the new values of pg_textsearch `and` / `phrase` / `count` / `filter` were 77 / 47 / 176 / 252 qps (standard errors 13-47) against 39 / 32 / 114 / 115 and the old ones 36 / 22 / 164 / 160: inside two standard errors for the new, outside for the old; the fast engines
(sift, sift_nopos, pg_search) agree within a few percent with all three.

| idea | result |
|---|---|
| each cell until its relative standard error is 5% (batch means of the queries per second, order statistics of the p50), at most the cap | **taken.** |
| the first query of every client dropped from a cell with many queries per client | **taken.** A backend that has just started pays for its caches in it; the warm-up round is gone. |
| a cell that one more round cannot bring below 5% is not repeated | **taken** (the third round of 56% of the places cells was a repeat). |
| whole cluster of the serial session in the cache | **taken.** The cold session and a rerun do not build again. |
| stopping pgbench early (SIGINT, SIGTERM, `pg_cancel_backend`) | **rejected.** SIGINT is ignored until the transaction ends, a kill loses the log, a cancel drops the longest statements and so biases the mean. |
| `-T` / `-P` with fractions of a second | **rejected.** pgbench takes whole seconds; short chunks use `-t`. |
| a load generator of my own (Python or psql) | **rejected.** It would distort the throughput of the fast engines. |
| cold cache in the session of the warm cells | **rejected** (the evictions would perturb the cells), the cache of the cluster made it unnecessary. |

`bench/bench all --full --serial` writes `docs/results/*.tsv` and redraws `docs/img` in one command; the prose of this file is not generated. Results are kept as small tables per run (`cells.tsv`, `builds.tsv`, `summary.tsv`,
`machine.txt` with the CPU, engine versions and load) in the result directory, and the scratch cluster is removed when the run ends.
