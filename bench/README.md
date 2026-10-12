# sift benchmark and correctness harness

Reproduces PlanetScale's TIN methodology (https://planetscale.com/blog/introducing-tin) on real
documents text. Everything lives under `/tmp/sift-bench` (tmpfs); `bench/out` and `bench/data`
are gitignored. Results are summarized in `docs/BASELINE.md`.

## Layout

| path | role |
|---|---|
| `lib.sh` | shared settings: tiers, ports, `q` (psql), `locked` (perf lock), `limited` (8 CPU / 32 GB budget) |
| `cluster.sh` | per-tier throwaway PG18 cluster: `TIER=M bench/cluster.sh init\ | start\ | stop\ | destroy` |
| `corpus.sh` | `export` text from `source_db` (read-only), `load` a tier and cache its dump, `restore` it |
| `fetch.sh` | unpack third-party engines under `/tmp/sift-bench/engines` and build `xbench` |
| `engines/*.sh` | one adapter per engine (see below) |
| `run.sh` | Postgres engines: timed index build + pgbench workloads |
| `xrun.sh` | non-Postgres engines: same documents, queries, clients and budget, driven by `xbench` |
| `oracle.sh` | exact match-set / count(*) oracle (tsvector) for and/or/phrase/not |
| `rank_check.sh` | soft BM25 check: top-10 overlap with pgx-bm25 |
| `report.sh` | markdown tables from `bench/out/<tier>/*.tsv` |
| `sql/` | export, schema, tier load + query trace, harness (rendering), oracle |
| `xbench/` | Rust load generator (HTTP keep-alive) and in-process Tantivy driver |

## Tiers and cached dumps

| tier | docs | port | dump |
|---|---:|---:|---|
| S | 300,000 (deterministic sample) | 56010 | `/tmp/sift-bench/cache/sift-S.dump` |
| M | 2,639,309 (every non-empty document) | 56020 | `/tmp/sift-bench/cache/sift-M.dump` |
| L | 15,835,854 (M + 5 resampled copies) | 56030 | `/tmp/sift-bench/cache/sift-L.dump` |

A document is the document title plus all vm comments (manager replies excluded). L copies are
`head(doc a) + tail(doc b)` split at a space near the middle, with deterministic hash choices: the
corpus-wide term distribution (Zipf) and local n-grams stay real, the vocabulary does not grow.

Restore a tier in minutes (no access to `source_db` needed):

```sh
bench/fetch.sh bm25 paradedb                 # extension packages referenced by the cluster config
TIER=M bench/corpus.sh restore               # init + start the cluster, pg_restore -j4 into db `bench`
psql -h /tmp/sift-bench/run -p 56020 bench   # docs, props, queries, params
```

Rebuild from scratch: `bench/corpus.sh export && TIER=S bench/corpus.sh load` (`L_MULT`, `NSUB` tunable).

`bench` holds `docs(id, body, prop_id, score, country, created, likes, url)`, `props(id, name,
region)` for the join workload, `params` (tier constants) and `queries`: 1000 substrings of 2-15
consecutive plain word tokens (PG `simple` parser) from random documents, each as `and`, `or` and
`phrase` (3000 queries). The oracle adds a `not` form (all terms, last one negated).

## Engine adapters

Postgres adapters (`gin`, `rum`, `pg_textsearch`, `bm25` = pgx-bm25, `paradedb`, `sift`) set
`REQUIRES` (extension), `SETUP`, `INDEX` and `RENDER`, which defines two SQL functions:
`bench_pred(mode, terms)` (WHERE clause, NULL if unsupported) and `bench_rank(mode, terms)`
(ORDER BY, NULL if no top-k). `sql/harness.sql` composes every statement kind from them
(top10, count, match, filter, join), so a new engine is one small file. `sift` targets the
planned API (`body @@> 'a AND b'`, `sift.score(ctid)`) and is skipped until the extension exists.

External adapters (`elasticsearch`, `tantivy`, `meilisearch`, `typesense`) set `EXTERNAL`, `URL`,
`XB_ARGS` and define `x_start`, `x_stop`, `x_build`, `x_size`, `x_pid` plus SQL `x_req` / `x_write`
that render requests from the same query trace.

## Running

```sh
TIER=S bench/run.sh gin                      # build + all workloads; append to bench/out/S/*.tsv
TIER=M DURATION=60 bench/run.sh bm25 or mixed update
TIER=M bench/xrun.sh elasticsearch           # external engine, same workloads
bench/report.sh S M L                        # markdown tables
```

Workloads: `and`, `or`, `phrase`, `mixed` (all three, top-10), `count` (count(*) of all three),
`filter` (top-10 + `created >= cutoff`, ~2% of rows), `join` (top-10 joined with `props`, region
filter), `update` (writers at `UPDATE_RATE`=1000 ops/s: 70% update, 15% insert, 15% delete, while
`mixed` readers run; WAL bytes from `pg_current_wal_lsn`). Defaults: 8 clients, 5 s warm-up,
30 s timed, `statement_timeout` 120 s (pgbench aborts a client on timeout; reported as timeouts).
Latency percentiles come from pgbench per-transaction logs and include a PK lookup that fetches
the statement text; MB/query is shared buffer hits + reads from `pg_stat_statements`.

Timed sections (index builds, warm-up + measured runs) hold `flock /tmp/sift-perf.lock`. Every
engine server runs in a `systemd-run --user --scope` with `CPUQuota=800%` and `MemoryMax=32G`
(`CPU_QUOTA`, `MEM_MAX`); load generators run outside it.

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

`TIER=X bench/cluster.sh destroy` removes a cluster; the dumps in `/tmp/sift-bench/cache` stay.
