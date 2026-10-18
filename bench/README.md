# sift benchmark and correctness harness

This harness measures the **PostgreSQL** extension against other PostgreSQL engines (and Elasticsearch). The SQLite extension is measured against FTS5 by the scripts in [../sqlite/bench/](../sqlite/bench/) (results in [../sqlite/README.md](../sqlite/README.md#numbers-against-fts5)); its numbers are not comparable with these.

Two public datasets, measured on sift, its variant (`positions = off`, once the installed sift has that reloption), ParadeDB pg_search, pg_textsearch, rum,
pgx-bm25 and Elasticsearch with the same rows, queries, clients and cache state. Every engine has a cluster of its own (a lane), built one after the
other, and the engines are measured **one at a time**, each with every CPU of the host to itself, interleaved in rounds (below), so host
load drifts hit all of them alike. The CPUs are left alone: nothing is shared out or capped. Everything runs through one entry point, `bench/bench`; `bench/bench --help` is the full reference.
One suite serves every engine: `engines.tsv` (the registry) plus `engines/<id>.sh` (the adapter), one runner (`steps/articles.sh`, `steps/places.sh`),
one result format (`docs/results/*.tsv`, below) and one report path (`steps/tables.py`, `steps/charts.py`); gin, Tantivy and Elasticsearch
run through the same pipeline (Tantivy and Elasticsearch, which are servers or libraries outside Postgres, through `xbench`); gin and Tantivy are charted
only when the registry says so, Elasticsearch is in the default run.

| use-case | data | what it exercises |
|---|---|---|
| articles (full text) | Simple English Wikipedia | and / or / phrase / mixed top-10, `count(*)`, filtered, join |
| places (multi-field autocomplete) | GeoNames | 1-4 letter prefixes, whole words, 2-3 word ANDs, name-field boost, top-k, filtered, unordered scans, `count(*)` |

## Reproduce

```sh
bench/bench all                    # setup + run + report on the slim tier (the default): about 9 minutes of builds and measurements once set up
bench/bench all --full             # the full-size tier (218,058 articles, 236,262 places) with the cold-cache tables: about an hour
bench/bench all --docs             # the slim tier plus the cold-cache tables, then docs/results, docs/img and the generated blocks of README.md and docs/BENCHMARKS.md
bench/bench regress --run          # sift against a build of an older release (sift_ref, --ref, default v1.1.0): exit 1 on a regression, about 1 minute of measurements
bench/bench du                     # what the work directory and the store hold; bench/bench clean [--cache|--build|--all] frees it
```

Scale: the default is the slim tier (50,000 articles, 34,165 places of cities15000, two to three rounds of cells of at most 1 s). It keeps the ordering of the engines and
most of the ratios of the full tier (the table in docs/BENCHMARKS.md, generated, says which ones move and by how much) at a fraction of the time and disk. `--full` (or `--tier full`) uses all 218,058
articles and the 236,262 places of cities500 with 2-3 rounds, `--tier ab` the 150,000 article / 34,165 place tier in between. The first run also downloads and converts the datasets (about
3 minutes, once; the later setups find the stamps of the datasets and the keys of the engines up to date and do nothing). Ratios depend on the size of the corpus (docs/BENCHMARKS.md).

**One command regenerates every published number**: `bench/bench all --docs` measures the slim tier with every engine of `engines.tsv` that is installed (Elasticsearch included), and rewrites
`docs/results/*.tsv`, `docs/img/*.svg`, the slim section of `docs/BENCHMARKS.md` and the blocks of `README.md` (between the `<!-- bench:NAME:begin -->` / `<!-- bench:NAME:end -->` lines; `bench/bench readme` rewrites
only those two files from the committed rows, without measuring). Nothing in them is typed by hand: `steps/readme.py` takes every figure from the rows, the engine list from the registry, and the
wall time of the session and the size of the corpus from the `wall_s` and `corpus` rows. The full-size tables of docs/BENCHMARKS.md and its Correctness section are kept as dated history and are not
rewritten (`check` produces the correctness numbers).

Prerequisites: PostgreSQL 18 with its development packages and contrib (`pg_config`, PGXS, `pgbench`, `psql`, the
server tools, `pg_stat_statements`, `pg_prewarm`, `pg_buffercache`), `gcc` and `make`, `python3`, `curl`, `bzip2`,
`unzip`, `ar`, `flock`, `zstd`, network access, free space under `$BENCH_WORK` (default
`/tmp/sift-bench`; a fast disk or tmpfs): about 2 GB for the slim tier, about 5.5 GB for `--full`, at the peak of a run (Footprint, below). Nothing is installed system-wide and no sudo is needed. `setup`
checks all of this before it downloads anything.

Two engines are system packages that `setup` does not build: Elasticsearch (`pacman -S elasticsearch`: `/usr/share/elasticsearch`, `ES_HOME` overrides it) and pgx-bm25
(`pacman -S pgx-bm25`, or `yay -S pgx-bm25`: `bm25_native.control` in `pg_config --sharedir` and the library in the server's library directory, which the harness's clusters find as they are).
`setup` fails with that install hint when one of them is selected and missing, nothing is downloaded for them; on a host without the pgx-bm25 package `BM25_PKG=/path/to/pgx-bm25-<version>.pkg.tar.zst`
(unpacked into the work directory) or `BM25_SRC=/path/to/source.tar.gz` (built with PGXS) are the fallbacks. `machine.txt` says whether an extension is the system's or the work directory's.
Elasticsearch is started as an ordinary process of the user, not as the package's service: no sudo, nothing written to `/etc` or `/usr`, with a configuration directory of its own
(`elasticsearch.yml`, `jvm.options`, `log4j2.properties`, generated into `$BENCH_WORK/engines/elasticsearch/<tier>/conf`). The node has security and machine learning off, one shard, no replica
and a heap of the size of the Postgres engines' `shared_buffers`, on a free port of the tier (`ES_PORT` overrides it), with every CPU of the host like the Postgres servers. The
packaged version is recorded in the machine description of the results. The run stops the node and removes its data directory when it ends. `--engines elasticsearch` runs it alone.

`all` is three commands that can be run on their own (with `--docs`, and for the `ab` and `full` tiers, it also runs `cold`; with `--docs` it writes `docs/results`, `docs/img` and the generated
blocks of the docs):

| command | what it does |
|---|---|
| `bench/bench setup` | checks the prerequisites; downloads, verifies and converts the datasets; builds this checkout's sift, the pinned rum and pg_textsearch (PGXS) and unpacks the pinned pg_search package into `$BENCH_WORK`; pgx-bm25 and Elasticsearch are the installed system packages |
| `bench/bench run --tier T` | machine description, then the articles and places A/B on a throwaway cluster into a result directory; an engine that is not installed is skipped with a message |
| `bench/bench report` | `results.tsv`, `builds.tsv` and `tables.md` from the result directory (`--docs` also writes `docs/results/<dataset>.tsv`, redraws `docs/img` and, for the slim tier, rewrites the blocks of README.md and docs/BENCHMARKS.md) |

The other commands: `charts` (redraw the SVGs of `docs/img` from the committed `docs/results`), `readme` (rewrite the generated blocks of README.md and docs/BENCHMARKS.md from them), `du` (disk footprint), `clean [--cache|--build|--all]`, `cold` (cache-evicted latency of every class on every engine), `check oracle|rank [ENGINE]` (correctness against a tsvector
oracle / pgx-bm25, every engine of `--engines` when none is named, pgx-bm25 included; `check --dataset places` against the SQL oracle of the places queries), `suite` (the standard multi-engine suite: gin, rum, pg_textsearch, pgx-bm25, ParadeDB, sift,
Tantivy, Elasticsearch), `workload ENGINE` (one engine, timed pgbench workloads), `workers` (sift build time per
parallel worker count), `shred` (documents per second of the sift_col jsonb shredder and elements per second of its SIMD kernels, docs/COL-SHRED.md), `remote` (the same on an ssh host), `machine`. Options: `--tier slim|ab|full` (`--full` = `--tier full`; `quick` is the old name of `slim`), `--docs`, `--keep`,
`--dataset articles|places|all`, `--engines sift,sift_nopos,pg_search,pg_textsearch,rum,bm25,elasticsearch`, `--classes LIST` and `--workloads LIST` (restrict a run to some
query classes / places workloads), `--rebuild` (ignore the cache of built clusters), `--sample N` (statements per articles class; default 0 = the whole trace), `--clients N`, `--workers N` (pgbench threads and
the server's parallel workers; default 4 pgbench threads, one parallel worker per hardware thread and, for an index build, one fewer than the physical cores), `--max-time S` (the longest a cell is measured in a round, seconds; `--duration` is the same), `--min-time S` (the shortest, default 0.25), `--min-samples N`
(the fewest transactions), `--rse X` (the relative standard error a cell is measured to, and below which it is not measured again, default 0.05), `--rounds MIN-MAX` (default 2-3, regress 3-3;
more than 3 runs with a warning), `--ref REF` and `--run` (`regress`), `--out DIR`, `--no-build`, `--no-datasets`. `--keep` keeps the cache of built lanes after the session (Footprint, below).

| tier | articles | places | rounds | run time of `all` |
|---|---|---|---|---|
| `slim` (default) | 50,000 | 34,165 (cities15000) | 2-3 measured with the seven default engines on a host with a load average of 7-18, setup done: 551 s (9 min) for both datasets, of which 144 s restore and build (the lanes are built one after the other), 398 s cells; 769 s (13 min) with `--docs` (the cold-cache tables, 218 s more). The first setup also downloads and converts the datasets and builds the engines (about 3-5 min) |
| `ab` | 150,000 | 34,165 | 2-3 | projected from the measured phases: about 4 min (articles build 33 s for six engines, cells 1.5 min; places 1.5 min), about 6 min with the cold-cache tables |
| `full` (`--full`) | 218,058 | 236,262 (cities500) | 2-3 | about an hour, projected: the engines are measured one at a time, so the cells take the sum of the engines' times (the earlier method, all engines at once on shares of the CPUs, took 51 min for six engines with fixed-length cells, 18 min of cells that stop on their standard error) |

Why 50,000 articles: at 25,000 documents the ratios of the full tier were not kept for the engines whose cost grows with the corpus (pg_textsearch, rum); 50,000 was the smallest size whose
ratios were within noise of the full tier on the classes where sift leads, and 100,000 cost twice the time for no closer match. The generated fidelity table of docs/BENCHMARKS.md compares the
slim ratios with the full ones, class by class, from the committed rows.

The measurement takes as long as the sum of the engines' cells: seven engines one at a time take about seven times what one engine needs, which is the price of a method without CPU shares.

Result directory (default `bench/out/run-<UTC time>`, gitignored; `bench/out/latest` links to the last one):

| file | content |
|---|---|
| `machine.txt` | CPU, cores, SIMD, RAM, kernel, OS, PostgreSQL, engine versions and where they come from, sift commit, the method, load average at start, wall time |
| `results.tsv` | chart input, one row per measurement (columns below) |
| `builds.tsv` | setup time, index build time, index size and table growth of every build |
| `tables.md` | the same numbers as Markdown: one row per engine with index size, build time and queries per second per class (articles) or p50 latency per class (places), then the ratios |
| `articles-<tier>/`, `places-<tier>/` | the raw tables of the run: `cells.tsv` (every cell of every round with the load average at that moment), `builds.tsv`, `summary.tsv`, `machine.txt`; `*-cold` for `cold` (`summary.tsv` / `cold.tsv`) |

Read the numbers within one table of one run: the engines were measured one after another in the same session, so
the ratios are meaningful when the host stayed quiet (no `noisy.txt`, pressure in `machine.txt` and the log) and the load average (`cells.tsv`, `tables.md`) was steady; absolute values depend on the CPU
and on whatever else the host runs. A cell the engine cannot run is `NA`, one whose statement failed or timed out is
`error`. The committed results of the final session are in [docs/results/](../docs/results/), the discussion in
[docs/BENCHMARKS.md](../docs/BENCHMARKS.md).

`results.tsv` columns (the first line is a `#` comment with the same list; tab separated, stable for chart generators):

| column | values |
|---|---|
| `dataset` | `articles`, `places` |
| `tier` | `slim`, `ab`, `full`, `slim-cold`, `ab-cold`, `full-cold` (rows of the earlier sessions say `quick`) |
| `engine` | `sift`, `sift_nopos` (`positions = off`), `pg_search`, `pg_textsearch`, `rum`, `bm25` (pgx-bm25), `elasticsearch`; `-` for host rows |
| `class` | articles: `and or phrase mixed count filter join`; places: `p1 p2 p3 p4 w1 a2 a2s a3 x2`; `-` for build and host rows |
| `variant` | places workload `top boost filter count scan` (cold: the workload too); `emulation` on the build rows of the extra columns and indexes of an emulated workload; `forcemerge` on the Elasticsearch build rows of its force-merge (`build_s` the merge time, `index_mib` the size before it; the unmarked row is the bulk load and refresh and the size after the merge); `-` otherwise |
| `metric` | `qps` (articles, 8 clients), `p50_ms` / `p95_ms` (places, 1 client), `cold_p50_ms` / `cold_max_ms` (tier `<tier>-cold`), `index_mib`, `build_s` (CREATE INDEX; Elasticsearch: bulk load and refresh), `setup_s` (work before it, rum's generated column), `heap_growth_mib` (table growth of that setup), `load_1m`; host rows (`engine` `-`): `corpus` (documents of the tier), `wall_s` (seconds of a phase of the session: `variant` dump, build, cells, ...), `session` (the whole session) |
| `value`, `min`, `max` | median over the interleaved rounds and its range (`NA` / `error` as above); one build per engine for the build metrics |
| `stderr`, `n`, `values` | the standard error of the cell, the larger of `sd / sqrt(n)` over its `n` rounds and `sqrt(sum se^2) / n` of the rounds' own errors (below, Standard error; `NA` for single measurements: builds, cold-cache cells, host rows), and the per-round values, comma separated |
| `unit` | `queries/s`, `ms`, `MiB`, `s`, `load average` |

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
`$BENCH_STORE/download`, verifies it, writes `SHA256SUMS` next to it and converts it with the
`python3 -I` scripts in `bench/datasets/` (the downloads are only read, never executed). The converted files are gzip-compressed (`articles.tsv.gz`, parsed in parallel by `wiki.py`; the text is byte-identical to the
uncompressed conversion), a stamp next to each (`articles.stamp`, `places.stamp`: the pinned inputs and the converter) makes the next setup skip them when nothing changed, and the raw downloads are
removed after the conversion (`KEEP_DOWNLOADS=1` keeps them, `REFRESH_DATASETS=1` converts again).

| dataset | source | license | checksum |
|---|---|---|---|
| Simple English Wikipedia, `simplewiki-20261001-pages-articles.xml.bz2` (342 MB) | https://dumps.wikimedia.org/simplewiki/20261001/ | CC BY-SA 4.0 | sha1 `f55881650e447c39e8bdeba4b1bb8d5576342d4b`, equal to the published `simplewiki-20261001-sha1sums.txt` |
| GeoNames `cities500.zip` (236,262 places), `cities15000.zip` (34,165), `admin1CodesASCII.txt`, `countryInfo.txt`, `featureCodes_en.txt` | https://download.geonames.org/export/dump/ (files of 2026-10-16) | CC BY 4.0 | sha256 `ca136e8a...f626e05` (cities500), `da8eca61...2cf2069` (cities15000), pinned in full in `versions.env`; all files in the `SHA256SUMS` written by the script |

An article is its title plus the cleaned wikitext of one main-namespace, non-redirect page of at least
200 characters (218,058 articles, 1.4 KB on average). A place is name, alternate names, region,
country, country code, feature name and population; the typed queries (300 per class) are sampled from
the places themselves with a fixed seed. Without network access, nothing else in the harness changes:
put any `articles.tsv` or `articles.tsv.gz` (COPY text of `id, title + body, category_id, created, likes, url`) in
`$BENCH_STORE/data`.

## Results

Tables, build times, sizes, cold-cache runs and the host load of every round are in [docs/BENCHMARKS.md](../docs/BENCHMARKS.md),
the numbers of the final session in [docs/results/](../docs/results/).

## How a run works

`bench run` calls `steps/articles.sh` (articles) and `steps/places.sh` (places); `cold` runs the cold-cache variant of the same steps. A run is a series of phases, whose
wall times each result directory keeps in `phases.txt`:

1. **Lanes.** Every engine gets a lane (`steps/lanes.sh`): a cluster with its own data directory and port (Elasticsearch and Tantivy a server or process of their own next to it). The lanes
   are restored from the corpus dump and indexed one after the other, so that a build is timed alone (and warmed, for the engines outside Postgres), and then all stay up and idle. Whichever
   engine is being measured has all the CPUs of the host to itself, the other servers idle.
2. **Cache of built lanes.** A built, stopped lane is saved as a zstd-compressed tar of its data directory (`$BENCH_STORE/cache/lanes/<key>/data.tar.zst`, with the `bench` template database dropped and the WAL cut to the checkpoint segment) when the session writes the cache
   (`all` with cold-cache tables, or `--keep`), the key being a hash of the adapter, the extension binary, the corpus,
   the harness and PostgreSQL; a rerun, a `regress` run, the cold-cache session of `all`, a report change rebuilds nothing that did not change (`--rebuild` builds again; `bench clean --all`
   removes the cache). After a successful session the cache is removed, since the next build changes the extension and so every key, unless `--keep` is given: then the least recently used lanes
   beyond `BENCH_CACHE_MAX` MiB (default 4096) go.
3. **Rounds.** Round r runs every class on every engine, **one engine at a time**: for a class, each engine in turn runs its cell alone (the others are idle), and the order of the engines
   rotates every round (engine k goes first in round k), so no engine always runs first or last, and a drift of the host load (the rest of the machine is not controlled; each batch waits for a quiet host, see below, and the load average is
   recorded with every cell) hits the engines alike over the rounds. Every engine has the same client counts (articles 8, places 1) and every statement trace is the same (pgbench `--random-seed=7`).
   The relations are prewarmed after the build; a cell has no warm-up of its own, except for the engines outside Postgres, whose JVM and caches are warmed for a few seconds per class (`XWARM`) in the
   build phase and again before the first round. Why one engine at a time: engines that run side by side without CPU shares compete for cores, memory bandwidth and the turbo budget, so the
   result of one depends on what the others do at that moment; a machine divided into equal slices is smaller than any server wants (an index build or a parallel scan uses more than one core). One at a time every engine
   sees the same whole machine, and the cost is only the wall time.
4. **A cell** (a class on an engine in a round) is measured by `steps/cell.py` until it is precise enough, not for a fixed time: pgbench runs in chunks that log every transaction (`-l`), and the cell stops
   when it has `--min-time` seconds (0.25) and `--min-samples` transactions (articles: 1 per client, places: 3) and the relative standard error of its value is at most `--rse` (5%), or when
   `--max-time` seconds are measured (slim 1 s; full: articles 2 s, places 1 s), whichever comes first. A fast cell (microseconds per query) is done in the minimum time; a slow one, where fewer than
   `1 / rse^2` = 400 queries fit in `--max-time`, is measured for `--max-time` in one go and ends with the statements still running at that moment (which dominates the cost of a cell of seconds per query), and
   its standard error says how precise it is. External engines (Tantivy, Elasticsearch) and the articles `update` class keep a fixed `--max-time` (whole seconds).
5. **Stopping the rounds.** After `MINR` rounds (default 2) only the cells whose relative standard error is above `--rse` are measured again, up to `MAXR` (default 3, the most there is by default), except the cells whose
   own error per round is so large that three rounds cannot bring it to `--rse` (a cell limited by time): another round would cost as much and change little. A cell that failed is not repeated.
6. **Cold cache** (`cold`): per engine and class, up to `NQ` (12) queries, each after the heap, TOAST and index files of the table were evicted from shared buffers and the page cache (per relation, not the whole cache; a data
   directory on tmpfs has no other page cache to evict). At least 5 queries are run, then only while the cell has used less than 3 s (`COLD_MIN`, `COLD_TIME`): a query of seconds is not run twelve times.

It keeps only compact tables (`cells.tsv` with the load average of every cell, `builds.tsv`, `machine.txt`, `phases.txt`) and removes its scratch clusters on exit (`KEEP=1` keeps them); the corpus dump
and the lane cache stay. `bench` holds `/tmp/sift-perf.lock` for the whole run, so two benchmarks never time at once (its cells never run next to another benchmark).

### Host and CPUs

The suite leaves the CPUs alone. An engine under measurement can use all the hardware threads of the host (a Postgres server's parallel workers are one per hardware thread, an index build one fewer than the physical cores, so that the engines that cut the index into a segment per worker do not get 15 segments for a small table;
`--workers` caps both), and the load generators (pgbench with 4 threads, `xbench`) run next to it on the same CPUs, as any client of the machine would, with the same client counts for every engine.
What the method relies on is the one-at-a-time order above, the rotation of the order, the repetition (2-3 rounds with a standard error per cell) and the check of the host's pressure before every timed batch (below); the load average is still printed with every table and
recorded with every cell, for the older runs. A busy host shows up as wide standard errors and as a drift between rounds; the numbers of a session are for comparing the engines of that
session with each other, not with another session or machine.

**Quiet host (`scripts/quiet.sh`).** The load average counts runnable tasks, not the time anything waited, so a timed step does not use it to judge the host; it uses Linux pressure stall information instead
(`/proc/pressure/cpu` and `io`, the `some` line). Before each timed batch (a class of cells, a pgbench round, a timed query, a build) `quiet_wait` blocks until both resources are below 10% in the
10 s average (`avg10`) for two samples 2 s apart; after `SIFT_QUIET_TIMEOUT` seconds (default 60) the batch runs anyway and is marked **NOISY** (in the log, in `noisy.txt` of the table directory, in the table or the footer of the
step), never silently. Headers and footers print `pressure cpu=X%/Y% io=X%/Y% (some avg10/avg60)`. Instruction-count steps (`cpu_gate.sh`, the cycles of `ab.sh run`) print it and never wait. Override with the environment:
`SIFT_QUIET_MAX` (percent, default 10), `SIFT_QUIET_TIMEOUT` (seconds), `SIFT_QUIET_WINDOW` (`avg10` or `avg60`); the same options exist on the command (`scripts/quiet.sh wait --max 20 --timeout 300`, `report`). On a kernel without PSI it says
that it cannot measure (exit status 3) and every batch is NOISY.

### Standard error

Every cell of `results.tsv`, `tables.md` and the charts is the median over its `n` rounds with a standard error. Each round has an error of its own (`se` in `cells.tsv`): for the queries per second of the
articles, batch means (the transactions of each client are cut into consecutive batches of at least 5, the rate of a batch is the clients' transactions over their pooled seconds, and the error is the standard
deviation of the batch rates over the square root of their number, with at least 4 batches; with fewer, the standard error of the mean latency, sd / (mean sqrt(n)), whichever is larger); for the p50 of the places,
distribution free: half the distance between the order statistics at n/2 - sqrt(n)/2 and n/2 + sqrt(n)/2. The standard error of the cell is the larger of the sample standard deviation of the rounds over `sqrt(n)` and
the error that the rounds' own errors leave, `sqrt(sum se^2) / n`. Because the within-round error also sees which statements of the trace a cell happened to run (the rounds replay the same seeded statements, so the spread
of the rounds alone does not), the errors of a cell with heavy-tailed statements are larger than the spread of its rounds says. The tables show `value ± stderr` (and the range of the rounds where there is room), the charts
draw one standard error as an error bar, and a relative standard error above 10% is flagged (`!` in the tables). With n = 2-3 the spread of the rounds is itself estimated from two or three numbers and does not cover the
host: read it with the per-round values (`values`).

### Regression check

`bench/bench regress --run` builds sift from this checkout and the reference `sift_ref` from the git ref `--ref` (default the tag `SIFT_REF` of `versions.env`, v1.1.0, under the extension name `pg_sift_ref`),
measures both on the articles and places with a reduced matrix (articles `and or phrase count filter join`; places classes `p1 p3 w1 a3` with the workloads `top boost`; `--engines sift,sift_ref,sift_nopos`
and `--classes`, `--workloads` widen it) with 3 rounds, and compares each cell with its reference: queries per second, p50 latency, cold-cache p50, index size and CREATE INDEX time. A cell is a
REGRESSION when the new value is worse by more than 5% **and** by more than 2 combined standard errors (`sqrt(se_new^2 + se_ref^2)`); a cell that fails on the new engine only is one too, a cell that fails on both is an
ERROR. Builds and cold-cache cells are single measurements: 15% relative error is assumed for them (`--noise`). The exit status is 1 on a regression, 2 on an ERROR or when nothing could be compared. `regress`
without `--run` compares result directories or `results.tsv` / `docs/results/*.tsv` files that exist. The tail latency p95 is not compared: it is too noisy at 1 s cells for a 5% rule.
Per-tier cluster, port and dump:

| tier letter | articles | port | dump |
|---|---:|---:|---|
| Q (`slim` 50,000, `ab` 150,000: `Q_DOCS`) | 50,000 / 150,000 | 56040 | `$BENCH_STORE/cache/sift-Q-<docs>.dump` |
| F (`full`) | 218,058 | 56050 | `$BENCH_STORE/cache/sift-F-ALL.dump` |

### Gate on a busy host (`steps/cpu_gate.sh`)

Throughput and latency say nothing on a host whose cpu pressure stays above the 10% of `scripts/quiet.sh`, so this gate compares builds by what the server executes: user-space instructions (and cycles, as secondary
evidence, because the other load contends for the caches) that `perf stat -e instructions:u,cycles:u -p POSTMASTER` counts for the processes the postmaster forks during one operation. That is the
backend plus the maintenance worker and parallel workers, summed; a write's window ends when the worker has gone. It is standalone, never part of `make check` or `scripts/ci.sh`, needs `perf`
(`perf_event_paranoid` <= 2) and one checkout per build with `scripts/pgtest.sh` (`git worktree add --detach /tmp/x REF`; build everything the same way, the Makefile links without LTO by default):

```sh
bench/steps/cpu_gate.sh --build before=/tmp/x-before --build after=/tmp/x-after [--build head=/tmp/x-head] \
    [--reps 7] [--max-reps 25] [--se 1] [--budget 3600] [--cells REGEX] [--rows 20000] [--inserts 10000] [--queries 100] [--raw FILE] \
    [--index-opts LABEL='vacuum_truncate = false'] [--guc LABEL=sift.maintenance_cost_delay=0]
```

The first `--build` is the baseline. `--index-opts` gives the indexes of one build a reloption list and `--guc` a setting of its cluster, so the same checkout under two labels compares two configurations of one build (`merge_fanin = 32` against the default). Each build gets a production cluster under `/tmp/sift-gate-LABEL` (ports `SIFT_PORT`, default 56120, and up); nothing else is touched, and clusters and scratch files are removed
at the end. The data are deterministic (`setseed`), made once per build into template databases and copied per run, and the heap's md5 must be equal on all builds. Cells: `insert1 bulk copy update
delete promote merge create` (writes on a fresh copy), `vacuum_backlog vacuum_dead vacuum_clean vacuum_nofree` (the last is the second VACUUM in a row, which costs less than a session, so it is compared raw, session included: what the check for a free tail to give back costs), and the query classes `count rank and or phrase` on 61 segments (`idle60_*`), on one (`idle1_*`) and
on the index after VACUUM and more rows (`vac_*`), plus `session` (connect, load sift, one query on an empty index), whose cost is subtracted from every other cell, which is divided by its operations.
Per round every cell runs on every build one after the other, the first of the round rotating (ABAB, BABA). The ratio to the baseline is the geometric mean of the per-round ratios of instructions per
operation with a 95% interval (t distribution); `--reps` rounds run for every cell, then only the cells whose standard error of the ratio is above `--se` percent get more, up to `--max-reps` rounds or
`--budget` seconds. A cell regresses when its whole interval is above 1.02; the table also gives the ratio of cycles, WAL bytes, index size, segments left and documents not promoted. A cell whose end
state differs between builds (`design`: VACUUM merging further, the worker promoting the tail of a bulk load) is work done by design: its ratio is reported apart, with the segment counts, and does not gate.
Run the gate with the same checkout under two labels to see the noise floor: at a load average of 24 the instruction ratios were 1.000 with intervals within 0.2%, the cycle ratios 0.89-1.04. `--raw FILE` keeps every measurement (`cell build round metric value`). The run time is printed at the end: the default sizes take about 1 minute per round for three builds on a 16-core host with a load average of 20, so 7 rounds are about 10 minutes and the cap of 25 rounds about 30.

## Layout

| path | role |
|---|---|
| `bench` | the entry point: `bench/bench --help` |
| `versions.env` | every pinned version, date and checksum |
| `engines.tsv` | the A/B engines in table order: id, chart and Markdown labels, chart color |
| `steps/` | internal helpers, called by `bench` (below); not meant to be run directly |
| `datasets/` | `fetch.sh`, `wiki.py`, `places.py`: download, verify, convert |
| `engines/*.sh` | one adapter per engine (see below) |
| `sql/` | schema, tier load + query trace, harness (rendering), oracle, places schema; `sql/analytics/`: the analytics corpus (generator, layouts, queries); `sql/store/`: the online-store data generator, the layout of each system and the ten queries |
| `xbench/` | Rust load generator (HTTP keep-alive) and in-process Tantivy driver |

| `steps/` | role |
|---|---|
| `articles.sh`, `places.sh` | the runs of articles and places (`run`, `cold`, `suite`, `workload`) |
| `lanes.sh` | the lanes (a cluster per engine, the compressed cache of built lanes) |
| `footprint.sh` | `du`, `clean`, the peak footprint of a session and the pruning of the cache of lanes |
| `readme.py` | the generated blocks of README.md and docs/BENCHMARKS.md (`readme`) |
| `xlib.sh` | the external engines (Tantivy, Elasticsearch) in the lanes: `xbench` as the load generator, `x_*` of the adapters |
| `cell.py` | one measured cell: pgbench in chunks until the relative standard error is small enough or the time is up (batch means / order statistics), see How a run works |
| `stats.py` | the statistics of repeated measurements (median, sd, standard error), imported by `cell.py`, `tables.py` and `analytics.py` |
| `perf.sh` | `perf_counts PID [-- CMD]`: user-space cycles, instructions and CPU time of a process by `perf stat`, one line; sourced by `ab.sh` and `bench/cpu-query.sh`, run by `analytics.py` |
| `lib.sh` | shared settings and helpers: tiers, ports, engine roots, `q` (psql), `load_engine`, `rounds` / `rotated` / `rse_cells` (the rounds and their stopping rule), `cold_table` / `cold_ms` / `cold_cell` (cold cache), `locked` (perf lock) |
| `machine.sh`, `tables.py` | the machine and engine description; raw run tables to `results.tsv` / `tables.md` (`report`) |
| `regress.py` | the regression check of `results.tsv` files (`regress`) |
| `charts.py` | SVG charts of `docs/results/*.tsv` into `docs/img`, all five engines in every chart, an NA as a gap (`charts`; stdlib only, same tables give the same bytes) |
| `cluster.sh` | per-tier throwaway PG18 cluster: `TIER=F steps/cluster.sh init\|start\|stop\|destroy` |
| `corpus.sh` | `load` a tier and cache its dump, `restore` it |
| `fetch.sh` | build sift and rum (PGXS), unpack pg_search, check the Elasticsearch package, build `xbench` (in a scratch target; only the stripped binary stays): one `$BENCH_WORK/engines/<name>/{dl,root}` per engine, which the cluster searches for extensions |
| `build.sh` | sift CREATE INDEX per parallel worker count, appends to `build-workers.tsv` (`workers`) |
| `ab.sh` | standalone, run directly: interleaved A/B of sift builds (git refs or the working tree) on the slim corpus, one backend's user cycles and instructions per transaction (or qps at N clients), mean and standard error; `prepare`, `prepare_places` (adapter index plus a btree on `country_code`), `shapes` (`sql/ab_shapes.sql`, the filter shapes of the plan-choice tests), `build`, `run`, `clients` (header of the script; `run bench/sql/ab_and_filter_prep.pgb ...` is a prepared rare AND query with a selective filter, the shape a generic plan with the pushdown serves worse than main's post-filter plan) |
| `cpu_gate.sh` | standalone, run directly: gate of two or more sift builds by user instructions and cycles per operation of the server processes (backend plus maintenance worker), per cell ratio to the baseline with a 95% interval, interleaved rounds extended until the standard error is small; holds up on a busy host (section Gate on a busy host) |
| `shred.sh` | the shredder's throughput on a throwaway cluster of `scripts/pgtest.sh` (`shred`; `sql/shred.sql`; `SHRED_DOCS`, `SHRED_ROUNDS`) |
| `facets.sh`, `facets_es.py`, `parallel.sh` | standalone, run directly: facet counts of `sift.facets` against GROUP BY and Elasticsearch (DESIGN.md (l), its own scratch cluster under `$FACET_TMP`); speedup of the parallel scan by worker count on a restored `bench` database |
| `oracle.sh`, `rank_check.sh` | exact match-set / count(*) oracle (tsvector); soft BM25 check against pgx-bm25 (`check`) |
| `remote.sh` | sync, build, run and clean the whole harness on a remote host (`remote`) |
| `final.sh`, `gap.sh`, `gap.py` | `bench/analytics --final` (the one command for the published analytics numbers, with `--dry-run`); the pg_sift profile behind docs/BENCH-GAP.md |
| `store.sh`, `store.py` | the online-store benchmark against Elasticsearch and ClickHouse (`store`): docker containers, seeded data, load, warm and cold runs, updates, the Markdown tables of README.md and docs/BENCHMARKS.md |
| `analytics.sh`, `analytics.py` | the analytics benchmark of the sift_col index (`analytics`): cluster, corpus, ClickHouse and ClickBench setup; measurement of CPU time and instructions per query, result comparison, report |

`bench` holds `docs(id, body, category_id, created, likes, url)`, `categories(id, name, region)` for
the join workload, `params` (tier constants) and `queries`: 1000 substrings of 2-15 consecutive plain
word tokens (PG `simple` parser) from random documents, each as `and`, `or` and `phrase` (3000
queries). The oracle adds a `not` form (all terms, last one negated).

## Engine adapters

Postgres adapters (`gin`, `rum`, `pg_textsearch`, `bm25` = pgx-bm25, `pg_search`, `sift`, `sift_nopos` = sift with `positions = off`) set `REQUIRES` (extension), `PROBE` (optional: a statement over a temp table `probe (t text)` that must work for the engine to run, e.g. a reloption of a newer sift build; the engine is skipped with a message otherwise), `SETUP`, `INDEX`, and `PRED` and `RANK`: SQL expressions over
`mode` and `terms` rendering the WHERE condition and the ORDER BY of a query, NULL if the engine cannot answer it
(`HELPERS`: SQL they use). `steps/lib.sh` turns them into `bench_pred(mode, terms)` and `bench_rank(mode, terms)` and
`sql/harness.sql` composes every statement kind from them (top10, count, match, filter, join), so a new engine is
one small file. `sift` follows DESIGN.md (`body @@> 'a AND b'`,
`sift.score(ctid)`); the cluster loads it from the engine roots (`setup` builds sift into
`$BENCH_WORK/engines/sift/root`) and the harness skips an engine whose extension is absent. rum is a stored `tsvector` column
(`simple` configuration) with a `rum_tsvector_ops` index, queried with `@@` and ranked with `<=>`; on places it
lives in its own database so the column does not enlarge the other engines' table, and it has no per-field boost
(its `boost` cells are an emulation, below). pg_textsearch is a `bm25` index with `text_config = 'simple'` (the analysis of gin and rum); see its adapter for the
queries: `@@ tsquery` filters through the index, `ORDER BY body <@> to_bm25query(...)` ranks, a ranked disjunction needs
no filter, AND and phrase top-k filter through the index in a derived table whose matches are scored standalone (the plain form is planned as a
sequential scan, and a bare `ORDER BY body <@> ...` over the `@@` scan does not rank), and the database runs with `enable_seqscan = off` as
the extension's README suggests. ParadeDB is indexed as an application would: `bm25 (id, body, created, category_id)`
with the default tokenizer, and `edge_ngram(1, 15)` on the places name and all-fields text.

Places adapters are part of the engine's file (`PLACES_SETUP`, `PLACES_INDEX`, the statements `P_match`, `P_top`, `P_boost`, `P_filter`, `P_count`, `P_scan`; `engines/_places.sh` has the shared fragments). sift, its variant (`positions = off`) and pg_search answer every workload natively. rum and pg_textsearch
filter by the prefix tsquery (`'gra':* & 'ho':*`, their native prefix match) for `count` and `scan`; they have no native equivalent of the ranked workloads, which are
emulations built next to the native index, in the same database, reported as the `emulation` build of the engine (variant `emulation`
in `results.tsv`) and described in docs/BENCHMARKS.md:

| engine | workload | emulation |
|---|---|---|
| pg_textsearch | top, filter | `bm25` indexes on stored columns holding the edge n-grams (1-15 letters) of every word, pg_search's analysis, so a typed prefix is a whole term; the matches `ng_text @@ plainto_tsquery(typed)` (a derived table) are scored standalone with the distinct n-grams of the typed words |
| pg_textsearch | boost | the same matches ordered by the all-fields score plus 3 x the score of a second index on the name field (a weighted sum of two bm25 scores) |
| rum | boost | a second stored tsvector with the name words at weight A (1.0) and the other fields at B (0.4), indexed with `rum_tsvector_ops` and ranked with `<=>`; the weights are rum's, so the name counts 2.5 times, not 3 |
| sift positions off | all | the variant of the index is not needed by prefixes (never a phrase); it runs to show its cost on a places workload |

Each pg_textsearch and rum database keeps the tables of both: `count` and `scan` run on the native index and are not changed by the emulation.

External adapters (`elasticsearch`, `tantivy`) set `EXTERNAL`, `URL`, `XB_ARGS` and define `x_start`, `x_stop`,
`x_build`, `x_size`, `x_pid` plus SQL `x_req` / `x_write` (in `RENDER`) that render requests from the same query trace (and, for places, `PLACES_FLAVOR` and `PLACES_RENDER`);
`JOINS=yes` says that the engine answers the articles `join` class (Elasticsearch does it with a denormalized `region` field of the document, `steps/xlib.sh` exports it from `categories`).
`steps/xlib.sh` is the glue: it exports the tier's documents once as NDJSON (`$BENCH_STORE/data/<tier letter>`), renders the request files of every class from the
same trace as the pgbench scripts (`x_req`), measures the cells with `steps/cell.py --xbench` (the same stopping rule and standard errors as pgbench) and runs the cold-cache cells. Elasticsearch is an HTTP engine
(`EXTERNAL=http`): `xbench` keeps its connections open, takes the latency from the client and the ids and counts from the response (`--ids-ptr`, `--count-ptr`). Its adapter documents its configuration and the
emulations in its header: one shard, BM25 with k1 = 1.2 and b = 0.75 set explicitly, a tokenizer on runs of letters and digits with lowercase and ASCII folding,
edge n-grams (1-15) for places, caches of results off, a force-merge to one segment after the bulk load, the client's time for the cold cells (the index is closed and opened again). Its heap is the size of
the `shared_buffers` of the Postgres engines.

## Workloads (`run`, `workload`)

`and`, `or`, `phrase`, `mixed` (all three, top-10), `count` (count(*) of all three), `filter` (top-10 +
`created >= cutoff`, ~2% of rows), `join` (top-10 joined with `categories`, region filter), `update`
(`workload` only: writers at `UPDATE_RATE`=1000 ops/s: 70% update, 15% insert, 15% delete, while `mixed`
readers run; WAL bytes from `pg_current_wal_lsn`; it changes the documents, so it runs last in a round and a run with it should not be compared with one without). Defaults of `workload`: 8 clients, 5 s warm-up, 30 s timed,
`statement_timeout` 120 s. `workload` and `suite` are `run --dataset articles` over the classes (and engines) they select.

Places classes are what a user has typed: `p1`-`p4` a name's first word cut to 1-4 letters, `w1` a whole
word, `a2`/`a2s` two words (last cut to 2 / 1 letters), `a3` name, region and country word, `x2` name word
plus country word; workloads `top` (top-20), `boost` (name matches x3), `filter` (`country_code = 'US'`),
`count`, `scan` (unordered, `population >= 100000`).

## Analytics (sift_col)

`bench/bench analytics run` measures aggregate queries over jsonb (objects with dynamic keys, arrays of objects) instead of text search:
a deterministic orders corpus (1.2M orders by default, `--scale 10` for 12M), the queries S1-S7 on the store sets K10, K400 and K4000 and the
whole-table scans G1-G8, as the CPU time and instructions of one backend with the baseline plan against the sift_col plan, optionally
against flat tables and a throwaway ClickHouse; `analytics clickbench` runs the 43 queries of ClickBench. It uses a cluster of its own
under `SIFT_TMP` (port `SIFT_PORT`) and does not touch the tiers above. docs/perf/ANALYTICS.md has the method, the options and the numbers.

## Analytics for everyone: the online store (`bench/bench store`)

The simple case behind the Analytics sections of README.md and docs/BENCHMARKS.md: a table of orders (`id, customer_id, country, category, status,
amount, quantity, created_at, title`), ten questions anyone can ask of it (an order by id, orders per country, revenue per month, the top 10 categories,
an average, distinct customers per month, a filtered count, and three after a full-text search: a count, counts per category, the 10 newest), and four
servers: PostgreSQL (heap, btree and GIN indexes: what most teams run), PostgreSQL with pg_sift (`sift_col` + `sift`, and `sift_table`), Elasticsearch and
ClickHouse. It extends this harness rather than replacing it: `bench` is the entry point and holds the perf lock for the timed runs, `steps/lib.sh` gives the
helpers, `steps/stats.py` the median and spread, and `docs/results/store.json` is in the layout of `docs/results/analytics.json`.

```sh
bench/bench store all --docs            # 1M and 10M orders: containers, data, load, warm and cold runs, updates; then docs/results/store.json, README.md, docs/BENCHMARKS.md
bench/bench store all --sizes 1000000 --cold-runs 0 --update-sizes none      # one size, warm runs only
bench/bench store report                # the tables again from docs/results/store.json, without measuring
bench/analytics                         # the quick run: 1M orders, 3 runs, the same equality checks, about 2 minutes (bench/out/latest/store.json)
bench/bench store compare A.json B.json # two runs side by side: every warm median and the ratio (changes above 10% marked)
bench/bench store gap                   # pg_sift against ClickHouse and Elasticsearch per question, with the ratios (docs/BENCH-GAP.md)
bench/bench store all --sizes 1000000 --compression --docs   # the pg_sift `compression` variants as lanes and every system's size with its other codec settings
bench/steps/gap.sh                      # the pg_sift profile (planning, instructions, symbols) on a private cluster under a memory cap
bench/analytics --final --dry-run       # the plan, time, disk and memory of the one command that regenerates every published analytics number (without --dry-run: runs it)
bench/bench store clean                 # containers, volumes, the image, the CSV
```

Needs docker (it pulls the images pinned in `versions.env` and builds the PostgreSQL image from this checkout's tracked files, as `scripts/replication.sh --container`
does), `psql`, `python3`; nothing is installed on the host and every container and anonymous volume is removed at the end (`store all` removes them on any exit).
What it does, in order, for each size:

1. Four containers, one per server (the two pg_sift layouts share one PostgreSQL container, in two databases), each `--cpus` (4) and `--memory` (8g) with no swap, on
   ports `STORE_PORT_BASE` to `+3` of 127.0.0.1 (default 56280); the servers not under test sit idle. The PostgreSQL containers are the same image and settings
   (`shared_buffers` 2GB, `max_parallel_workers_per_gather` as many as CPUs, `jit` off); Elasticsearch has a heap of half the memory; ClickHouse sizes itself from the
   container limit.
2. The data: `sql/store/gen.sql` writes the rows with hashes of (id, draw number), so a size gives the same CSV every run (its sha256 is in the results); the same
   file is loaded into every system with its own layout (`sql/store/pg.sql`, `sift.sql`, `sift_table.sql`, `ch.sql`, `es.json`), and the load time and the size on disk
   are recorded. Elasticsearch is merged to one segment and ClickHouse optimized after the load, PostgreSQL vacuumed and analyzed.
3. Warm runs (`steps/store.py run`): for each query one untimed run per engine, whose result is compared with PostgreSQL's (rows exactly, numbers within 1e-9
   relative, Elasticsearch's approximate distinct count within 2%; a difference stops the run with exit status 3), two more untimed runs, then `--runs` (7) timed runs
   with the engines interleaved and the first rotating each run. A timing is the wall time of one request over a persistent local connection, one client, until the
   last byte; Elasticsearch is timed with its request cache off and, as a separate engine, on. The plan nodes of the PostgreSQL engines are recorded.
4. Cold runs (`cold`): for each engine and query, restart the container, then time the first run (`--cold-runs`, 3); the operating system's page cache is not dropped
   (that needs root), so this is "first run after a restart", not a cold disk.
5. Updates (`updates`, the first size only; `--update-sizes`): update 1% and 10% of the rows and delete 5% by primary key, in batches of 1,000, each system by the path its
   documentation recommends (listed in docs/BENCHMARKS.md with the official pages), the state compared with PostgreSQL's after each step; single-row changes timed until
   a query sees them; the revenue-per-month query timed under 0, 1,000 and 10,000 updated rows per second (a second connection, batches of 100 rows); sizes before,
   after the changes and after a maintenance step (`VACUUM`, `OPTIMIZE ... FINAL`, `_forcemerge`). A system that fails is a row of the results, not a gap.

The timed steps take the perf lock (`/tmp/sift-perf.lock`) one at a time; loads do not. Options: `--sizes`, `--cpus`, `--memory`, `--runs`, `--cold-runs`, `--update-sizes`,
`--engines pg,sift,sift_table,es,es_cached,ch` (for `updates`: `pg,sift,sift_table,es,ch,ch_mut,ch_rmt`), `--queries 01,03,search_count`, `--docs`, `--out`;
`STORE_LOAD_SECONDS` and `STORE_NAME` (container prefix) are environment variables. `steps/store.py` has the individual commands (`gen`, `load`, `run`, `cold`, `updates`, `merge`,
`render`); `store.sh setup|run|cold|updates` keep the containers up for development.

## Remote host

`bench/bench remote` reproduces everything on another machine under `~/Projects/sift` without sudo: a PostgreSQL
18.6 build in `pg18/` with contrib, rum, pg_textsearch, pgvector and pgx-bm25 built by PGXS, the ParadeDB
package, a minimal rustup and `xbench`; the host needs the Elasticsearch system package. `RHOST` names the ssh host.

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
bench/bench check --tier ab oracle elasticsearch 200               # an engine outside Postgres: built in its own server, asked over HTTP
bench/bench check --tier ab rank elasticsearch 200
bench/bench check --dataset places --tier ab --engines elasticsearch 30
```

The rank check is soft: pgx-bm25 is the reference, it tokenizes alphanumeric runs while pg_textsearch (and rum, gin) use the PG
parser, and pg_textsearch scores a query as a bag of terms; expect overlaps below 1 for pg_textsearch (docs/BENCHMARKS.md has the numbers).

`oracle` builds the engine index in `o_<engine>` (reused unless `REBUILD=1`) and an oracle `oracle_tsv`
table (`to_tsvector` with a `simple`-based configuration, `unaccent` when `ORACLE_ACCENTS=fold`,
`<lang>_stem` when `ORACLE_STEM=<lang>`). For each query it compares the engine's match set and `count(*)`
with the oracle and prints a summary plus every mismatch. Exit status is 1 on any mismatch; the report is
saved as `bench/out/<tier letter>/oracle-<engine>.txt`. By default the oracle text is split into alphanumeric
runs first (`ORACLE_TOKENS=split`), the tokenization of sift, because the PG parser keeps `Force.In`, `a@b.c` or
`1.5` as one token; `ORACLE_TOKENS=pg` uses the PG parser as is (gin, rum, pg_textsearch, whose adapters select it). A tsvector keeps at most 256 positions of a lexeme (and positions saturate at 16383), so
a phrase of a very frequent word (`within the`) is missed by the oracle in the few documents where the word occurs more often than that: the oracle then has fewer matches than an engine that is right 
An engine outside Postgres has no database of its own for the check: the harness builds it in its own server from the documents of the clone (`o_<engine>`), asks it the match requests and the count requests and compares the answers in SQL (`oracle_diff` in `sql/oracle.sql`).

## Footprint

`bench/bench du` lists what `$BENCH_WORK` (live clusters, scratch, engine builds; default `/tmp/sift-bench`) and `$BENCH_STORE` (datasets, downloads, corpus dumps, cache of lanes; default the same
directory) hold, by component, and `run`, `all` and `cold` end with the peak footprint of the session (a sampler that sizes both every 3 s). Put `BENCH_STORE` on a disk to keep the inputs and the
caches out of a tmpfs; the live clusters stay in `BENCH_WORK`, which should be fast.

Measured on this host (slim tier, seven engines; the peak is the sampler's maximum):

| | before this change | now |
|---|---|---|
| setup (five engines) | 163 s | 74 s |
| peak of the first session | 4.4 GiB (quick, 25,000 articles, five engines) | 2.05 GiB (slim, 50,000 articles, seven engines, `--docs` with the cold-cache tables) |
| left after the session | 3.3 GiB, plus 2.1 GiB of orphaned lanes per further session (6.5 GiB after the second, 19.6 GiB after a full-tier probe) | 0.47 GiB: the datasets, the engines and the dump of the tier |
| full tier, one class, one round | 18.7 GiB peak, 15.3 GiB left (five engines) | 5.2 GiB peak, 2.6 GiB left with `--keep` (the lanes cached; measured with five engines before the one-at-a-time method) |

What made it large, and what changed: the cache of lanes was never pruned and its key changed with every rebuild of the extension (orphans); a lane held the `bench`
template next to the engine's database (`adopt_db` renames it now); the write-ahead log the build recycled was cached (`lane_trim`); cached lanes were uncompressed (zstd now, written
only with `--keep` or for the tiers whose builds take minutes); the raw downloads and the engine tarballs were kept (removed after setup); the Rust target directories (0.8 GB) stayed
(only the stripped `xbench` does); `articles.tsv` was plain text (gzip, parsed in parallel); the Elasticsearch corpus export was never removed. None of this changes a measurement: index sizes were identical before and after, and
interleaved runs of the old and the new harness on the same engines gave cell medians within 5% of each other in both directions (the old harness against itself differs as much).

`bench/bench clean` destroys the clusters and removes the scratch under `$BENCH_WORK`; `run` does it for its own cluster on exit. `--cache` also removes the corpus dumps, the cache of lanes and the
corpus exported for the external engines, `--build` the engine builds, `--all` everything including the datasets and downloads (the same as `rm -rf $BENCH_WORK $BENCH_STORE`).
