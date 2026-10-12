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
| **sift** | 1.3 | 18 | 0.8 / 3.1 | 1.6 / 4.9 | 1.8 / 5.1 | 1.7 / 5.2 | 3.5 / 9.5 | 7.7 / 10.9 | 0.9 / 124.4 | 3393 |
| GIN (`tsvector`) | 13.9 | 53 | 109.7 / 235.4 | 113.2 / 182.7 | 361.9 / 772.6 | 111.2 / 176.7 | 141.6 / 264.4 | n/a | 35.2 / 73.9 | 47 |
| rum 1.3.15 | 18.4 | 94 | 44.5 / 71.8 | 54.0 / 82.3 | 126.7 / 134.8 | 56.4 / 89.7 | 144.0 / 242.3 | n/a | 45.9 / 69.7 | 128 |
| pg_textsearch 1.5.1 | 5.4 | 50 | 1.9 / 2.5 | n/a | 3.0 / 4.4 | n/a | n/a | n/a | n/a | n/a |
| pgx-bm25 1.0 | 5.9 | 66 | 56.5 / 91.7 | 91.3 / 127.2 | 99.0 / 149.8 | 97.6 / 175.9 | 145.7 / 561.5 | n/a | 134.7 / 204.1 | 92 |
| ParadeDB pg_search 0.26.1 | 1.9 | 59 | 1.6 / 4.6 | 3.1 / 5.2 | 5.0 / 6.7 | 7.5 / 16.0 | 2.8 / 3.6 | 83.3 / 2054.8 | 2.3 / 12.4 | 1325 |
| Tantivy 0.26 (in-process) | 2.8 | 40 | 0.4 / 0.9 | 1.0 / 2.2 | 0.8 / 1.7 | 2.6 / 7.2 | 1.2 / 1.3 | 2.2 / 2.7 | 0.5 / 6.4 | 4187 |
| Elasticsearch 9.5.5 (HTTP) | 13.1 | 96 | 2.9 / 5.6 | 7.8 / 14.8 | 4.0 / 9.3 | 4.7 / 10.7 | 3.4 / 6.6 | 11.3 / 21.4 | 3.4 / 7.0 | 683 |

n/a: the engine cannot express that query class. Postgres latencies go through pgbench, so they
include a client round trip and a primary-key lookup; Elasticsearch's include HTTP and JSON;
Tantivy's are in-process calls with no client/server hop. sift ran with AVX2 kernels (this host has
no AVX-512) and is behind ParadeDB on prefix queries and on `count(*)` of phrases at p95. The host was shared, with a 1-minute
load average of about 5 at the start (uptime 5.40 / 5.08 / 5.05). [docs/BASELINE.md](docs/BASELINE.md) has the method, the TIN-style full
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
