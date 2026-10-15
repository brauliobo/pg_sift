# sift benchmark and correctness harness

Reproduces PlanetScale's TIN methodology (https://planetscale.com/blog/introducing-tin) on real
documents text. Data lives under `$SIFT_BENCH_ROOT` (default `/tmp/sift-bench`); `bench/out*`
and `bench/data` are gitignored. Results are summarized in `docs/BASELINE.md`.

## Layout

| path | role |
|---|---|
| `lib.sh` | shared settings: tiers, ports, `q` (psql), `locked` (perf lock), `limited` (8 CPU / 32 GB budget) |
| `cluster.sh` | per-tier throwaway PG18 cluster: `TIER=M bench/cluster.sh init\|start\|stop\|destroy` |
| `corpus.sh` | `export` text from `source_db` (read-only), `load` a tier and cache its dump, `restore` it |
| `fetch.sh` | unpack third-party engines under `$SIFT_BENCH_ROOT/engines` and build `xbench` |
| `engines/*.sh` | one adapter per engine (see below) |
| `run.sh` | Postgres engines: timed index build + pgbench workloads |
| `build.sh` | sift CREATE INDEX per parallel worker count: `[COPIES=n] TIER=M bench/build.sh 0 2 4` (0 = serial; COPIES replicates the corpus), appends to `build-workers.tsv` |
| `cold.sh` | cold-cache A/B of a boolean GUC (`GUC=sift.prefetch`): index and heap evicted before every query, per-query and per-class medians in `cold.tsv` |
| `xrun.sh` | non-Postgres engines: same documents, queries, clients and budget, driven by `xbench` |
| `oracle.sh` | exact match-set / count(*) oracle (tsvector) for and/or/phrase/not |
| `rank_check.sh` | soft BM25 check: top-10 overlap with pgx-bm25 |
| `report.sh` | markdown tables from `bench/out/<tier>/*.tsv` (`RESULTS=dir` for another results tree) |
| `remote.sh` | sync, build, run and clean the whole harness on a remote host |
| `sql/` | export, schema, tier load + query trace, harness (rendering), oracle |
| `xbench/` | Rust load generator (HTTP keep-alive) and in-process Tantivy driver |

## Tiers and cached dumps

| tier | docs | port | dump |
|---|---:|---:|---|
| S | 300,000 (deterministic sample) | 56010 | `$SIFT_BENCH_ROOT/cache/sift-S.dump` |
| M | 2,639,309 (every non-empty document) | 56020 | `$SIFT_BENCH_ROOT/cache/sift-M.dump` |
| L | 15,835,854 (M + 5 resampled copies) | 56030 | `$SIFT_BENCH_ROOT/cache/sift-L.dump` |

A document is the document title plus all guest comments (manager replies excluded). L copies are
`head(doc a) + tail(doc b)` split at a space near the middle, with deterministic hash choices: the
corpus-wide term distribution (Zipf) and local n-grams stay real, the vocabulary does not grow.
L is built and cached but not part of the baseline yet (it runs together with sift).

Restore a tier in seconds (no access to `source_db` needed):

```sh
bench/fetch.sh bm25 paradedb                 # extension packages referenced by the cluster config
TIER=M bench/corpus.sh restore               # init + start the cluster, pg_restore -j4 into db `bench`
psql -h /tmp/sift-bench/run -p 56020 bench   # docs, props, queries, params
```

Rebuild from scratch: `bench/corpus.sh export && TIER=S bench/corpus.sh load` (`L_MULT`, `NSUB` tunable).

`bench` holds `docs(id, body, prop_id, rating, country, created, likes, url)`, `props(id, name,
region)` for the join workload, `params` (tier constants) and `queries`: 1000 substrings of 2-15
consecutive plain word tokens (PG `simple` parser) from random documents, each as `and`, `or` and
`phrase` (3000 queries). The oracle adds a `not` form (all terms, last one negated).

## Engine adapters

Postgres adapters (`gin`, `rum`, `pg_textsearch`, `bm25` = pgx-bm25, `paradedb`, `sift`, `sift_pairs` = sift with `phrase_index = pairs`) set
`REQUIRES` (extension), `SETUP`, `INDEX` and `RENDER`, which defines two SQL functions:
`bench_pred(mode, terms)` (WHERE clause, NULL if unsupported) and `bench_rank(mode, terms)`
(ORDER BY, NULL if no top-k). `sql/harness.sql` composes every statement kind from them
(top10, count, match, filter, join), so a new engine is one small file. `sift` follows DESIGN.md
(`body @@> 'a AND b'`, `sift.score(ctid)`); the cluster loads it from `SIFT_ROOT` (default
`/tmp/sift-arch/root`, the `scripts/pgtest.sh install` output) and the harness skips it while absent.

External adapters (`elasticsearch`, `tantivy`, and `meilisearch` / `typesense`, which are kept but
not in the baseline: typo-tolerant, non-BM25 ranking) set `EXTERNAL`, `URL`, `XB_ARGS` and define
`x_start`, `x_stop`, `x_build`, `x_size`, `x_pid` plus SQL `x_req` / `x_write` that render requests
from the same query trace. Elasticsearch runs with a 4 GB heap (`ES_HEAP`) and no pre-touch.

## Running

```sh
TIER=S bench/run.sh gin                      # build + all workloads; append to bench/out/S/*.tsv
TIER=M DURATION=60 bench/run.sh bm25 or mixed update
TIER=M bench/xrun.sh tantivy                 # external engine, same workloads
bench/report.sh S M                          # markdown tables
```

Workloads: `and`, `or`, `phrase`, `mixed` (all three, top-10), `count` (count(*) of all three),
`filter` (top-10 + `created >= cutoff`, ~2% of rows), `join` (top-10 joined with `props`, region
filter), `update` (writers at `UPDATE_RATE`=1000 ops/s: 70% update, 15% insert, 15% delete, while
`mixed` readers run; WAL bytes from `pg_current_wal_lsn`). Defaults: 8 clients, 5 s warm-up,
30 s timed, `statement_timeout` 120 s (pgbench aborts a client on an error; reported as aborted).
Latency percentiles come from pgbench per-transaction logs and include a PK lookup that fetches
the statement text; MB/query is shared buffer hits + reads from `pg_stat_statements`.

Timed sections (index builds, warm-up + measured runs) hold `flock /tmp/sift-perf.lock`. Every
engine server runs in a `systemd-run --user --scope` with `CPUQuota=800%` and `MemoryMax=32G`
(`CPU_QUOTA`, `MEM_MAX`); load generators run outside it.

## Property search (`places.sh`)

sift against ParadeDB on the shape of a multi-field directory search: one table of properties, word-prefix
AND across fields (name, alternative names, city, regions, country, postal code, address, type,
categories), 1-2 letter prefixes, name matches boosted x3, top-k by score, filtered and unordered
scans, `count(*)`. The real properties cannot be exported, so `places.py` makes a synthetic
directory (Zipfian pseudo-words, skewed first letters) and 300 typed queries per class; ParadeDB is
indexed as the application does (`edge_ngram(1, 15)` on a name field and an all-fields field), sift
with `fields = 'name', prefix_index = 3` on `name || chr(29) || other fields`.

```sh
scripts/pgtest.sh install                 # sift into /tmp/sift-arch/root; pg_search comes from pg_config
ROWS=300000 bench/places.sh init           # cluster under /tmp/sift-props (port 56060), load, build both indexes
WLS="top boost" bench/places.sh run p1 p2  # workloads top boost filter count scan; classes p1 p2 p3 p4 w1 a2 a2s a3 x2
bench/places.sh destroy
```

Classes are what a user has typed: `p1`-`p4` one word cut to 1-4 letters, `w1` a whole word, `a2`/`a2s`
two words (last cut to 2 / 1 letters), `a3` three words, `x2` name word plus city. `run` prints the
p50 / p95 ms of single-client pgbench transactions (each one primary-key lookup of the query text plus the
statement), `SECS` seconds per cell, `SIFT_OPTS` overrides the sift index options.

## Remote host

`remote.sh` reproduces everything on another machine under `~/Projects/sift` without sudo: a
PostgreSQL 18.6 build in `pg18/` with contrib, rum, pg_textsearch, pgvector and pgx-bm25 built by
PGXS, the ParadeDB package, Elasticsearch, a minimal rustup and `xbench` (`-C target-cpu=znver2`).

```sh
bench/remote.sh sync                         # repo (no .git), S/M dumps, pgx-bm25 source
bench/remote.sh setup                        # builds (nice, -j4)
bench/remote.sh run 'TIER=M bench/run.sh gin; TIER=M bench/xrun.sh tantivy'
bench/remote.sh pull                         # -> bench/out-<host>/
RESULTS=bench/out-HOST bench/report.sh S M
bench/remote.sh clean                        # keep dumps; `clean all` removes ~/Projects/sift
```

sift on the same host, in the same session as the baselines:
`bench/remote.sh sync && bench/remote.sh sift` (builds with the remote PG and installs into `$SIFT_ROOT`),
then `bench/remote.sh quick sift` (the standard suite (timed sections hold `flock /tmp/sift-perf.lock`), rebuilding sift
first) or `bench/remote.sh run 'TIER=M bench/run.sh sift'` (the cluster picks the extension up from `$SIFT_ROOT`).

## Correctness

```sh
TIER=S bench/oracle.sh sift 200                       # accent + case folding, no stemming (default)
TIER=S ORACLE_ACCENTS=keep bench/oracle.sh gin 200    # engines indexing raw 'simple' lexemes
TIER=S MODES="phrase not" REBUILD=1 bench/oracle.sh sift 50
TIER=S bench/rank_check.sh sift 200                   # top-10 overlap with pgx-bm25 per mode
```

`oracle.sh` builds the engine index in `o_<engine>` (reused unless `REBUILD=1`) and an oracle
`oracle_tsv` table (`to_tsvector` with a `simple`-based configuration, `unaccent` when
`ORACLE_ACCENTS=fold`, `<lang>_stem` when `ORACLE_STEM=<lang>`). For each query it compares the
engine's match set (`SELECT id ... WHERE pred`) and `count(*)` with the oracle and prints a summary
plus every mismatch: missing/extra counts, the first ids, and an excerpt of the first differing
document. Exit status is 1 on any mismatch; the report is saved as `bench/out/<tier>/oracle-<engine>.txt`.
Tokenization is the PG parser's: CJK runs are one token, hyphenated words index the compound and
its parts, `don't` is `don` + `t`. An engine with a different tokenizer shows those differences.

## Cleanup

`TIER=X bench/cluster.sh destroy` removes a cluster; the dumps in `$SIFT_BENCH_ROOT/cache` stay.
