# sift benchmark and correctness harness

Two public datasets, measured on sift, its variants (`phrase_index = pairs`, and `positions = off` once the installed sift has that reloption), ParadeDB pg_search, pg_textsearch, rum
and pgx-bm25 with the same rows, queries, clients, CPU budget and cache state. By default every engine has a cluster of its own on an equal
share of the CPU budget and all engines are measured at the same time (`--serial`: one cluster, engines interleaved cell by cell), so host
load drifts hit all of them alike. Everything runs through one entry point, `bench/bench`; `bench/bench --help` is the full reference.
One suite serves every engine: `engines.tsv` (the registry) plus `engines/<id>.sh` (the adapter), one runner (`steps/articles.sh`, `steps/places.sh`),
one result format and one report path (`steps/tables.py`, `steps/charts.py`); gin, Tantivy and Elasticsearch
run through the same pipeline (Tantivy and Elasticsearch through `xbench`) and are charted only when the registry says so.

| use-case | data | what it exercises |
|---|---|---|
| articles (full text) | Simple English Wikipedia | and / or / phrase / mixed top-10, `count(*)`, filtered, join |
| places (multi-field autocomplete) | GeoNames | 1-4 letter prefixes, whole words, 2-3 word ANDs, name-field boost, top-k, filtered, unordered scans, `count(*)` |

## Reproduce

```sh
bench/bench all                    # setup + run + report at the reduced default scale: about 2 minutes of measurements
bench/bench regress --run          # sift against a build of an older release (sift_ref, --ref, default v1.1.0): exit 1 on a regression, about 1 minute of measurements
```

Scale: the default run uses a reduced dataset (25,000 articles, 34,165 places) and two rounds of 1 s per cell, which is
enough to see the ordering of the engines but not their exact ratios; `--full` (or `--tier full`) uses all 218,058
articles and the 236,262 places of cities500 with 2-3 rounds. `--tier ab` is the 150,000 article / 34,165 place tier
in between (2-3 rounds). The first run also downloads and converts the datasets (about 3 minutes,
once). Ratios depend on the size of the corpus (the default index's phrase and `count(*)` cross pg_search somewhere

Prerequisites: PostgreSQL 18 with its development packages and contrib (`pg_config`, PGXS, `pgbench`, `psql`, the
server tools, `pg_stat_statements`, `pg_prewarm`, `pg_buffercache`), `gcc` and `make`, `python3`, `curl`, `bzip2`,
`unzip`, `ar`, `flock`, network access, 8 GB (quick, ab) to 15 GB (full) of free space under `$SIFT_BENCH_ROOT`
(default `/tmp/sift-bench`; a fast disk or tmpfs). Nothing is installed system-wide and no sudo is needed. `setup`
checks all of this before it downloads anything.

`docs/img`, so one command regenerates the committed results, tables and charts):

| command | what it does |
|---|---|
| `bench/bench setup` | checks the prerequisites; downloads, verifies and converts the datasets; builds this checkout's sift, the pinned rum and pg_textsearch (PGXS) and unpacks the pinned pg_search package into `$SIFT_BENCH_ROOT` |
| `bench/bench run --tier T` | machine description, then the articles and places A/B on a throwaway cluster into a result directory; an engine that is not installed is skipped with a message |
| `bench/bench report` | `results.tsv`, `builds.tsv` and `tables.md` from the result directory |

The other commands: `charts`, `cold` (cache-evicted latency of every class on every engine), `check oracle|rank ENGINE` (correctness against a tsvector
oracle / pgx-bm25; `check --dataset places` against the SQL oracle of the places queries), `suite` (the standard multi-engine suite: gin, rum, pg_textsearch, pgx-bm25, ParadeDB, sift,
Tantivy, Elasticsearch), `workload ENGINE` (one engine, timed pgbench workloads), `workers` (sift build time per
parallel worker count), `remote` (the same on an ssh host), `machine`, `clean [--all]`. Options: `--tier quick|ab|full` (`--full` = `--tier full`),
`--dataset articles|places|all`, `--engines sift,sift_nopos,sift_pairs,pg_search,pg_textsearch,rum,bm25`, `--classes LIST` and `--workloads LIST` (restrict a run to some
query classes / places workloads), `--cpus LIST` and `--load-cpus LIST` (the CPU budget, below), `--serial` (one cluster, engines interleaved cell by cell, as the published tables were
measured), `--rebuild` (ignore the cache of built clusters), `--sample N` (statements per articles class; default 0 = the whole trace), `--clients N`, `--workers N` (pgbench threads and
the server's parallel workers), `--duration S`, `--rounds MIN-MAX` (default 2-3), `--rse X` (the relative standard error that ends the rounds of a cell, default 0.05),
`--ref REF` and `--run` (`regress`), `--out DIR`, `--no-build`, `--no-datasets`.

| tier | articles | places | rounds | expected run time of `all` |
|---|---|---|---|---|
| `quick` (default) | 25,000 | 34,165 (cities15000) | 2 | measured: 2 min for both datasets with the six default engines (builds included), about 1 min more for the cold-cache tables, on the default 8 + 4 CPU budget |
| `ab` | 150,000 | 34,165 | 2-3 | projected from the measured phases: about 4 min (articles build 33 s for six engines, cells 1.5 min; places 1.5 min), about 6 min with the cold-cache tables |
| `full` (`--full`) | 218,058 | 236,262 (cities500) | 2-3 | projected: about 15-20 min including the builds and the cold-cache tables |

The times are those of the default budget (8 server and 4 load generator CPUs, below); a smaller budget runs the cells for as long but builds
and answers more slowly. `--serial` (one cluster, engines one after the other) took about twice as long with three engines in the measured comparison, and more with more engines.

Result directory (default `bench/out/run-<UTC time>`, gitignored; `bench/out/latest` links to the last one):

| file | content |
|---|---|
| `machine.txt` |  |
| `results.tsv` | chart input, one row per measurement (columns below) |
| `builds.tsv` | setup time, index build time, index size and table growth of every build |
| `tables.md` | the same numbers as Markdown: one row per engine with index size, build time and queries per second per class (articles) or p50 latency per class (places), then the ratios |
| `articles-<tier>/`, `places-<tier>/` | the raw tables of the run: `cells.tsv`, `builds.tsv`, `summary.tsv`, `machine.txt`; `*-cold` for `cold` (`summary.tsv` / `cold.tsv`) |

Read the numbers within one table of one run: the engines were measured one after another in the same session, so
and on whatever else the host runs. A cell the engine cannot run is `NA`, one whose statement failed or timed out is
`error`.

`results.tsv` columns (the first line is a `#` comment with the same list; tab separated, stable for chart generators):

| column | values |
|---|---|
| `dataset` | `articles`, `places` |
| `tier` | `quick`, `ab`, `full`, `quick-cold`, `ab-cold`, `full-cold` |
| `engine` | `sift`, `sift_nopos` (`positions = off`), `sift_pairs` (`phrase_index = pairs`), `pg_search`, `pg_textsearch`, `rum`; `-` for host rows |
| `class` | articles: `and or phrase mixed count filter join`; places: `p1 p2 p3 p4 w1 a2 a2s a3 x2`; `-` for build and host rows |
| `variant` | places workload `top boost filter count scan` (cold: the workload too); `emulation` on the build rows of the extra columns and indexes of an emulated workload; `-` otherwise |
| `metric` | `cpu_budget` (host row: `value` server CPUs and `min` load generator CPUs of an engine, `max` engines; `variant` the lists and the mode), `qps` (articles, 8 clients), `p50_ms` / `p95_ms` (places, 1 client), `cold_p50_ms` / `cold_max_ms` (tier `<tier>-cold`), `index_mib`, `build_s` (CREATE INDEX), `setup_s` (work before it, rum's generated column), `heap_growth_mib` (table growth of that setup), `load_1m` |
| `value`, `min`, `max` | median over the interleaved rounds and its range (`NA` / `error` as above); one build per engine for the build metrics |
| `stderr`, `n`, `values` | the standard error of the cell, `sd / sqrt(n)` over its `n` rounds (sample standard deviation; `NA` below two rounds and for single measurements: builds, cold-cache cells, host rows), and the per-round values, comma separated |
| `unit` |  |

Pinned inputs are all in [versions.env](versions.env): Wikipedia dump date and sha1, GeoNames date and sha256 (GeoNames
regenerates its files daily and keeps no archive, so a mismatch only warns; the places, and the numbers, then differ
slightly), pg_search 0.26.1, pg_textsearch 1.5.1 (tag v1.5.1 of timescale/pg_textsearch, commit and tarball sha256),
rum 1.3.15 (tag of postgrespro/rum, tarball sha256), PostgreSQL major. sift is built from the checkout
(`SIFT_SRC=head` builds the last commit instead of the working tree); its commit, and whether the tree had
uncommitted changes, is in `machine.txt`.

Adding an engine: one adapter file `engines/<name>.sh` (see Engine adapters below; for places it holds the `PLACES_*` and `P_*` statements too) and one line
in `engines.tsv` (id, chart and Markdown labels, color, charted yes/no, in the default run yes/no, reference engine), which sets the engine list of `bench`,
`steps/articles.sh`, `steps/places.sh`, `steps/tables.py` and `steps/charts.py` and the order of every table and chart. An engine that has to be built also needs a
`fetch_<name>` function in `steps/fetch.sh` (and its pinned version in `versions.env`).

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

## How a run works

`bench run` calls `steps/articles.sh` (articles) and `steps/places.sh` (places); `cold` runs the cold-cache variant of the same steps. A run is a series of phases, whose
wall times each result directory keeps in `phases.txt`:

1. **Lanes.** Every engine gets a lane (`steps/lanes.sh`): a cluster with its own data directory and port, pinned to an equal share of the server CPUs, its load
   generators pinned to an equal share of the load CPUs. All lanes are restored from the corpus dump and indexed at the same time, so the build phase takes as long as the slowest
   engine, not the sum. `--serial` uses one cluster with all the server CPUs instead.
2. **Cache of built lanes.** A built, stopped lane is copied to `$SIFT_BENCH_ROOT/cache/lanes/<key>`, the key being a hash of the adapter, the extension binary, the corpus,
   the harness, PostgreSQL and the CPU count of the lane; a rerun, a `regress` run, a report change rebuilds nothing that did not change (`--rebuild` builds again; `bench clean --all` removes the cache,
   which is as large as the clusters it holds).
3. **Rounds.** Round r runs every class on every engine at the same time, one cell group per class. In round r the engine k takes the CPUs of the lane (k + r) mod n (the servers are pinned
   again, the load generators follow), so no engine keeps a core that the host happens to load; with the interleaved rounds this also averages out the CPU an engine had. Every statement
   trace is the same for every engine (pgbench `--random-seed=7`). The first round has a warm-up cell per group, and the relations are prewarmed after the build.
4. **Stopping.** After `MINR` rounds (default 2) only the cells whose relative standard error is above `--rse` (default 5%) are measured again, up to `MAXR` (default 3); a cell that failed is not repeated.
5. **Cold cache** (`cold`): per engine and class, `NQ` (12) queries, each after the heap, TOAST and index files of the table were evicted from shared buffers and the page cache (per relation, not the whole cache),
   in all lanes at once.

It keeps only compact tables and removes its scratch clusters on exit (`KEEP=1` keeps them); the corpus dump
and the lane cache stay. `bench` holds `/tmp/sift-perf.lock` for the whole run, so two benchmarks never time at once (the lanes of one run overlap on purpose, on disjoint CPUs).

### CPU budget

The CPUs of a run are a parameter, printed in `machine.txt` (`cpu budget:`) and in the header of every table (`CPU budget: ...`): `--cpus LIST` for the servers (default: the 8 least busy
hardware threads, whole cores, picked by `steps/cpus.sh` from a one second sample of `/proc/stat`, so cores another job keeps busy are left alone) and `--load-cpus LIST` for the load generators (pgbench,
`xbench`; default 4 threads), two disjoint lists in `taskset` syntax (`0-3,8-11`). With n engines in concurrent mode each lane gets `floor(servers / n)` server CPUs
(at least one; engines share a CPU, with a warning, when there are more engines than CPUs) and `floor(load / n)` load CPUs; the parallel workers of a server (and an index build) and the pgbench
threads follow from the lists (`--workers` caps them) and each server runs in a cgroup scope with `CPUQuota` of 100% per CPU when a systemd user session exists. `--serial` gives
the one cluster all of `--cpus`; to measure with the CPUs of a concurrent lane, pass the share of one lane.

### Standard error

Every cell of `results.tsv`, `tables.md` and the charts is the median over its `n` rounds with a standard error, the sample standard deviation divided by `sqrt(n)`. The tables show `value ± stderr` (and the range of
the rounds where there is room), the charts draw one standard error as an error bar, and a relative standard error above 10% is flagged (`!` in the tables). With n = 2-3 the standard error is a rough
indication: it is itself estimated from two or three numbers, and it only covers the variation between the rounds, not the choice of the trace or the host. Read it with the per-round values (`values`).

### Regression check

`bench/bench regress --run` builds sift from this checkout and the reference `sift_ref` from the git ref `--ref` (default the tag `SIFT_REF` of `versions.env`, v1.1.0, under the extension name `pg_sift_ref`),
measures both on the articles and places with a reduced matrix (articles `and or phrase count filter join`; places classes `p1 p3 w1 a3` with the workloads `top boost`; `--engines sift,sift_ref,sift_pairs,sift_pairs_ref,sift_nopos`
and `--classes`, `--workloads` widen it) with 3-5 rounds, and compares each cell with its reference: queries per second, p50 latency, cold-cache p50, index size and CREATE INDEX time. A cell is a
REGRESSION when the new value is worse by more than 5% **and** by more than 2 combined standard errors (`sqrt(se_new^2 + se_ref^2)`); a cell that fails on the new engine only is one too, a cell that fails on both is an
ERROR. Builds and cold-cache cells are single measurements: 15% relative error is assumed for them (`--noise`). The exit status is 1 on a regression, 2 on an ERROR or when nothing could be compared. `regress`
The tail latency p95 is not compared: it is too noisy at 1 s cells for a 5% rule.
Per-tier cluster, port and dump:

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
| `articles.sh`, `places.sh` | the runs of articles and places (`run`, `cold`, `suite`, `workload`) |
| `lanes.sh`, `cpus.sh` | the lanes (a cluster per engine, the cache of built lanes, the rotation of the CPUs); the default CPU budget picked from the load of the host |
| `xlib.sh` | the external engines (Tantivy, Elasticsearch) in the lanes: `xbench` as the load generator, `x_*` of the adapters |
| `lib.sh` | shared settings and helpers: tiers, ports, engine roots, `q` (psql), `load_engine`, `rounds` / `rotated` / `rse_cells` (the rounds and their stopping rule), `evict_table` (cold cache), `locked` (perf lock), `limited` (a server pinned to its CPUs, a cgroup scope with a CPU quota when a systemd user session exists) |
| `machine.sh`, `tables.py` | the machine, CPU budget and engine description; raw run tables to `results.tsv` / `tables.md` (`report`) |
| `regress.py` | the regression check of `results.tsv` files (`regress`) |
| `charts.py` |  |
| `cluster.sh` | per-tier throwaway PG18 cluster: `TIER=F steps/cluster.sh init\ | start\ | stop\ | destroy` |
| `corpus.sh` | `load` a tier and cache its dump, `restore` it |
| `fetch.sh` | build sift and rum (PGXS), unpack pg_search, fetch Elasticsearch, build `xbench`: one `$SIFT_BENCH_ROOT/engines/<name>/{dl,root}` per engine, which the cluster searches for extensions |
| `build.sh` | sift CREATE INDEX per parallel worker count, appends to `build-workers.tsv` (`workers`) |
| `facets.sh`, `facets_es.py`, `parallel.sh` | standalone, run directly: facet counts of `sift.facets` against GROUP BY and Elasticsearch; speedup of the parallel scan by worker count on a restored `bench` database |
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
one small file.
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

Places adapters are part of the engine's file (`PLACES_SETUP`, `PLACES_INDEX`, the statements `P_match`, `P_top`, `P_boost`, `P_filter`, `P_count`, `P_scan`; `engines/_places.sh` has the shared fragments). sift, its variants (`pairs`, `positions = off`) and pg_search answer every workload natively. rum and pg_textsearch
filter by the prefix tsquery (`'gra':* & 'ho':*`, their native prefix match) for `count` and `scan`; they have no native equivalent of the ranked workloads, which are
emulations built next to the native index, in the same database, reported as the `emulation` build of the engine (variant `emulation`

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
readers run; WAL bytes from `pg_current_wal_lsn`; it changes the documents, so it runs last in a round and a run with it should not be compared with one without). Defaults of `workload`: 8 clients, 5 s warm-up, 30 s timed,
`statement_timeout` 120 s. `workload` and `suite` are `run --dataset articles` over the classes (and engines) they select; the engine
servers run in a `systemd-run --user --scope` with a CPU quota of 100% per CPU of their lane and `MemoryMax=32G` when a systemd user session exists.

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
parser, and pg_textsearch scores a query as a bag of terms; expect overlaps below 1 for pg_textsearch.

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
