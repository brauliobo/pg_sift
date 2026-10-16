# sift benchmark and correctness harness

Two public, standard use-cases, both measured against ParadeDB pg_search (and, for articles, the
other Postgres engines) with the same data, queries, clients and cache state. Data lives under
`$SIFT_BENCH_ROOT` (default `/tmp/sift-bench`); `bench/out*` is gitignored.

| use-case | data | what it exercises |
|---|---|---|
| articles (full text) | Simple English Wikipedia | and / or / phrase / mixed top-10, `count(*)`, filtered, join |
| places (multi-field autocomplete) | GeoNames | 1-4 letter prefixes, whole words, 2-3 word ANDs, name-field boost, top-k, filtered, unordered scans, `count(*)` |

## Datasets

`bench/datasets/fetch.sh articles|places|all` downloads each dataset into its own new directory under
`$SIFT_BENCH_ROOT/download`, verifies it, writes `SHA256SUMS` next to it and converts it with the
`python3 -I` scripts in `bench/datasets/` (the downloads are only read, never executed).

| dataset | source | license | checksum |
|---|---|---|---|
| Simple English Wikipedia, `simplewiki-20261001-pages-articles.xml.bz2` (342 MB) | https://dumps.wikimedia.org/simplewiki/20261001/ | CC BY-SA 4.0 | sha1 `f55881650e447c39e8bdeba4b1bb8d5576342d4b`, equal to the published `simplewiki-20261001-sha1sums.txt` |
| GeoNames `cities500.zip` (236,192 places), `cities15000.zip` (34,163), `admin1CodesASCII.txt`, `countryInfo.txt`, `featureCodes_en.txt` | https://download.geonames.org/export/dump/ (files of 2026-10-15) | CC BY 4.0 | sha256 `46fd044a...351ad2` (cities500), `d5cc2af7...3d231` (cities15000), full list in the `SHA256SUMS` written by the script |

An article is its title plus the cleaned wikitext of one main-namespace, non-redirect page of at least
200 characters (218,058 articles, 1.4 KB on average). A place is name, alternate names, region,
country, country code, feature name and population; the typed queries (300 per class) are sampled from
the places themselves with a fixed seed. Without network access, nothing else in the harness changes:
put any `articles.tsv` (COPY text of `id, title + body, category_id, created, likes, url`) in
`$SIFT_BENCH_ROOT/data`.

## Results

With the default index sift is ahead of pg_search on and / or / mixed / join and on every places cell, level on the filter
class and behind on phrase and `count(*)` (0.50x and 0.52x at 218k articles); `phrase_index = pairs` is ahead everywhere.

3 min 40 s after 23 s of index builds), `ab.sh quick` 9 min 14 s, `places.sh full` 36 min 31 s, `places.sh quick` 8 min 12 s. The
earlier per-engine harness took about 5 min 11 s per round for 3 engines and 6 classes, plus about 35 s for the join and 20 s
to load the corpus.

## Tiers

| tier | articles | places | port | dump |
|---|---:|---:|---:|---|
| Q (quick) | 75,000 (`Q_DOCS`) | 34,163 (cities15000) | 56040 | `$SIFT_BENCH_ROOT/cache/sift-Q-75000.dump` |
| F (full) | 218,058 | 236,192 (cities500) | 56050 | `$SIFT_BENCH_ROOT/cache/sift-F-ALL.dump` |

## Entry points

```sh
scripts/pgtest.sh install                    # sift into /tmp/sift-arch/root (SIFT_ROOT); pg_search comes from pg_config
bench/datasets/fetch.sh all                  # once: download, verify, convert
bench/ab.sh quick|full                       # articles: sift vs sift phrase_index=pairs vs pg_search, interleaved
bench/ab.sh cold quick|full                  # the same classes with index and heap evicted before every query
bench/places.sh quick|full [cold]            # places: sift vs pg_search, interleaved
```

`ab.sh` builds every engine database once from the cached corpus dump, then runs every class on every
alike. Rounds stop once every cell's spread is under `TOL` after `MINR`, or at `MAXR`. It keeps only
`builds.tsv`, `summary.tsv` with medians and ratios) and removes its scratch cluster on exit (`KEEP=1`
keeps it); the corpus dump stays in the cache.

## Layout

| path | role |
|---|---|
| `ab.sh`, `places.sh` | the interleaved A/B entry points above |
| `lib.sh` | shared settings: tiers, ports, `q` (psql), `locked` (perf lock), `limited` (8 CPU / 32 GB budget) |
| `cluster.sh` | per-tier throwaway PG18 cluster: `TIER=F bench/cluster.sh init\ | start\ | stop\ | destroy` |
| `corpus.sh` | `fetch` the articles, `load` a tier and cache its dump, `restore` it |
| `datasets/` | `fetch.sh`, `wiki.py`, `places.py`: download, verify, convert |
| `fetch.sh` | unpack third-party engines under `$SIFT_BENCH_ROOT/engines` and build `xbench` |
| `engines/*.sh` | one adapter per engine (see below) |
| `run.sh` | one Postgres engine: timed index build + pgbench workloads (`BUILD_ONLY=1`: build and keep) |
| `quick.sh`, `report.sh` | the multi-engine standard suite and its tables |
| `build.sh` | sift CREATE INDEX per parallel worker count: `[COPIES=n] TIER=F bench/build.sh 0 2 4`, appends to `build-workers.tsv` |
| `cold.sh` | cold-cache A/B of a boolean GUC (`GUC=sift.prefetch`) |
| `xrun.sh` | non-Postgres engines: same documents, queries, clients and budget, driven by `xbench` |
| `oracle.sh`, `rank_check.sh` | exact match-set / count(*) oracle (tsvector); soft BM25 check against pgx-bm25 |
| `remote.sh` | sync, build, run and clean the whole harness on a remote host (`RHOST`) |
| `sql/` | schema, tier load + query trace, harness (rendering), oracle, places schema |
| `xbench/` | Rust load generator (HTTP keep-alive) and in-process Tantivy driver |

`bench` holds `docs(id, body, category_id, created, likes, url)`, `categories(id, name, region)` for
the join workload, `params` (tier constants) and `queries`: 1000 substrings of 2-15 consecutive plain
word tokens (PG `simple` parser) from random documents, each as `and`, `or` and `phrase` (3000
queries). The oracle adds a `not` form (all terms, last one negated).

## Engine adapters

Postgres adapters (`gin`, `rum`, `pg_textsearch`, `bm25` = pgx-bm25, `paradedb`, `sift`, `sift_pairs` = sift
with `phrase_index = pairs`) set `REQUIRES` (extension), `SETUP`, `INDEX` and `RENDER`, which defines two SQL
functions: `bench_pred(mode, terms)` (WHERE clause, NULL if unsupported) and `bench_rank(mode, terms)`
(ORDER BY, NULL if no top-k). `sql/harness.sql` composes every statement kind from them (top10, count,
match, filter, join), so a new engine is one small file.
`sift.score(ctid)`); the cluster loads it from `SIFT_ROOT` (default `/tmp/sift-arch/root`) and the harness
skips it while absent. ParadeDB is indexed as an application would: `bm25 (id, body, created, category_id)`
with the default tokenizer, and `edge_ngram(1, 15)` on the places name and all-fields text.

External adapters (`elasticsearch`, `tantivy`, and `meilisearch` / `typesense`, which are kept but not
compared: typo-tolerant, non-BM25 ranking) set `EXTERNAL`, `URL`, `XB_ARGS` and define `x_start`, `x_stop`,
`x_build`, `x_size`, `x_pid` plus SQL `x_req` / `x_write` that render requests from the same query trace.

## Workloads (`run.sh`, `ab.sh`)

`and`, `or`, `phrase`, `mixed` (all three, top-10), `count` (count(*) of all three), `filter` (top-10 +
`created >= cutoff`, ~2% of rows), `join` (top-10 joined with `categories`, region filter), `update`
(`run.sh` only: writers at `UPDATE_RATE`=1000 ops/s: 70% update, 15% insert, 15% delete, while `mixed`
readers run; WAL bytes from `pg_current_wal_lsn`). Defaults of `run.sh`: 8 clients, 5 s warm-up, 30 s timed,
`statement_timeout` 120 s. Timed sections of `run.sh` hold `flock /tmp/sift-perf.lock`; its engine
servers run in a `systemd-run --user --scope` with `CPUQuota=800%` and `MemoryMax=32G`.

Places classes are what a user has typed: `p1`-`p4` a name's first word cut to 1-4 letters, `w1` a whole
word, `a2`/`a2s` two words (last cut to 2 / 1 letters), `a3` name, region and country word, `x2` name word
plus country word; workloads `top` (top-20), `boost` (name matches x3), `filter` (`country_code = 'US'`),
`count`, `scan` (unordered, `population >= 100000`).

## Remote host

`remote.sh` reproduces everything on another machine under `~/Projects/sift` without sudo: a PostgreSQL
18.6 build in `pg18/` with contrib, rum, pg_textsearch, pgvector and pgx-bm25 built by PGXS, the ParadeDB
package, Elasticsearch, a minimal rustup and `xbench`. `RHOST` names the ssh host.

```sh
RHOST=bench-host bench/remote.sh sync        # repo (no .git), Q/F dumps, pgx-bm25 source (BM25_SRC)
bench/remote.sh setup                        # builds (nice, -j4)
bench/remote.sh run 'TIER=F bench/run.sh gin; TIER=F bench/xrun.sh tantivy'
bench/remote.sh pull                         # -> bench/out-<host>/
bench/remote.sh clean                        # keep dumps; `clean all` removes ~/Projects/sift
```

## Correctness

```sh
TIER=Q bench/oracle.sh sift 200                       # accent + case folding, no stemming (default)
TIER=Q ORACLE_ACCENTS=keep bench/oracle.sh gin 200    # engines indexing raw 'simple' lexemes
TIER=Q MODES="phrase not" REBUILD=1 bench/oracle.sh sift 50
TIER=Q bench/rank_check.sh sift 200                   # top-10 overlap with pgx-bm25 per mode
```

`oracle.sh` builds the engine index in `o_<engine>` (reused unless `REBUILD=1`) and an oracle `oracle_tsv`
table (`to_tsvector` with a `simple`-based configuration, `unaccent` when `ORACLE_ACCENTS=fold`,
`<lang>_stem` when `ORACLE_STEM=<lang>`). For each query it compares the engine's match set and `count(*)`
with the oracle and prints a summary plus every mismatch. Exit status is 1 on any mismatch; the report is
saved as `bench/out/<tier>/oracle-<engine>.txt`. Tokenization is the PG parser's: CJK runs are one token,
hyphenated words index the compound and its parts, `don't` is `don` + `t`.

## Cleanup

`TIER=X bench/cluster.sh destroy` removes a cluster; `ab.sh` and `places.sh` do it on exit. The dumps in
`$SIFT_BENCH_ROOT/cache` and the downloads stay (`rm -rf $SIFT_BENCH_ROOT` removes everything).
