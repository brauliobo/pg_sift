# Benchmarks

sift (and sift with `phrase_index = pairs`) against ParadeDB pg_search 0.26.1, Timescale pg_textsearch 1.5.1 and rum 1.3.15 on two public
datasets, measured with `bench/bench run` (articles through `bench/steps/ab.sh`, places through `bench/steps/places.sh`; method,
datasets, licenses and checksums: `bench/README.md`). Every engine indexes the same rows with the same analysis (lowercase, no
stemming, no stopwords; pg_textsearch and rum through the PostgreSQL `simple` text search configuration) and answers the same
queries from the same cache state; cells are run one at a time with the engine order rotated every round, and the tables report
medians over the rounds. The engines, in the order of every table and chart, are listed in `bench/engines.tsv` (it also has
`sift_nopos`, sift built with `positions = off`, which is skipped until the installed sift has that reloption; it is not in any table
below). The numbers of every table below are also in
[results/articles.tsv](results/articles.tsv) and [results/places.tsv](results/places.tsv), one row per measurement (columns:
`bench/README.md`), for charts.

Host: AMD Ryzen 7 9700X (8 cores, 16 hardware threads), 188 GB RAM, Linux 7.2, PostgreSQL 18.6, scratch data on tmpfs, shared with
other jobs. **Every table in this document was measured with the engine servers and the load generators pinned to two cores**
(`taskset -c 6,7`, `nice`; the server cgroup scope at `CPUQuota=200%`, `--workers 2`: pgbench `-j 2` and at most 2 parallel
workers, the 8 clients of the articles cells unchanged), because the host was in use. sift was built from commit `cd2fb81d18d6`,
pg_search from the ParadeDB 0.26.1 package, pg_textsearch 1.5.1 from the v1.5.1 tag (commit `fa142d049d08`, tarball sha256 pinned in
`bench/versions.env`) with PGXS, rum 1.3.15 (tag 1.3.15, tarball sha256 pinned) with PGXS. One session per tier measured all five
engines together, the sessions one after another: places full (39 min, load average 9-36), articles full (36 min including the
builds, load 15-43), the 150k and 34k tiers (26 and 22 min, load 13-30 and 19-45), the cold-cache runs (4 min for articles, 13 for
places) and the quick tier (8 min; its places part was run again in 7 min). The places sessions were run twice: after the first set
the harness put sift in a database of its own (it had shared the database of pg_search's index, which cost it 0.1 ms per query), and the
full tier a second time because one pg_textsearch cell, the one-letter name boost, took longer than the 30 s statement timeout in a
round (the harness allows 120 s now). Only the second set is published. The load averages are those of the whole host (16 hardware threads), so absolute numbers carry noise and the per-round ranges in
the tables are wide; no articles round reached the 15% spread that ends a run early. Compare engines within a table, not across
tables, tiers or hosts: **ratios depend on the CPU the engines get**. The earlier full-tier session (sift `dd3692043b5b`, the
server budget of 8 vCPUs with `CPUQuota=800%`, four pgbench threads, load 4-31; git history of this file) measured sift at 2.49 (and),
2.33 (or), 0.65 (phrase), 1.78 (mixed), 0.70 (count), 0.93 (filter) and 4.31 (join) times pg_search, and `pairs` at 2.76, 2.44, 2.83, 2.69, 3.23,
1.20 and 7.49; on two cores every sift ratio is lower (below) while `pairs` keeps its order of classes. Index sizes and build times of every table are for one build per engine.

## What is native, what is emulated, what is NA

Every cell of every table and chart has a number: **no NA cell is left**, because every workload can be expressed on every engine,
natively or by an emulation that is documented here. Two engines have no native query for some places workloads (a typed prefix
ranked by BM25; a field boost), and the emulation of each is checked against an exact SQL oracle (Correctness, below). Coverage before
and after:

| table | before | now |
|---|---|---|
| articles, full tier | 5 engines x 7 classes | same cells, new session; the pg_textsearch AND, phrase, mixed, filter and join top-10 are now ranked (below) |
| articles, 150k tier | sift, sift pairs, pg_search | all five engines (14 new cells) |
| articles, 25k tier (quick) | not published | all five engines (35 cells) |
| articles, cold cache | 3 engines x 3 classes | 5 engines x 7 classes (26 new cells) |
| places, full tier | pg_textsearch NA in 27 ranked cells, rum NA in 9 boost cells, sift pairs not run (45 cells) | all 225 cells: 27 pg_textsearch and 9 rum cells by emulation, 45 sift pairs cells new |
| places, 34k tier | sift, pg_search (6 classes x 4 workloads) | all five engines (72 new cells: 18 pg_textsearch and 6 rum emulated) |
| places, 34k tier (quick) | not published | all five engines (120 cells) |
| places, cold cache | sift, pg_search | all five engines (48 new cells) |
| places correctness | none | match sets and top-20 of every engine against an SQL oracle |
| charts | 11 NA marks (index size, build time: sift pairs on places; places latency: pg_textsearch) | none |

| engine | places workload | how it runs |
|---|---|---|
| sift, sift `pairs`, pg_search | all | native (pg_search: `edge_ngram(1, 15)` fields, a typed prefix is a term) |
| pg_textsearch | count, unordered scan | native: the prefix tsquery `'gra':* & 'ho':*` through its `bm25` index (a tsquery `@@` filter) |
| pg_textsearch | top-20, boost, filtered top-20 | **emulation**: `bm25` indexes on stored columns of the edge n-grams of the words |
| rum | top-20, filtered, count, unordered scan | native: prefix tsquery, `<=>` ranking |
| rum | boost | **emulation**: a second tsvector with weights |

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

8 clients, 4 interleaved rounds of 6 s per cell (2 s warm-up), queries are 2-15 consecutive words of random articles (1000 each as
AND, OR and phrase); `bench/bench run --full --dataset articles --rounds 4-4 --duration 6 --workers 2`. Throughput in queries per
second, median (minimum-maximum over the rounds), with the size of the index and the time of its `CREATE INDEX` (rum also needs a stored
`tsvector` column: 28.2 s to fill it, 437.7 MiB of table growth, neither counted in the index column):

| engine | index (MiB) | build (s) | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 47.9 | 3.1 | 2,046 (1,814-2,415) | 550 (362-642) | 493 (399-563) | 598 (510-621) | 277 (254-355) | 650 (546-709) | 192 (176-281) |
| sift `pairs` | 267.2 | 15.0 | 2,076 (1,921-2,532) | 446 (388-559) | 2,085 (1,488-3,270) | 791 (747-1,262) | 1,451 (842-1,756) | 941 (727-1,013) | 387 (330-443) |
| pg_search | 161.0 | 9.9 | 1,222 (1,129-1,359) | 581 (477-645) | 1,044 (649-1,334) | 673 (487-706) | 697 (540-742) | 974 (791-1,056) | 69 (58-78) |
| pg_textsearch | 147.3 | 19.5 | 4 (2-7) | 777 (619-932) | 3 (2-4) | 8 (4-24) | 14 (12-18) | 16 (11-29) | 13 (9-38) |
| rum | 309.9 | 32.3 | 328 (288-439) | 3 (3-5) | 11 (10-11) | 6 (5-7) | 12 (10-14) | 8 (5-8) | 5 (4-8) |

Ratio of queries per second to pg_search (the baseline of every ratio in this document):

| engine | and | or | phrase | mixed | count | filter | join |
|---|---:|---:|---:|---:|---:|---:|---:|
| sift / pg_search | 1.67 | 0.95 | 0.47 | 0.89 | 0.40 | 0.67 | 2.77 |
| sift `pairs` / pg_search | 1.70 | 0.77 | 2.00 | 1.18 | 2.08 | 0.97 | 5.58 |
| pg_textsearch / pg_search | 0.00 | 1.34 | 0.00 | 0.01 | 0.02 | 0.02 | 0.19 |
| rum / pg_search | 0.27 | 0.01 | 0.01 | 0.01 | 0.02 | 0.01 | 0.07 |

Index size and build time move with the build: pg_search's size depends on how its parallel workers split the segments (161-185 MiB
over the builds of the sessions), build times move with load and with the two cores; sift's sizes do not.

How pg_textsearch is configured: a `bm25` index with `text_config = 'simple'` on the body (the analysis of the other engines), the
database `default_text_search_config` set to `simple` (its `@@` filter parses the column with it) and `enable_seqscan = off` in its
database, which the extension's README suggests for getting its index plans; adapter: `bench/engines/pg_textsearch.sh`. A ranked
OR is `ORDER BY body <@> to_bm25query(terms, 'docs_fts') LIMIT 10`, which the index answers with block-max WAND. It has no AND,
phrase or NOT inside that scan: those are the tsquery `@@` filter through the same index, and the extension does not combine
filter and ranking in one scan. Written as `WHERE body @@ q ORDER BY body <@> ... LIMIT 10` the planner chooses a sequential
scan that tokenizes every document (3 s per query at 25,000 articles), so the harness writes the AND, phrase, filter and join
queries as the extension's plan: `FROM (SELECT * FROM docs WHERE body @@ q OFFSET 0) docs ORDER BY (body <@> ...) * 1`, the filter
through the index and the standalone scores of its matches sorted. **The `* 1` is new in this session.** Without it the planner
replaces the score by the current score of the `@@` scan, which that scan does not compute: the earlier sessions' AND, phrase,
mixed, filter and join top-10 of pg_textsearch (for example 1472 queries per second for AND at the full tier) came back in index
order, not ranked (at the 150k tier, 53 of the first 200 AND queries have more than 10 matches and returned an arbitrary top-10). Scoring the matches
ranks them and costs roughly 50-100 microseconds per match, which is why AND is now 4 queries per second at the full tier. Without
`enable_seqscan = off` the planner counts a disjunction of common words with a sequential scan too: 0.25 queries per second for
`count(*)` against 37.9 with it off (a separate full-tier measurement of the earlier session, 8 clients, 8 s).
Correctness: its match sets and `count(*)` equal the tsvector oracle for 200 substrings in the and, or, phrase and not modes
(`bench/bench check oracle pg_textsearch 200`, PG parser tokens like gin and rum, 25,000 articles); its top-10 overlap with pgx-bm25's BM25
is below (`check rank`, 200 substrings; a soft check: it scores the PG parser's tokens and a bag of terms).

How rum is configured: a stored `tsvector` column (`to_tsvector('simple', body)`, a generated column, so the heap grows by
437.7 MiB and filling it takes 28.2 s on top of the 32.3 s `CREATE INDEX`) with a `rum_tsvector_ops` index, the same column gin
would index; adapter: `bench/engines/rum.sh`. A query is `WHERE tsv @@ tsquery ORDER BY tsv <=> tsquery LIMIT 10`, rum's own
ranking operator (`<=>` is rum's ts_rank-style distance from the positions it stores, not BM25).
Correctness: its match sets and `count(*)` equal the tsvector oracle for 200 substrings in the and, or, phrase and not modes
(`bench/bench check oracle rum 200`, 25,000 articles, the PG parser's tokens like gin and pg_textsearch).

Cold cache (index, heap and TOAST evicted from shared buffers before every query; `EXPLAIN ANALYZE` execution time
of 12 queries per class, `bench/bench cold --full --dataset articles --workers 2`), p50 / maximum in ms. The scratch data is on
tmpfs, so there is no disk behind the evicted pages: this is the cost of rebuilding the buffers from memory-backed files,
not of disk reads. All five engines and all seven classes, one session:

| class | sift | sift `pairs` | pg_search | pg_textsearch | rum |
|---|---:|---:|---:|---:|---:|
| and | 1.03 / 2.52 | 0.96 / 3.17 | 1.53 / 1.95 | 1.78 / 9,508 | 1.02 / 68.07 |
| or | 4.24 / 8.08 | 4.91 / 9.78 | 13.64 / 32.45 | 4.71 / 18.24 | 952 / 2,604 |
| phrase | 2.25 / 125 | 1.83 / 11.97 | 4.48 / 49.72 | 2.17 / 9,665 | 136 / 341 |
| mixed | 1.40 / 78.79 | 1.04 / 4.53 | 1.62 / 7.33 | 2.91 / 8,683 | 35.66 / 1,356 |
| count | 0.85 / 476 | 0.51 / 1.76 | 1.32 / 9.29 | 4.05 / 8,564 | 22.25 / 408 |
| filter | 1.41 / 45.83 | 1.19 / 5.23 | 1.95 / 5.50 | 5.23 / 8,925 | 31.09 / 1,175 |
| join | 1.76 / 638 | 1.50 / 10.17 | 6.52 / 84.94 | 3.76 / 8,758 | 87.77 / 3,274 |

The p50 of sift is 0.85-4.2 ms, pg_search's 1.3-13.6 ms, pg_textsearch's 1.8-5.2 ms and rum's 1.0 ms for AND and 22-950 ms for every
other class (its ranked and count queries read every match). pg_textsearch's maximum is 8.5-9.7 s in every class but OR, rum's up to 3.3 s; sift's default index has the slow maxima of its
heap recheck (phrase 125 ms, `count(*)` 476 ms, join 638 ms) that `pairs` removes (12, 1.8 and 10 ms).

Reading the numbers:

- sift's default index is the smallest and the quickest to build: 47.9 MiB against pg_search's 161.0 (3.4 times) and 3.1 against 9.9 s
  (3.2 times).
- AND is 1.67 times pg_search and the join 2.77 times; OR (0.95x) and mixed (0.89x) are level with it, and phrase (0.47x),
  `count(*)` (0.40x) and the filtered top-10 (0.67x) are behind it. The margins are wider on smaller corpora (150k articles: and
  2.68, or 1.22, phrase 0.67, mixed 1.15, count 0.61, filter 1.00, join 3.29; 25k: 3.53, 1.60, 1.55, 1.70, 1.51, 2.66, 1.81):
  the heap recheck grows with the corpus.
- Without `pairs`, a phrase is checked against the heap after the term intersection, and the check (decompressing and
  tokenizing the candidates) dominates when the words are common: phrase and `count(*)` are at 0.47x and 0.40x of
  pg_search at this size (0.65x and 0.70x in the earlier 8 vCPU session, 0.74x and 0.63x and 0.74x and 0.70x before: read it as
  0.4-0.8x), and the cold maximum of the default index shows the same cost. `phrase_index = pairs` removes it: phrase 2.00x,
  `count(*)` 2.08x, join 5.58x, AND 1.70x, mixed 1.18x, filter 0.97x and OR 0.77x, but its index is 5.6 times the default one and
  1.66 times pg_search's (267.2 against 161.0 MiB; 1.4-1.7 times over the builds of the sessions), and it takes 15.0 s to build against 9.9 s for
  pg_search: sift with `pairs` is the larger index.
- The recheck used to be slower. Profiling the phrase class (perf, one client) put 38% of the CPU in the per-document
  term table that the recheck built before looking for the phrase; the recheck now scans the document's tokens and builds
  the table only for the other leaf kinds. The same measurement with fixed instruction counts: phrase 48.9M to 36.6M
  instructions per query (7.0 to 4.7 ms CPU), `count(*)` 46.0M to 33.9M (6.4 to 3.8 ms), mixed 39.4M to 34.2M, and 8.2M
  (unchanged) for AND. What is left of the phrase cost is tokenizing (30% of the CPU) and decompressing
  the TOASTed text (21%).
- pg_textsearch wins one class and loses the others. A bare ranked disjunction is its native query, answered by block-max
  WAND: 777 queries per second, 1.34 times pg_search, 1.41 times sift and 1.74 times sift `pairs`, the fastest of the five at this size
  (3943 against 1774 for sift at 25k articles). Its index is 147.3 MiB (0.9 times pg_search, 3.1 times sift's default index) and takes
  19.5 s to build (2.0 times pg_search, 6.3 times sift; `pairs` takes 15.0 s). Everything that is not a bare disjunction goes through the
  tsquery filter and then scores the matches one by one (see the configuration above): AND runs at 4 queries per second and phrase (3),
  mixed (8), `count(*)` (14), filter (16) and join (13) at 0.00-0.19x of pg_search. The cost follows the number of matching
  articles, which the 2-15 word queries of the trace make large; I did not profile it further. The extension's README says that
  combining the filter with the ranking is not yet one index scan.
- rum is behind pg_search in every class but AND at the smaller tiers (1878 queries per second at 25k articles, 1.56x pg_search and 0.44x
  sift; 591 at 150k, 0.47x) and is the largest index: 309.9 MiB (1.9 times pg_search, 6.5 times sift's default index, 1.16 times sift
  `pairs`), 32.3 s for `CREATE INDEX` (3.3 times pg_search, 10 times sift) after 28.2 s to fill its stored `tsvector` column, which adds
  437.7 MiB to the table (748 MiB in all, 15.6 times sift's index). AND, a two-to-fifteen word intersection answered from the index,
  runs at 328 queries per second (0.27x of pg_search, 0.16x of sift). Every other class is two orders of magnitude lower: phrase 11,
  `count(*)` 12, mixed 6, filter 8, and OR and join at 3 and 5 queries per second. I did not profile it; its `<=>` ordering and the `@@`
  recheck of the stored `tsvector` (positions are checked in the heap for phrases) both visit every match of these common-word queries.
- In short, on two cores: sift's default index is the smallest and the quickest to build and wins AND (1.7x) and join (2.8x), is level on OR and
  mixed and behind on phrase, `count(*)` and the filtered top-10; `pairs` adds phrase (2.0 times), `count(*)` (2.1 times) and join
  (5.6 times) for 5.6 times the index; pg_textsearch wins only the bare ranked OR; rum wins nothing at this size and is the
  largest index.

### 150,000 articles (`ab` tier)

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

### 25,000 articles (`quick` tier)

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

## Places: GeoNames cities500, 236,262 places (full tier)

Multi-field autocomplete as an application would run it: sift indexes the name section and the all-fields text with
`fields = 'name', prefix_index = 3`, pg_search indexes `edge_ngram(1, 15)` fields for both; every typed word is a
prefix. `sift.max_expansions` is set to 100000 for the database: at the default 512, prefixes of four or more letters
of common words (`pari*`, `san*`) raise the expansion error on this corpus. Single client, 4 interleaved rounds of 2 s
per cell (`bench/bench run --full --dataset places --rounds 4-4 --duration 2 --workers 2`, load 9-36). Every engine lives in a database of its
own, a copy of the loaded table (the extra columns of rum and pg_textsearch do not enlarge the others' table; pg_textsearch's access method
has the name of pg_search's; no engine pays for another's hooks). Index size, build time and p50 latency in ms of the top-20 workload per
query class (pg_textsearch is the emulation described above). **The host was loaded in this session and the cells are single-client
milliseconds: between rounds the same cell moves by 2-10 times below 3 ms (minimum and maximum per cell: `results/places.tsv`), the
medians of 4 rounds carry that, and build times are 2-4 times those of an earlier session on the same host while index sizes did not move.
Read orders of magnitude, not 2x differences between sub-3 ms cells; `sift` and `sift pairs` index prefix queries identically, so their
difference is noise:**

| engine | index (MiB) | build (s) | p1 | p2 | p3 | p4 | w1 | a2 | a2s | a3 | x2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sift | 42.6 | 8.0 | 2.91 | 1.96 | 1.39 | 1.75 | 0.25 | 2.10 | 2.30 | 0.46 | 1.35 |
| sift `pairs` | 75.7 | 12.3 | 3.19 | 2.67 | 2.43 | 2.42 | 0.35 | 0.54 | 1.49 | 0.41 | 1.39 |
| pg_search | 125.8 | 39.3 | 2.99 | 7.69 | 6.62 | 4.42 | 5.22 | 6.74 | 8.92 | 10.39 | 5.89 |
| pg_textsearch | 68.3 | 16.0 | 8,795 | 1,356 | 186 | 37.91 | 16.02 | 15.39 | 21.46 | 11.50 | 10.80 |
| rum | 94.9 | 17.9 | 741 | 65.94 | 22.98 | 7.83 | 2.65 | 58.01 | 234 | 33.28 | 27.64 |

Extra columns and indexes of the emulated workloads, built after the native index in the same database (not in the table above):

| engine | what | index (MiB) | build (s) | column fill (s) | table growth (MiB) |
|---|---|---:|---:|---:|---:|
| pg_textsearch | `ng_text`, `ng_name`, two `bm25` indexes | 364.5 | 85.2 | 62.2 | 179.2 |
| rum | weighted tsvector, `rum_tsvector_ops` index | 95.0 | 17.3 | 26.3 | 61.9 |

pg_textsearch needs 433 MiB in all for its ranked prefix workloads (68.3 native, 364.5 for the n-gram indexes; plus 179.2 MiB of
stored columns), 3.4 times pg_search's 125.8 MiB.

p50 latency in ms of every workload, with the ratio to sift (workloads: top-20, name boost x3, filtered `country_code = 'US'`, `count(*)`, unordered
`population >= 100000`; places queries are prefixes and ANDs and never reach the phrase recheck):

**top-20**

| class | sift | sift `pairs` | pg_search | pg_textsearch | rum |
|---|---:|---:|---:|---:|---:|
| p1 | 2.91 | 3.19 (1.1x) | 2.99 (1.0x) | 8,795 (3022.5x) | 741 (254.8x) |
| p2 | 1.96 | 2.67 (1.4x) | 7.69 (3.9x) | 1,356 (692.1x) | 65.94 (33.6x) |
| p3 | 1.39 | 2.43 (1.7x) | 6.62 (4.8x) | 186 (133.5x) | 22.98 (16.5x) |
| p4 | 1.75 | 2.42 (1.4x) | 4.42 (2.5x) | 37.91 (21.7x) | 7.83 (4.5x) |
| w1 | 0.25 | 0.35 (1.4x) | 5.22 (20.9x) | 16.02 (64.1x) | 2.65 (10.6x) |
| a2 | 2.10 | 0.54 (0.3x) | 6.74 (3.2x) | 15.39 (7.3x) | 58.01 (27.6x) |
| a2s | 2.30 | 1.49 (0.6x) | 8.92 (3.9x) | 21.46 (9.3x) | 234 (101.6x) |
| a3 | 0.46 | 0.41 (0.9x) | 10.39 (22.6x) | 11.50 (25.0x) | 33.28 (72.3x) |
| x2 | 1.35 | 1.39 (1.0x) | 5.89 (4.4x) | 10.80 (8.0x) | 27.64 (20.5x) |

**name boost x3**

| class | sift | sift `pairs` | pg_search | pg_textsearch | rum |
|---|---:|---:|---:|---:|---:|
| p1 | 4.07 | 6.34 (1.6x) | 9.80 (2.4x) | 11,258 (2766.1x) | 355 (87.1x) |
| p2 | 3.94 | 4.40 (1.1x) | 11.27 (2.9x) | 2,013 (511.0x) | 72.88 (18.5x) |
| p3 | 2.66 | 2.39 (0.9x) | 16.25 (6.1x) | 137 (51.4x) | 22.19 (8.3x) |
| p4 | 2.62 | 3.18 (1.2x) | 15.04 (5.7x) | 43.25 (16.5x) | 4.97 (1.9x) |
| w1 | 2.67 | 1.58 (0.6x) | 19.53 (7.3x) | 18.73 (7.0x) | 3.66 (1.4x) |
| a2 | 2.21 | 1.81 (0.8x) | 22.42 (10.1x) | 18.68 (8.5x) | 83.05 (37.6x) |
| a2s | 2.11 | 2.99 (1.4x) | 16.29 (7.7x) | 14.80 (7.0x) | 241 (114.4x) |
| a3 | 2.53 | 2.28 (0.9x) | 30.65 (12.1x) | 21.93 (8.7x) | 50.81 (20.1x) |
| x2 | 1.08 | 1.37 (1.3x) | 19.58 (18.1x) | 26.10 (24.2x) | 24.18 (22.4x) |

**filtered top-20**

| class | sift | sift `pairs` | pg_search | pg_textsearch | rum |
|---|---:|---:|---:|---:|---:|
| p1 | 28.42 | 30.06 (1.1x) | 85.68 (3.0x) | 934 (32.9x) | 312 (11.0x) |
| p2 | 13.37 | 11.16 (0.8x) | 58.14 (4.3x) | 218 (16.3x) | 38.40 (2.9x) |
| p3 | 2.70 | 2.95 (1.1x) | 69.01 (25.6x) | 24.22 (9.0x) | 9.81 (3.6x) |
| p4 | 1.55 | 1.51 (1.0x) | 57.64 (37.2x) | 8.52 (5.5x) | 2.94 (1.9x) |
| w1 | 0.48 | 1.23 (2.6x) | 56.47 (117.6x) | 9.37 (19.5x) | 1.39 (2.9x) |
| a2 | 0.35 | 0.36 (1.0x) | 80.34 (229.5x) | 8.02 (22.9x) | 40.89 (116.8x) |
| a2s | 1.77 | 1.38 (0.8x) | 55.71 (31.5x) | 13.02 (7.4x) | 210 (118.5x) |
| a3 | 0.45 | 0.51 (1.1x) | 47.99 (106.6x) | 9.31 (20.7x) | 29.89 (66.4x) |
| x2 | 1.35 | 0.40 (0.3x) | 44.80 (33.2x) | 10.30 (7.6x) | 27.13 (20.1x) |

**count(*)**

| class | sift | sift `pairs` | pg_search | pg_textsearch | rum |
|---|---:|---:|---:|---:|---:|
| p1 | 0.44 | 0.42 (1.0x) | 0.95 (2.2x) | 2,091 (4753.0x) | 160 (363.7x) |
| p2 | 0.33 | 0.34 (1.0x) | 2.92 (8.8x) | 287 (871.2x) | 34.79 (105.4x) |
| p3 | 0.19 | 0.18 (0.9x) | 2.98 (15.7x) | 17.18 (90.4x) | 6.93 (36.5x) |
| p4 | 0.22 | 0.23 (1.0x) | 3.21 (14.6x) | 5.99 (27.2x) | 2.33 (10.6x) |
| w1 | 0.22 | 0.18 (0.8x) | 5.56 (25.3x) | 3.33 (15.1x) | 1.46 (6.6x) |
| a2 | 0.32 | 0.26 (0.8x) | 5.28 (16.5x) | 6.14 (19.2x) | 24.88 (77.8x) |
| a2s | 0.29 | 0.32 (1.1x) | 4.11 (14.2x) | 28.90 (99.7x) | 121 (415.8x) |
| a3 | 0.26 | 0.23 (0.9x) | 16.62 (63.9x) | 2.91 (11.2x) | 19.53 (75.1x) |
| x2 | 0.34 | 1.23 (3.6x) | 6.27 (18.4x) | 3.92 (11.5x) | 16.54 (48.6x) |

**unordered scan**

| class | sift | sift `pairs` | pg_search | pg_textsearch | rum |
|---|---:|---:|---:|---:|---:|
| p1 | 50.70 | 59.53 (1.2x) | 53.09 (1.0x) | 1,998 (39.4x) | 242 (4.8x) |
| p2 | 12.26 | 16.02 (1.3x) | 18.87 (1.5x) | 363 (29.6x) | 38.70 (3.2x) |
| p3 | 1.52 | 0.88 (0.6x) | 6.66 (4.4x) | 53.00 (34.9x) | 7.56 (5.0x) |
| p4 | 0.36 | 0.40 (1.1x) | 1.68 (4.7x) | 4.42 (12.3x) | 1.37 (3.8x) |
| w1 | 0.20 | 0.17 (0.8x) | 4.10 (20.5x) | 2.37 (11.8x) | 0.30 (1.5x) |
| a2 | 0.34 | 0.21 (0.6x) | 3.37 (9.9x) | 5.91 (17.4x) | 12.81 (37.7x) |
| a2s | 0.22 | 0.24 (1.1x) | 3.52 (16.0x) | 24.74 (112.5x) | 112 (510.9x) |
| a3 | 0.25 | 0.29 (1.2x) | 6.16 (24.6x) | 2.00 (8.0x) | 13.12 (52.5x) |
| x2 | 0.52 | 0.27 (0.5x) | 3.69 (7.1x) | 2.31 (4.4x) | 15.32 (29.5x) |

Reading the numbers:

- sift is ahead of pg_search in every cell of this session (45 of 45; 1.03x to 230x, the closest the one-letter prefixes p1 of the top-20
  and the scan, where the result is a large share of the corpus and both engines spend their time on ranking and fetching rows). The index is 3.0 times smaller (42.6 against
  125.8 MiB) and builds 4.9 times faster (8.0 against 39.3 s, a loaded session).
- sift `pairs` is not needed by prefix queries (none of these reaches a phrase) and costs 1.8 times the index (75.7 MiB) and 1.5 times the
  build; its latencies are 0.26x-3.6x of sift's (median 1.03x) cell by cell, which is the noise of the session, not a difference.
- pg_textsearch (emulation) is behind pg_search on the top-20 in every class and by orders of magnitude on the short prefixes: top-20 8.8 s at one
  letter, 1.4 s at two, 186 ms at three and 11-38 ms from four letters on (sift 0.25-2.9 ms). The exact BM25 ranking scores every match, and
  a one-letter prefix matches a large share of the places. On the filtered top-20 it is 2.8-10 times faster than pg_search from three letters on (8-24 ms
  against 45-80 ms: 17 of the 45 cells are below pg_search's latency, mostly filtered, multi-word and whole-word `count(*)` and scan). Its native count and scan are the
  prefix tsquery: 2.1 s for `count(*)` of a one-letter prefix, 2-53 ms from three letters on.
- rum (native top-20, filtered, count, scan; emulated boost) is within 3 times of sift on whole-word filtered queries (1.39 against 0.48 ms) and 11 times on whole-word top-20
  (2.65 against 0.25 ms), within 5 times on four-letter prefixes (7.83 against 1.75 ms top-20, 2.94 against 1.55 ms filtered), and tens to hundreds of times slower on the
  multi-word and short-prefix classes: 741 ms for a one-letter prefix (254 times sift's 2.91 ms), 234 ms for a word and a one-letter prefix. It is faster than pg_search in 14
  of 45 cells (whole words, four-letter prefixes, some filtered). Its index is 94.9 MiB (2.2 times sift's, 0.75 times pg_search's).
- The name boost costs pg_search 1.5-3.7 times its plain top-20 in every class (9.80 against 2.99 ms at one letter); sift's boost is
  0.8-2.0 times its top-20 except for two sub-3 ms cells within the noise of the session, so this table does not resolve it.

Cold cache (`bench/bench cold --full --dataset places --workers 2`, 12 queries per cell, scratch data on tmpfs as above), p50 / maximum in ms; all five
engines in one session:

| workload | class | sift | sift `pairs` | pg_search | pg_textsearch | rum |
|---|---|---:|---:|---:|---:|---:|
| top | p1 | 3.07 / 7.45 | 3.19 / 7.47 | 3.03 / 8.15 | 5,354 / 15,624 | 350 / 752 |
| top | p2 | 1.00 / 7.82 | 2.96 / 5.12 | 3.45 / 7.96 | 1,410 / 3,341 | 69.00 / 114 |
| top | a2 | 0.21 / 11.60 | 0.20 / 15.55 | 3.21 / 6.61 | 0.88 / 24.44 | 28.37 / 149 |
| top | a3 | 0.27 / 2.47 | 0.27 / 0.54 | 5.86 / 16.07 | 5.13 / 12.85 | 35.46 / 69.93 |
| boost | p1 | 10.00 / 16.01 | 6.47 / 25.39 | 10.50 / 29.25 | 12,312 / 30,641 | 292 / 931 |
| boost | p2 | 1.37 / 8.38 | 3.19 / 5.44 | 10.01 / 39.55 | 1,893 / 4,064 | 55.71 / 131 |
| boost | a2 | 0.31 / 18.63 | 0.30 / 18.11 | 5.07 / 10.05 | 4.67 / 68.01 | 25.63 / 144 |
| boost | a3 | 0.38 / 8.88 | 0.39 / 5.05 | 7.45 / 16.11 | 4.08 / 7.27 | 24.14 / 56.74 |
| filter | p1 | 23.36 / 34.40 | 27.05 / 44.13 | 69.17 / 167 | 765 / 2,006 | 266 / 478 |
| filter | p2 | 10.71 / 23.50 | 14.14 / 32.20 | 48.00 / 108 | 126 / 222 | 37.28 / 78.69 |
| filter | a2 | 0.20 / 9.00 | 0.21 / 4.97 | 25.34 / 61.12 | 0.43 / 4.84 | 21.44 / 77.69 |
| filter | a3 | 0.27 / 2.29 | 0.27 / 2.51 | 23.56 / 82.65 | 0.53 / 2.61 | 23.35 / 39.95 |
| count | p1 | 0.33 / 0.41 | 0.33 / 0.39 | 1.91 / 4.58 | 995 / 2,215 | 170 / 237 |
| count | p2 | 0.15 / 0.22 | 0.15 / 2.20 | 0.78 / 3.07 | 304 / 771 | 134 / 551 |
| count | a2 | 0.09 / 5.04 | 0.09 / 6.01 | 6.84 / 23.52 | 2.84 / 38.02 | 51.24 / 272 |
| count | a3 | 0.15 / 0.32 | 0.20 / 2.20 | 18.68 / 29.31 | 8.15 / 20.10 | 48.31 / 95.82 |

These are execution times (`EXPLAIN ANALYZE`), without the planning and the query lookup that the warm p50 includes, so they are not
comparable with the tables above. sift is ahead of pg_search in 15 of the 16 cold cells; the exception is the one-letter top-20
(3.07 against 3.03 ms).

### 34,165 places (`ab` tier, cities15000)

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

All cells of the three tiers are in `results/places.tsv`.

## Correctness

`bench/bench check oracle|rank ENGINE` checks the articles engines against a tsvector oracle (match sets, `count(*)`) and pgx-bm25's BM25
(soft top-10 overlap); `bench/bench check --dataset places [oracle|rank] [N]` checks the places engines against an exact oracle in SQL
(`bench/sql/places_oracle.sql`), N queries per class (30: 270 queries, 34,165 places):

- match set: the places returned for a typed query must be those with every typed word as a prefix of a word of the all-fields text, with
  the engine's own idea of a word (pg_search's tokenizer, sift's accent folding, the PostgreSQL parser for rum and pg_textsearch's native
  index, alphanumeric runs for the n-gram text of the pg_textsearch emulation);
- top-20, boost, filtered top-20: BM25 over the edge n-grams (k1 = 1.2, b = 0.75, the typed words' distinct n-grams, boost = all-fields
  score + 3 x name score, ties at rank 20 accepted): the share of the returned places that the oracle accepts as a top-20, averaged over the queries
  (`agree`; `exact` counts the queries at 1.0), and the returned places that are not matches (`wrong`).

Places, `bench/bench check --dataset places 30` (270 queries, 34,165 places, `agree` = Jaccard index of the match sets for `match`, share of
accepted places for the ranked workloads; `exact` of 270 queries at 1.0):

| engine | match exact / agree | top-20 exact / agree | boost exact / agree | filtered top-20 exact / agree |
|---|---|---|---|---|
| pg_search | 268 / 1.000 | 202 / 0.982 | 192 / 0.972 | 231 / 0.992 |
| pg_textsearch (emulation) | 270 / 1.000 | 201 / 0.982 | 213 / 0.984 | 232 / 0.992 |
| rum | 270 / 1.000 | 154 / 0.759 | 154 / 0.720 | 193 / 0.876 |
| sift, sift `pairs` (identical) | 228 / 0.985 | 154 / 0.826 | 152 / 0.807 | 194 / 0.929 |

- The oracle is sound: pg_search, which ranks BM25 over the same n-grams, agrees at 0.97-0.99, and the pg_textsearch emulation, which is the same computation in an extension, agrees at 0.98 with all match sets
  exact. pg_search's 2 inexact match sets are a tokenizer difference (its edge-ngram tokenizer splits `i` out of some names).
- sift does not rank by n-grams: a typed prefix is expanded to the indexed words and scored as them, so its top-20 is another
  ranking than the oracle's, most visibly on the 1-2 letter prefixes (p1 0.46, p2 0.63) whose matches are a large share of the corpus;
  from four letters on it agrees at 0.85-0.95. Its match sets agree at 0.985; the `a3` class (a word, then a 2-3 letter prefix) is the
  inexact one (0.867, 310 places that the oracle does not return); I did not investigate it. rum's `<=>` is a proximity ranking, not BM25:
  0.23 at one letter, 0.93 on whole words; its match sets are exact.

Articles, 25,000-article tier, 200 substrings (`bench/bench check oracle|rank ENGINE 200`): match sets and `count(*)` of pg_textsearch
and rum equal the tsvector oracle in all four modes (and, or, phrase, not: 0 mismatches). Top-10 overlap with pgx-bm25's BM25
(a soft check):

| engine | and | or | phrase |
|---|---:|---:|---:|
| sift | 0.997 | 0.979 | 0.999 |
| pg_textsearch | 0.993 | 0.967 | 0.998 |
| rum | 0.916 | 0.180 | 0.964 |

pg_textsearch's AND overlap was 0.88 before its score was written `(... <@> ...) * 1` (the unranked top-10 described above).

## Harness run time

| | before (review-text corpus) | now |
|---|---|---|
| per interleaved round, 3 engines | about 5 min 11 s for 6 classes at 10 s per cell, plus about 35 s for the join | about 9 min per round of 7 articles classes with five engines at 6 s per cell (8 s with the warm-up) |
| corpus and indexes | about 20 s corpus load per run | restore of the cached dump 8 s, each index built once (about 110 s for the five at 218k articles on two cores, with rum's column) |
| full articles run | not repeatable in one command | 4 rounds of 6 s with five engines (sift, sift `pairs`, pg_search, pg_textsearch, rum): 36 min with the builds (load 15-43, two cores); early stop when every cell is within 15% |
| 150k articles run | | 26 min for 6 rounds, five engines (load 13-30) |
| quick tier | | articles 2 rounds of 1 s and places 2-5 rounds: 8 min with five engines |
| places | | 34k tier 22 min (5 rounds, five engines), full 39 min (4 rounds of 2 s, five engines, 45 cells per engine; load 9-36) |
| cold cache | | articles 4 min 15 s (7 classes, five engines), places 13 min (16 cells per engine, five engines) |

`bench/bench all --full` runs the full tier, the cold-cache tables, writes `docs/results/*.tsv` and redraws `docs/img` in one command (about 2 hours on two pinned cores of this
host); the prose of this file is not generated. Under this host's load no run reached the early-stop spread. Results are kept as small tables per run (`cells.tsv`, `builds.tsv`,
`summary.tsv`, `machine.txt` with the CPU, engine versions and load) in the result directory, and the scratch cluster is removed when the run ends.
