# sift

sift is a full-text search index access method for PostgreSQL 18. Postings are keyed directly by
heap ctid and stored as two-level bitmaps (heap page, then line offset), so boolean queries and
`count(*)` are bitmap operations, ranking is exact BM25 with block-max pruning, and MVCC is
handled through the visibility map instead of a doc-id mapping. The design follows PlanetScale's
TIN and is documented in [DESIGN.md](DESIGN.md).

Status: work in progress. Index build, storage, the SIMD codec and the query parser are in;
query execution is being wired up, so the sift rows below are still pending.

```sql
CREATE EXTENSION sift;
CREATE INDEX docs_body ON docs USING sift (body);

SELECT id FROM docs WHERE body @@> 'breakfast AND "friendly staff"'
ORDER BY sift.score(ctid) DESC LIMIT 10;                  -- BM25 top-10
SELECT count(*) FROM docs WHERE body @@> 'pool OR spa';   -- counts from bitmaps
SELECT id FROM docs WHERE body @@> 'brea* AND NOT noisy~1';
```

## Benchmarks

Standard suite on 300,000 real documents, 36 fixed queries, 8 clients, every engine with the
same analysis (lowercase, no stemming, no stopwords). Host: AMD Ryzen 5 3600 (6 cores / 12
threads, AVX2, no AVX-512), 62 GB RAM, PostgreSQL 18.6; each engine server is capped at 8 CPUs and
32 GB. Latency p50 / p95 in ms, client-observed; QPS of the ranked top-10 mix.

| engine | build s | index MB | term | and | or | phrase | prefix | fuzzy | count(*) | mix QPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **sift** | pending | | | | | | | | | |
| GIN (`tsvector`) | 11.8 | 53 | 99.2 / 216.1 | 110.4 / 180.6 | 316.1 / 672.0 | 109.5 / 173.5 | 121.3 / 222.8 | n/a | 26.7 / 70.3 | 54 |
| rum 1.3.15 | 16.6 | 94 | 44.0 / 70.5 | 52.3 / 89.7 | 87.0 / 132.2 | 55.1 / 88.0 | 105.2 / 198.3 | n/a | 35.9 / 70.3 | 151 |
| pg_textsearch 1.5.1 | 4.7 | 50 | 2.0 / 4.8 | n/a | 2.8 / 4.4 | n/a | n/a | n/a | n/a | n/a |
| pgx-bm25 1.0 | 5.8 | 66 | 57.0 / 85.5 | 73.6 / 112.8 | 94.1 / 143.2 | 101.4 / 167.2 | 108.0 / 561.0 | n/a | 96.4 / 195.3 | 104 |
| ParadeDB pg_search 0.26.1 | 1.8 | 50 | 1.4 / 4.6 | 2.2 / 5.2 | 3.3 / 6.6 | 7.4 / 37.2 | 2.1 / 3.5 | 65.0 / 2099.6 | 2.2 / 11.5 | 1443 |
| Tantivy 0.26 (in-process) | 2.5 | 40 | 0.4 / 0.8 | 1.0 / 2.2 | 0.8 / 1.8 | 2.5 / 6.8 | 0.9 / 1.3 | 2.0 / 2.7 | 0.4 / 5.2 | 4522 |
| Elasticsearch 9.5.5 (HTTP) | 10.8 | 96 | 1.9 / 2.8 | 4.6 / 35.6 | 2.8 / 5.6 | 3.4 / 26.7 | 2.3 / 3.9 | 10.3 / 60.0 | 1.9 / 4.5 | 1145 |

n/a: the engine cannot express that query class. Postgres latencies go through pgbench, so they
include a client round trip and a primary-key lookup; Elasticsearch's include HTTP and JSON;
Tantivy's are in-process calls with no client/server hop. The host was shared, with a 1-minute
load average of 3-9. [docs/BASELINE.md](docs/BASELINE.md) has the method, the TIN-style full
suite on 2.6M documents (boolean, phrase, count, filtered, join and 1000 writes/s update
workloads, MB read per query, WAL), correctness checks and per-engine notes.

TIN itself is proprietary (PlanetScale only) and is not benchmarked here. PlanetScale reports 199
QPS for mixed top-10 queries on 150M documents, about 25x ParadeDB on the same hardware
([source](https://planetscale.com/blog/introducing-tin)).

### Reproduce

```sh
bench/fetch.sh                        # ParadeDB, pgx-bm25 (BM25_PKG), Elasticsearch; builds bench/xbench
bench/quick.sh                        # all engines, about 4 minutes; prints the table
bench/quick.sh paradedb sift          # selected engines
bench/quick.sh --full gin             # TIN-style suite on 2.6M documents, about 10 minutes per engine
```

The corpus is private document data: `quick.sh` restores it from the cached dumps in
`$SIFT_BENCH_ROOT/cache` (`sift-S.dump`, `sift-M.dump`, `sift-L.dump`), which exist on the
development machine and the benchmark host; `bench/corpus.sh` rebuilds them from the source database.

On a dedicated benchmark host, without sudo:

```sh
bench/remote.sh sync && bench/remote.sh setup   # PG 18.6 + extensions + Rust + engines under ~/Projects/sift
bench/remote.sh quick                           # the table above; results pulled to bench/out-<host>/
```

`bench/oracle.sh ENGINE` checks an engine's match sets and `count(*)` against a tsvector oracle,
and `bench/rank_check.sh ENGINE` compares its top-10 with pgx-bm25's BM25. Details in
[bench/README.md](bench/README.md).

## Build and install

Requires PostgreSQL 18 with server headers (`pg_config` on `PATH`) and gcc or clang on x86-64.
SIMD kernels are selected at run time (AVX-512, AVX2 or scalar).

```sh
make USE_PGXS=1 -j4
make USE_PGXS=1 install
make USE_PGXS=1 installcheck                # regression tests
scripts/pgtest.sh install && scripts/pgtest.sh start   # throwaway cluster, no system install
```

Arch Linux packaging is in `packaging/`.

## Limitations

- sift is not yet measured; the table only shows the competitors it has to beat.
- The baseline corpus is small (300k and 2.6M documents, 227 characters each on average) compared
  with TIN's 150M; a 15.8M-document tier is cached and will run together with sift.
- The benchmark hosts are shared machines, so absolute numbers carry noise; compare engines measured
  in the same run. The reference host has no AVX-512.
- Tokenizers differ between engines (CJK runs, apostrophes, hyphenation), so result sets match
  exactly only for engines using PostgreSQL's parser; BM25 top-10 overlap between BM25 engines is
  91-96%.
- Meilisearch and Typesense adapters exist but are left out of the table: they are typo-tolerant
  and do not rank by BM25.

## Layout

| path | content |
|---|---|
| `DESIGN.md` | architecture and module contracts |
| `src/` | the extension |
| `test/`, `scripts/` | regression, isolation, TAP, differential and fuzz tests |
| `bench/` | benchmark and correctness harness |
| `docs/` | codec and kernel notes, `BASELINE.md` |
| `packaging/` | Arch Linux package |
