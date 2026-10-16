# sift benchmark and correctness harness

Two public datasets, measured on sift, its variants (`phrase_index = pairs`, and `positions = off` once the installed sift has that reloption), ParadeDB pg_search, pg_textsearch and rum with the same
rows, queries, clients, resource budget and cache state, the engines interleaved cell by cell so host load drifts hit
all of them alike. Everything runs through one entry point, `bench/bench`; `bench/bench --help` is the full reference.

| use-case | data | what it exercises |
|---|---|---|
| articles (full text) | Simple English Wikipedia | and / or / phrase / mixed top-10, `count(*)`, filtered, join |
| places (multi-field autocomplete) | GeoNames | 1-4 letter prefixes, whole words, 2-3 word ANDs, name-field boost, top-k, filtered, unordered scans, `count(*)` |

## Reproduce

```sh
bench/bench all                    # setup + run + report at the reduced default scale: about 10 minutes
bench/bench all --full             # the full-size run (the scale of the published numbers) with the cold-cache tables, docs/results and the charts: about 2 hours on two pinned cores
```

Scale: the default run uses a reduced dataset (25,000 articles, 34,165 places) and two rounds of 1 s per cell, which is
enough to see the ordering of the engines but not their exact ratios; `--full` (or `--tier full`) uses all 218,058
articles and the 236,262 places of cities500 with 4-6 rounds. `--tier ab` is the 150,000 article / 34,165 place tier
in between (3-6 rounds, about 25 minutes). The first run also downloads and converts the datasets (about 3 minutes,
once). Ratios depend on the size of the corpus (the default index's phrase and `count(*)` cross pg_search somewhere
between 150,000 and 218,058 articles, see docs/BENCHMARKS.md), so quote the full-size numbers.

Prerequisites: PostgreSQL 18 with its development packages and contrib (`pg_config`, PGXS, `pgbench`, `psql`, the
server tools, `pg_stat_statements`, `pg_prewarm`, `pg_buffercache`), `gcc` and `make`, `python3`, `curl`, `bzip2`,
`unzip`, `ar`, `flock`, network access, 8 GB (quick, ab) to 15 GB (full) of free space under `$SIFT_BENCH_ROOT`
(default `/tmp/sift-bench`; a fast disk or tmpfs). Nothing is installed system-wide and no sudo is needed. `setup`
checks all of this before it downloads anything.

`all` is three commands that can be run on their own (for the `ab` and `full` tiers it also runs `cold` and writes `docs/results` and
`docs/img`, so one command regenerates the committed results, tables and charts):

| command | what it does |
|---|---|
| `bench/bench setup` | checks the prerequisites; downloads, verifies and converts the datasets; builds this checkout's sift, the pinned rum and pg_textsearch (PGXS) and unpacks the pinned pg_search package into `$SIFT_BENCH_ROOT` |
| `bench/bench run --tier T` | machine description, then the articles and places A/B on a throwaway cluster into a result directory; an engine that is not installed is skipped with a message |
| `bench/bench report` | `results.tsv`, `builds.tsv` and `tables.md` from the result directory (`--docs` also writes `docs/results/<dataset>.tsv` and redraws `docs/img`) |

The other commands: `charts` (redraw the SVGs of `docs/img` from the committed `docs/results`), `cold` (cache-evicted latency of every class on every engine), `check oracle|rank ENGINE` (correctness against a tsvector
oracle / pgx-bm25; `check --dataset places` against the SQL oracle of the places queries), `suite` (the standard multi-engine suite: gin, rum, pg_textsearch, pgx-bm25, ParadeDB, sift,
Tantivy, Elasticsearch), `workload ENGINE` (one engine, timed pgbench workloads), `workers` (sift build time per
parallel worker count), `remote` (the same on an ssh host), `machine`, `clean [--all]`. Options: `--tier quick|ab|full` (`--full` = `--tier full`),
`--dataset articles|places|all`, `--engines sift,sift_nopos,sift_pairs,pg_search,pg_textsearch,rum`, `--clients N`, `--workers N` (pgbench threads and
the server's parallel workers, for a host that is shared), `--duration S`, `--rounds MIN-MAX`, `--out DIR`, `--no-build`, `--no-datasets`.

| tier | articles | places | rounds | expected run time of `all` |
|---|---|---|---|---|
| `quick` (default) | 25,000 | 34,165 (cities15000) | 2 | 8 min on a shared host from an empty `$SIFT_BENCH_ROOT` (2.8 min of downloads and builds, 5.5 min of measurements with the five engines); about 6 min once the datasets are cached |
| `ab` | 150,000 | 34,165 | 3-6 | about 50 min on a shared host on two pinned cores (articles 26 min, places 22 min, plus setup) |
| `full` (`--full`) | 218,058 | 236,262 (cities500) | 4-6 | about 1 h 15 min on a shared host on two pinned cores (articles 36 min with the builds, places 35-40 min), plus the cold-cache tables (about 17 min) |

Result directory (default `bench/out/run-<UTC time>`, gitignored; `bench/out/latest` links to the last one):

| file | content |
|---|---|
| `machine.txt` | CPU, cores, SIMD, RAM, kernel, OS, PostgreSQL, engine versions and where they come from, sift commit, engine budget, load average at start, wall time |
| `results.tsv` | chart input, one row per measurement (columns below) |
| `builds.tsv` | setup time, index build time, index size and table growth of every build |
| `tables.md` | the same numbers as Markdown: one row per engine with index size, build time and queries per second per class (articles) or p50 latency per class (places), then the ratios |
| `articles-<tier>/`, `places-<tier>/` | the raw tables of the run: `cells.tsv` (every cell of every round with the load average at that moment), `builds.tsv`, `summary.tsv`, `machine.txt`; `*-cold` for `cold` (`summary.tsv` / `cold.tsv`) |

Read the numbers within one table of one run: the engines were measured one after another in the same session, so
the ratios are meaningful when the load average (`cells.tsv`, `tables.md`) was steady; absolute values depend on the CPU
and on whatever else the host runs. A cell the engine cannot run is `NA`, one whose statement failed or timed out is
`error`. The committed results of the final session are in [docs/results/](../docs/results/), the discussion in
[docs/BENCHMARKS.md](../docs/BENCHMARKS.md).

`results.tsv` columns (the first line is a `#` comment with the same list; tab separated, stable for chart generators):

| column | values |
|---|---|
| `dataset` | `articles`, `places` |
| `tier` | `quick`, `ab`, `full`, `quick-cold`, `ab-cold`, `full-cold` |
| `engine` | `sift`, `sift_nopos` (`positions = off`), `sift_pairs` (`phrase_index = pairs`), `pg_search`, `pg_textsearch`, `rum`; `-` for host rows |
| `class` | articles: `and or phrase mixed count filter join`; places: `p1 p2 p3 p4 w1 a2 a2s a3 x2`; `-` for build and host rows |
| `variant` | places workload `top boost filter count scan` (cold: the workload too); `emulation` on the build rows of the extra columns and indexes of an emulated workload; `-` otherwise |
| `metric` | `qps` (articles, 8 clients), `p50_ms` / `p95_ms` (places, 1 client), `cold_p50_ms` / `cold_max_ms` (tier `<tier>-cold`), `index_mib`, `build_s` (CREATE INDEX), `setup_s` (work before it, rum's generated column), `heap_growth_mib` (table growth of that setup), `load_1m` |
| `value`, `min`, `max` | median over the interleaved rounds and its range (`NA` / `error` as above); one build per engine for the build metrics |
| `unit` | `queries/s`, `ms`, `MiB`, `s`, `load average` |

Pinned inputs are all in [versions.env](versions.env): Wikipedia dump date and sha1, GeoNames date and sha256 (GeoNames
regenerates its files daily and keeps no archive, so a mismatch only warns; the places, and the numbers, then differ
slightly), pg_search 0.26.1, pg_textsearch 1.5.1 (tag v1.5.1 of timescale/pg_textsearch, commit and tarball sha256),
rum 1.3.15 (tag of postgrespro/rum, tarball sha256), PostgreSQL major. sift is built from the checkout
(`SIFT_SRC=head` builds the last commit instead of the working tree); its commit, and whether the tree had
uncommitted changes, is in `machine.txt`.

Adding an engine: write `engines/<name>.sh` (see Engine adapters below), add a line (id, labels, chart color) to
`engines.tsv`, which sets the engine list of `bench`, `steps/ab.sh`, `steps/tables.py` and `steps/charts.py` and the order of every
table and chart, and a `fetch_<name>` function to `steps/fetch.sh` if it has to be built (and its pinned version to `versions.env`). For places add the setup, index and statements to the `SETUP`, `INDEX` and
`stmt` tables of `steps/places.sh`.

## Datasets

`bench/datasets/fetch.sh articles|places|all` (run by `setup`) downloads each dataset into its own new directory under
`$SIFT_BENCH_ROOT/download`, verifies it, writes `SHA256SUMS` next to it and converts it with the
`python3 -I` scripts in `bench/datasets/` (the downloads are only read, never executed).

| dataset | source | license | checksum |
|---|---|---|---|
| Simple English Wikipedia, `simplewiki-20261001-pages-articles.xml.bz2` (342 MB) | https://dumps.wikimedia.org/simplewiki/20261001/ | CC BY-SA 4.0 | sha1 `f55881650e447c39e8bdeba4b1bb8d5576342d4b`, equal to the published `simplewiki-20261001-sha1sums.txt` |
| GeoNames `cities500.zip` (236,262 places), `cities15000.zip` (34,165), `admin1CodesASCII.txt`, `countryInfo.txt`, `featureCodes_en.txt` | https://download.geonames.org/export/dump/ (files of 2026-10-16) | CC BY 4.0 | sha256 `ca136e8a...f626e05` (cities500), `da8eca61...2cf2069` (cities15000), pinned in full in `versions.env`; all files in the `SHA256SUMS` written by the script |

An article is its title plus the cleaned wikitext of one main-namespace, non-redirect page of at least
200 characters (218,058 articles, 1.4 KB on average). A place is name, alternate names, region,
country, country code, feature name and population; the typed queries (300 per class) are sampled from
the places themselves with a fixed seed. Without network access, nothing else in the harness changes:
put any `articles.tsv` (COPY text of `id, title + body, category_id, created, likes, url`) in
`$SIFT_BENCH_ROOT/data`.

## Results

Tables, build times, sizes, cold-cache runs and the host load of every round are in [docs/BENCHMARKS.md](../docs/BENCHMARKS.md),
the numbers of the final session in [docs/results/](../docs/results/).

## How a run works

`bench run` calls `steps/ab.sh` (articles) and `steps/places.sh` (places). `ab.sh` builds every engine database once
from the cached corpus dump, then runs every class on every engine, one cell at a time, the engine order rotated each
round, so host load drifts hit all engines alike. Rounds stop once every cell's spread is under `TOL` after `MINR`, or
at `MAXR`. It keeps only compact tables (`cells.tsv` with the load average of every cell, `builds.tsv`, `summary.tsv`
with medians and ratios, `machine.txt`) and removes its scratch cluster on exit (`KEEP=1` keeps it); the corpus dump
stays in the cache. Per-tier cluster, port and dump:

| tier letter | articles | port | dump |
|---|---:|---:|---|
| Q (`quick` 25,000, `ab` 150,000: `Q_DOCS`) | 25,000 / 150,000 | 56040 | `$SIFT_BENCH_ROOT/cache/sift-Q-<docs>.dump` |
| F (`full`) | 218,058 | 56050 | `$SIFT_BENCH_ROOT/cache/sift-F-ALL.dump` |

## Layout

| path | role |
|---|---|
| `bench` | the entry point: `bench/bench --help` |
| `versions.env` | every pinned version, date and checksum |
| `engines.tsv` | the A/B engines in table order: id, chart and Markdown labels, chart color |
| `steps/` | internal helpers, called by `bench` (below); not meant to be run directly |
| `datasets/` | `fetch.sh`, `wiki.py`, `places.py`: download, verify, convert |
| `engines/*.sh` | one adapter per engine (see below) |
| `sql/` | schema, tier load + query trace, harness (rendering), oracle, places schema |
| `xbench/` | Rust load generator (HTTP keep-alive) and in-process Tantivy driver |

| `steps/` | role |
|---|---|
| `ab.sh`, `places.sh` | the interleaved A/B runs of articles and places (`run`, `cold`) |
| `lib.sh` | shared settings and helpers: tiers, ports, engine roots, `q` (psql), `load_engine`, `rounds` / `rotated` / `spread_ok` (the interleaved rounds), `evict_table` (cold cache), `locked` (perf lock), `limited` (8 CPU / 32 GB budget, a cgroup scope when a systemd user session exists) |
| `machine.sh`, `tables.py` | the machine and engine description; raw run tables to `results.tsv` / `tables.md` (`report`) |
| `charts.py` | SVG charts of `docs/results/*.tsv` into `docs/img`, all five engines in every chart, an NA as a gap (`charts`; stdlib only, same tables give the same bytes) |
| `cluster.sh` | per-tier throwaway PG18 cluster: `TIER=F steps/cluster.sh init\|start\|stop\|destroy` |
| `corpus.sh` | `load` a tier and cache its dump, `restore` it |
| `fetch.sh` | build sift and rum (PGXS), unpack pg_search, fetch Elasticsearch, build `xbench`: one `$SIFT_BENCH_ROOT/engines/<name>/{dl,root}` per engine, which the cluster searches for extensions |
| `run.sh`, `xrun.sh` | one engine: timed index build + pgbench workloads (`BUILD_ONLY=1`: build and keep); non-Postgres engines driven by `xbench` (`workload`) |
| `quick.sh`, `report.sh` | the multi-engine standard suite and its tables (`suite`) |
| `build.sh` | sift CREATE INDEX per parallel worker count, appends to `build-workers.tsv` (`workers`) |
| `facets.sh`, `facets_es.py`, `parallel.sh` | standalone, run directly: facet counts of `sift.facets` against GROUP BY and Elasticsearch (DESIGN.md (l), its own scratch cluster under `$FACET_TMP`); speedup of the parallel scan by worker count on a restored `bench` database |
| `oracle.sh`, `rank_check.sh` | exact match-set / count(*) oracle (tsvector); soft BM25 check against pgx-bm25 (`check`) |
| `remote.sh` | sync, build, run and clean the whole harness on a remote host (`remote`) |

`bench` holds `docs(id, body, category_id, created, likes, url)`, `categories(id, name, region)` for
the join workload, `params` (tier constants) and `queries`: 1000 substrings of 2-15 consecutive plain
word tokens (PG `simple` parser) from random documents, each as `and`, `or` and `phrase` (3000
queries). The oracle adds a `not` form (all terms, last one negated).

## Engine adapters

Postgres adapters (`gin`, `rum`, `pg_textsearch`, `bm25` = pgx-bm25, `pg_search`, `sift`, `sift_pairs` = sift
with `phrase_index = pairs`, `sift_nopos` = sift with `positions = off`) set `REQUIRES` (extension), `PROBE` (optional: a statement over a temp table `probe (t text)` that must work for the engine to run, e.g. a reloption of a newer sift build; the engine is skipped with a message otherwise), `SETUP`, `INDEX`, and `PRED` and `RANK`: SQL expressions over
`mode` and `terms` rendering the WHERE condition and the ORDER BY of a query, NULL if the engine cannot answer it
(`HELPERS`: SQL they use). `steps/lib.sh` turns them into `bench_pred(mode, terms)` and `bench_rank(mode, terms)` and
`sql/harness.sql` composes every statement kind from them (top10, count, match, filter, join), so a new engine is
one small file. `sift` follows DESIGN.md (`body @@> 'a AND b'`,
`sift.score(ctid)`); the cluster loads it from the engine roots (`setup` builds sift into
`$SIFT_BENCH_ROOT/engines/sift/root`) and the harness skips an engine whose extension is absent. rum is a stored `tsvector` column
(`simple` configuration) with a `rum_tsvector_ops` index, queried with `@@` and ranked with `<=>`; on places it
lives in its own database so the column does not enlarge the other engines' table, and it has no per-field boost
(its `boost` cells are an emulation, below). pg_textsearch is a `bm25` index with `text_config = 'simple'` (the analysis of gin and rum); see its adapter for the
queries: `@@ tsquery` filters through the index, `ORDER BY body <@> to_bm25query(...)` ranks, a ranked disjunction needs
no filter, AND and phrase top-k filter through the index in a derived table whose matches are scored standalone (the plain form is planned as a
sequential scan, and a bare `ORDER BY body <@> ...` over the `@@` scan does not rank), and the database runs with `enable_seqscan = off` as
the extension's README suggests. ParadeDB is indexed as an application would: `bm25 (id, body, created, category_id)`
with the default tokenizer, and `edge_ngram(1, 15)` on the places name and all-fields text.

Places adapters live in `steps/places.sh` (`SETUP`, `INDEX`, `stmt`). sift, its variants (`pairs`, `positions = off`) and pg_search answer every workload natively. rum and pg_textsearch
filter by the prefix tsquery (`'gra':* & 'ho':*`, their native prefix match) for `count` and `scan`; they have no native equivalent of the ranked workloads, which are
emulations built next to the native index, in the same database, reported as the `emulation` build of the engine (variant `emulation`
in `results.tsv`) and described in docs/BENCHMARKS.md:

| engine | workload | emulation |
|---|---|---|
| pg_textsearch | top, filter | `bm25` indexes on stored columns holding the edge n-grams (1-15 letters) of every word, pg_search's analysis, so a typed prefix is a whole term; the matches `ng_text @@ plainto_tsquery(typed)` (a derived table) are scored standalone with the distinct n-grams of the typed words |
| pg_textsearch | boost | the same matches ordered by the all-fields score plus 3 x the score of a second index on the name field (a weighted sum of two bm25 scores) |
| rum | boost | a second stored tsvector with the name words at weight A (1.0) and the other fields at B (0.4), indexed with `rum_tsvector_ops` and ranked with `<=>`; the weights are rum's, so the name counts 2.5 times, not 3 |
| sift pairs, sift positions off | all | the variants of the index are not needed by prefixes (never a phrase); they run to show their cost on a places workload |

Each pg_textsearch and rum database keeps the tables of both: `count` and `scan` run on the native index and are not changed by the emulation.

External adapters (`elasticsearch`, `tantivy`) set `EXTERNAL`, `URL`, `XB_ARGS` and define `x_start`, `x_stop`,
`x_build`, `x_size`, `x_pid` plus SQL `x_req` / `x_write` (in `RENDER`) that render requests from the same query trace.

## Workloads (`run`, `workload`)

`and`, `or`, `phrase`, `mixed` (all three, top-10), `count` (count(*) of all three), `filter` (top-10 +
`created >= cutoff`, ~2% of rows), `join` (top-10 joined with `categories`, region filter), `update`
(`workload` only: writers at `UPDATE_RATE`=1000 ops/s: 70% update, 15% insert, 15% delete, while `mixed`
readers run; WAL bytes from `pg_current_wal_lsn`). Defaults of `workload`: 8 clients, 5 s warm-up, 30 s timed,
`statement_timeout` 120 s. Its timed sections hold `flock /tmp/sift-perf.lock`; the engine
servers run in a `systemd-run --user --scope` with `CPUQuota=800%` and `MemoryMax=32G` when a systemd user session exists.

Places classes are what a user has typed: `p1`-`p4` a name's first word cut to 1-4 letters, `w1` a whole
word, `a2`/`a2s` two words (last cut to 2 / 1 letters), `a3` name, region and country word, `x2` name word
plus country word; workloads `top` (top-20), `boost` (name matches x3), `filter` (`country_code = 'US'`),
`count`, `scan` (unordered, `population >= 100000`).

## Remote host

`bench/bench remote` reproduces everything on another machine under `~/Projects/sift` without sudo: a PostgreSQL
18.6 build in `pg18/` with contrib, rum, pg_textsearch, pgvector and pgx-bm25 built by PGXS, the ParadeDB
package, Elasticsearch, a minimal rustup and `xbench`. `RHOST` names the ssh host.

```sh
RHOST=bench-host bench/bench remote sync     # repo (no .git), Q/F dumps, pgx-bm25 source (BM25_SRC)
bench/bench remote setup                     # builds (nice, -j4)
bench/bench remote run 'bench/bench workload --tier full gin; bench/bench workload --tier full tantivy'
bench/bench remote pull                      # -> bench/out-<host>/
bench/bench remote clean                     # keep dumps; `clean all` removes ~/Projects/sift
```

## Correctness

```sh
bench/bench check --tier ab oracle sift 200                        # accent + case folding, no stemming (default)
bench/bench check --tier ab oracle pg_textsearch 200               # gin, rum and pg_textsearch: raw PG parser tokens (their adapters set it)
MODES="phrase not" REBUILD=1 bench/bench check --tier ab oracle sift 50
bench/bench check --tier ab rank sift 200                          # top-10 overlap with pgx-bm25 per mode
```

The rank check is soft: pgx-bm25 is the reference, it tokenizes alphanumeric runs while pg_textsearch (and rum, gin) use the PG
parser, and pg_textsearch scores a query as a bag of terms; expect overlaps below 1 for pg_textsearch (docs/BENCHMARKS.md has the numbers).

`oracle` builds the engine index in `o_<engine>` (reused unless `REBUILD=1`) and an oracle `oracle_tsv`
table (`to_tsvector` with a `simple`-based configuration, `unaccent` when `ORACLE_ACCENTS=fold`,
`<lang>_stem` when `ORACLE_STEM=<lang>`). For each query it compares the engine's match set and `count(*)`
with the oracle and prints a summary plus every mismatch. Exit status is 1 on any mismatch; the report is
saved as `bench/out/<tier letter>/oracle-<engine>.txt`. By default the oracle text is split into alphanumeric
runs first (`ORACLE_TOKENS=split`), the tokenization of sift, because the PG parser keeps `Force.In`, `a@b.c` or
`1.5` as one token; `ORACLE_TOKENS=pg` uses the PG parser as is (gin, rum, pg_textsearch, whose adapters select it). tsvector positions saturate at 16383, so
phrases are not compared in the few documents longer than that.

## Cleanup

`bench/bench clean` destroys the clusters and removes the scratch under `$SIFT_BENCH_ROOT`; `run` does it for its own
cluster on exit. The datasets, dumps and engine builds stay (`clean --all`, or `rm -rf $SIFT_BENCH_ROOT`, removes everything).
