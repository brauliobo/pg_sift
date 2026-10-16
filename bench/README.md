# sift benchmark and correctness harness

Two public datasets, measured on sift, sift with `phrase_index = pairs`, ParadeDB pg_search and rum with the same
rows, queries, clients, resource budget and cache state, the engines interleaved cell by cell so host load drifts hit
all of them alike. Everything runs through one entry point, `bench/bench`; `bench/bench --help` is the full reference.

| use-case | data | what it exercises |
|---|---|---|
| articles (full text) | Simple English Wikipedia | and / or / phrase / mixed top-10, `count(*)`, filtered, join |
| places (multi-field autocomplete) | GeoNames | 1-4 letter prefixes, whole words, 2-3 word ANDs, name-field boost, top-k, filtered, unordered scans, `count(*)` |

## Reproduce

```sh
bench/bench all                    # setup + run + report at the reduced default scale: about 10 minutes
bench/bench all --full             # the full-size run (the scale of the published numbers): about 1.5 hours
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

`all` is three commands that can be run on their own:

| command | what it does |
|---|---|
| `bench/bench setup` | checks the prerequisites; downloads, verifies and converts the datasets; builds this checkout's sift and the pinned rum (PGXS) and unpacks the pinned pg_search package into `$SIFT_BENCH_ROOT` |
| `bench/bench run --tier T` | machine description, then the articles and places A/B on a throwaway cluster into a result directory; an engine that is not installed is skipped with a message |
| `bench/bench report` | `results.tsv`, `builds.tsv` and `tables.md` from the result directory (`--docs` also writes `docs/results/<dataset>.tsv`) |

The other commands: `cold` (cache-evicted latency), `check oracle|rank ENGINE` (correctness against a tsvector
oracle / pgx-bm25), `suite` (the standard multi-engine suite: gin, rum, pg_textsearch, pgx-bm25, ParadeDB, sift,
Tantivy, Elasticsearch), `workload ENGINE` (one engine, timed pgbench workloads), `workers` (sift build time per
parallel worker count), `remote` (the same on an ssh host), `machine`, `clean [--all]`. Options: `--tier quick|ab|full` (`--full` = `--tier full`),
`--dataset articles|places|all`, `--engines sift,sift_pairs,pg_search,rum`, `--clients N`, `--duration S`,
`--rounds MIN-MAX`, `--out DIR`, `--no-build`, `--no-datasets`.

| tier | articles | places | rounds | expected run time of `all` |
|---|---|---|---|---|
| `quick` (default) | 25,000 | 34,165 (cities15000) | 2 | 8 min on a shared host from an empty `$SIFT_BENCH_ROOT` (3.4 min of downloads and builds, 4.8 min of measurements); about 5 min once the datasets are cached |
| `ab` | 150,000 | 34,165 | 3-6 | about 20 min on a shared host (articles 9 min, places 8 min, plus setup) |
| `full` (`--full`) | 218,058 | 236,262 (cities500) | 4-6 | about 1 h 15 min on a shared host (articles 23-31 min, places 36 min), about 1 h when quiet |

Result directory (default `bench/out/run-<UTC time>`, gitignored; `bench/out/latest` links to the last one):

| file | content |
|---|---|
| `machine.txt` | CPU, cores, SIMD, RAM, kernel, OS, PostgreSQL, engine versions and where they come from, sift commit, engine budget, load average at start, wall time |
| `results.tsv` | chart input, one row per measurement (columns below) |
| `builds.tsv` | setup time, index build time, index size and table growth of every build |
| `tables.md` | the same numbers as Markdown: one row per engine with index size, build time and queries per second per class (articles) or p50 latency per class (places), then the ratios |
| `articles-<tier>/`, `places-<tier>/` | the raw tables of the run: `cells.tsv` (every cell of every round with the load average at that moment), `builds.tsv`, `summary.tsv`, `machine.txt` |

Read the numbers within one table of one run: the engines were measured one after another in the same session, so
the ratios are meaningful when the load average (`cells.tsv`, `tables.md`) was steady; absolute values depend on the CPU
and on whatever else the host runs. A cell the engine cannot run is `NA`, one whose statement failed or timed out is
`error`. The committed results of the final session are in [docs/results/](../docs/results/), the discussion in
[docs/BENCHMARKS.md](../docs/BENCHMARKS.md).

`results.tsv` columns (the first line is a `#` comment with the same list; tab separated, stable for chart generators):

| column | values |
|---|---|
| `dataset` | `articles`, `places` |
| `tier` | `quick`, `ab`, `full` |
| `engine` | `sift`, `sift_pairs` (`phrase_index = pairs`), `pg_search`, `rum`; `-` for host rows |
| `class` | articles: `and or phrase mixed count filter join`; places: `p1 p2 p3 p4 w1 a2 a2s a3 x2`; `-` for build and host rows |
| `variant` | places workload `top boost filter count scan`; `-` otherwise |
| `metric` | `qps` (articles, 8 clients), `p50_ms` / `p95_ms` (places, 1 client), `index_mib`, `build_s` (CREATE INDEX), `setup_s` (work before it, rum's generated column), `heap_growth_mib` (table growth of that setup), `load_1m` |
| `value`, `min`, `max` | median over the interleaved rounds and its range (`NA` / `error` as above); one build per engine for the build metrics |
| `unit` | `queries/s`, `ms`, `MiB`, `s`, `load average` |

Pinned inputs are all in [versions.env](versions.env): Wikipedia dump date and sha1, GeoNames date and sha256 (GeoNames
regenerates its files daily and keeps no archive, so a mismatch only warns; the places, and the numbers, then differ
slightly), pg_search 0.26.1, rum 1.3.15 (tag of postgrespro/rum), PostgreSQL major. sift is built from the checkout
(`SIFT_SRC=head` builds the last commit instead of the working tree); its commit, and whether the tree had
uncommitted changes, is in `machine.txt`.

Adding an engine: write `engines/<name>.sh` (see Engine adapters below), add its id to `ALL_ENGINES` and the
`ab_engines` / `places_engines` cases of `bench`, a `fetch_<name>` function to `steps/fetch.sh` if it has to be built (and its
pinned version to `versions.env`), and its name to `ID`, `NAME` and `ORDER` in `steps/tables.py`. For places add the
extension, setup, index and statements to the `EXT`, `SETUP`, `INDEX` and `stmt` tables of `steps/places.sh`.

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
| `steps/` | internal helpers, called by `bench` (below); not meant to be run directly |
| `datasets/` | `fetch.sh`, `wiki.py`, `places.py`: download, verify, convert |
| `engines/*.sh` | one adapter per engine (see below) |
| `sql/` | schema, tier load + query trace, harness (rendering), oracle, places schema |
| `xbench/` | Rust load generator (HTTP keep-alive) and in-process Tantivy driver |

| `steps/` | role |
|---|---|
| `ab.sh`, `places.sh` | the interleaved A/B runs of articles and places (`run`, `cold`) |
| `lib.sh` | shared settings: tiers, ports, `q` (psql), `locked` (perf lock), `limited` (8 CPU / 32 GB budget, a cgroup scope when a systemd user session exists) |
| `machine.sh`, `tables.py` | the machine and engine description; raw run tables to `results.tsv` / `tables.md` (`report`) |
| `cluster.sh` | per-tier throwaway PG18 cluster: `TIER=F steps/cluster.sh init\|start\|stop\|destroy` |
| `corpus.sh` | `load` a tier and cache its dump, `restore` it |
| `fetch.sh` | build sift and rum (PGXS, `DESTDIR` roots), unpack pg_search and the other third-party engines under `$SIFT_BENCH_ROOT`, build `xbench` |
| `run.sh`, `xrun.sh` | one engine: timed index build + pgbench workloads (`BUILD_ONLY=1`: build and keep); non-Postgres engines driven by `xbench` (`workload`) |
| `quick.sh`, `report.sh` | the multi-engine standard suite and its tables (`suite`) |
| `build.sh` | sift CREATE INDEX per parallel worker count, appends to `build-workers.tsv` (`workers`) |
| `cold.sh` | cold-cache A/B of a boolean GUC (`GUC=sift.prefetch`) |
| `oracle.sh`, `rank_check.sh` | exact match-set / count(*) oracle (tsvector); soft BM25 check against pgx-bm25 (`check`) |
| `remote.sh` | sync, build, run and clean the whole harness on a remote host (`remote`) |

`bench` holds `docs(id, body, category_id, created, likes, url)`, `categories(id, name, region)` for
the join workload, `params` (tier constants) and `queries`: 1000 substrings of 2-15 consecutive plain
word tokens (PG `simple` parser) from random documents, each as `and`, `or` and `phrase` (3000
queries). The oracle adds a `not` form (all terms, last one negated).

## Engine adapters

Postgres adapters (`gin`, `rum`, `pg_textsearch`, `bm25` = pgx-bm25, `paradedb`, `sift`, `sift_pairs` = sift
with `phrase_index = pairs`) set `REQUIRES` (extension), `SETUP`, `INDEX` and `RENDER`, which defines two SQL
functions: `bench_pred(mode, terms)` (WHERE clause, NULL if unsupported) and `bench_rank(mode, terms)`
(ORDER BY, NULL if no top-k). `sql/harness.sql` composes every statement kind from them (top10, count,
match, filter, join), so a new engine is one small file. `sift` follows DESIGN.md (`body @@> 'a AND b'`,
`sift.score(ctid)`); the cluster loads it from `SIFT_ROOT` (default `$SIFT_BENCH_ROOT/sift-root`, built by
`setup`) and the harness skips an engine whose extension is absent. rum is a stored `tsvector` column
(`simple` configuration) with a `rum_tsvector_ops` index, queried with `@@` and ranked with `<=>`; on places it
lives in its own database so the column does not enlarge the other engines' table, and it has no per-field boost
(the `boost` cells are NA). ParadeDB is indexed as an application would: `bm25 (id, body, created, category_id)`
with the default tokenizer, and `edge_ngram(1, 15)` on the places name and all-fields text.

External adapters (`elasticsearch`, `tantivy`, and `meilisearch` / `typesense`, which are kept but not
compared: typo-tolerant, non-BM25 ranking) set `EXTERNAL`, `URL`, `XB_ARGS` and define `x_start`, `x_stop`,
`x_build`, `x_size`, `x_pid` plus SQL `x_req` / `x_write` that render requests from the same query trace.

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
ORACLE_ACCENTS=keep bench/bench check --tier ab oracle gin 200     # engines indexing raw 'simple' lexemes
MODES="phrase not" REBUILD=1 bench/bench check --tier ab oracle sift 50
bench/bench check --tier ab rank sift 200                          # top-10 overlap with pgx-bm25 per mode
```

`oracle` builds the engine index in `o_<engine>` (reused unless `REBUILD=1`) and an oracle `oracle_tsv`
table (`to_tsvector` with a `simple`-based configuration, `unaccent` when `ORACLE_ACCENTS=fold`,
`<lang>_stem` when `ORACLE_STEM=<lang>`). For each query it compares the engine's match set and `count(*)`
with the oracle and prints a summary plus every mismatch. Exit status is 1 on any mismatch; the report is
saved as `bench/out/<tier letter>/oracle-<engine>.txt`. Tokenization is the PG parser's: CJK runs are one token,
hyphenated words index the compound and its parts, `don't` is `don` + `t`.

## Cleanup

`bench/bench clean` destroys the clusters and removes the scratch under `$SIFT_BENCH_ROOT`; `run` does it for its own
cluster on exit. The datasets, dumps and engine builds stay (`clean --all`, or `rm -rf $SIFT_BENCH_ROOT`, removes everything).
