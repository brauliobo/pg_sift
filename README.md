# sift

sift is a full-text search index access method for PostgreSQL 18. Postings are keyed directly by
heap ctid and stored as two-level bitmaps (heap page, then line offset), so boolean queries and
`count(*)` are bitmap operations, ranking is exact BM25 with block-max pruning, and MVCC is
handled through the visibility map instead of a doc-id mapping. The design follows PlanetScale's
TIN and is documented in [DESIGN.md](DESIGN.md).

Status: index build, storage, the SIMD codec, query execution (boolean, phrase, prefix, fuzzy, ranked top-k,
`count(*)`), crash and physical-replica safety (generic WAL, no preload) and the benchmark harness are in.

```sql
CREATE EXTENSION sift;
CREATE INDEX docs_body ON docs USING sift (body);

SELECT id FROM docs WHERE body @@> 'breakfast AND "friendly staff"'
ORDER BY sift.score(ctid) DESC LIMIT 10;                  -- BM25 top-10
SELECT count(*) FROM docs WHERE body @@> 'pool OR spa';   -- counts from bitmaps
SELECT id FROM docs WHERE body @@> 'brea* AND NOT noisy~1';
```

## Property search

Short prefixes, fields and name boosts, as in a directory search over names, cities and countries:

```sql
CREATE INDEX props_sift ON props USING sift ((name || chr(29) || city || ' ' || country))
  WITH (fields = 'name', prefix_index = 3);                 -- chr(29) separates the sections, fields names them

SELECT id FROM props WHERE (name || chr(29) || city || ' ' || country) @@> '(gr* ho*) AND (name:{gr* ho*}^3 OR (gr* ho*))'
ORDER BY sift.score(ctid) DESC LIMIT 20;                    -- every word a prefix, name matches boosted x3
```

`prefix_index = n` indexes the prefixes of 1..n letters of every word, so 1-2 letter prefixes are one posting
list instead of a dictionary expansion; `fields` names the sections of the indexed text so `name:word` and
`name:{...}^3` search and boost one section. Accents and case are folded by default. On 300,000 synthetic
properties (`bench/places.sh`, one client, warm) sift answers every class in 0.4-2.4 ms at p50 where ParadeDB
`edge_ngram(1, 15)` takes 1.5-11 ms (boosted top-20: 0.7-2.4 vs 5.8-11.3 ms; filtered top-20: 0.6-1.5 vs 7.7-31.5 ms;
`count(*)`: 0.35-0.63 vs 1.8-6.7 ms), cold-cache medians are 1.5-6x lower, and the index is 25 MB against 107 MB and builds in 1.1 s against 2.1 s.

## Benchmarks

Standard suite on 300,000 real documents, 36 fixed queries, 8 clients, every engine with the
same analysis (lowercase, no stemming, no stopwords). Host: AMD Ryzen 5 3600 (6 cores / 12
threads, AVX2, no AVX-512), 62 GB RAM, PostgreSQL 18.6; each engine server is capped at 8 CPUs and
32 GB. Latency p50 / p95 in ms, client-observed; QPS of the ranked top-10 mix.

| engine | build s | index MB | term | and | or | phrase | prefix | fuzzy | count(*) | mix QPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **sift** | 1.3 | 18 | 0.8 / 3.0 | 1.6 / 5.0 | 1.8 / 5.1 | 1.8 / 5.3 | 1.6 / 6.5 | 4.5 / 5.6 | 0.9 / 125.4 | 3438 |
| sift, `phrase_index = pairs` | 5.2 | 77 | 1.1 / 3.1 | 1.8 / 5.0 | 2.0 / 5.1 | 2.0 / 4.9 | 1.5 / 6.4 | 4.5 / 5.7 | 1.0 / 1.5 | 3449 |
| GIN (`tsvector`) | 12.4 | 53 | 110.0 / 234.0 | 112.9 / 185.7 | 371.3 / 840.2 | 111.9 / 176.4 | 145.7 / 264.0 | n/a | 34.9 / 74.5 | 47 |
| rum 1.3.15 | 16.4 | 94 | 44.3 / 71.4 | 52.2 / 81.8 | 127.0 / 134.5 | 57.4 / 89.3 | 127.1 / 228.0 | n/a | 46.5 / 71.6 | 131 |
| pg_textsearch 1.5.1 | 5.5 | 50 | 2.1 / 2.5 | n/a | 2.8 / 4.4 | n/a | n/a | n/a | n/a | n/a |
| pgx-bm25 1.0 | 5.9 | 66 | 56.0 / 91.0 | 105.3 / 156.8 | 98.7 / 150.4 | 113.5 / 177.7 | 147.0 / 570.1 | n/a | 133.6 / 204.7 | 92 |
| ParadeDB pg_search 0.26.1 | 2.1 | 65 | 1.7 / 4.8 | 3.1 / 5.9 | 5.0 / 7.3 | 8.4 / 20.4 | 2.8 / 3.8 | 44.1 / 46.2 | 2.2 / 12.9 | 970 |
| Tantivy 0.26 (in-process) | 2.7 | 40 | 0.4 / 0.9 | 1.0 / 2.3 | 0.8 / 1.7 | 4.1 / 7.2 | 1.2 / 1.3 | 2.5 / 2.7 | 0.5 / 6.5 | 4132 |
| Elasticsearch 9.5.5 (HTTP) | 12.8 | 95 | 3.0 / 5.0 | 7.6 / 17.7 | 3.6 / 7.4 | 4.6 / 10.3 | 3.4 / 5.7 | 11.2 / 100.6 | 3.5 / 7.0 | 1069 |

n/a: the engine cannot express that query class. Postgres latencies go through pgbench, so they
include a client round trip and a primary-key lookup; Elasticsearch's include HTTP and JSON;
Tantivy's are in-process calls with no client/server hop. sift ran with AVX2 kernels (this host has
no AVX-512) and is behind ParadeDB only on prefix p95 and on `count(*)` of phrases at p95 (`phrase_index = pairs` removes the latter at 4x the index size, see DESIGN.md (g)). The host was shared, with a 1-minute
load average of about 5 at the start (uptime 5.01 / 5.01 / 4.72). [docs/BASELINE.md](docs/BASELINE.md) has the method, the TIN-style full
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

- On the 2.6M-document full suite sift trails ParadeDB on `or`, `phrase`, `mixed`, `filter` (0.5-0.9x QPS) and
  `count` (0.24x) and Tantivy on every class; a score above a join works but costs the whole candidate set when the join filter matches nothing. It builds the index
  1.8x faster and 2.8x smaller than ParadeDB (`docs/BASELINE.md`).
- The baseline corpus is small (300k and 2.6M documents, 227 characters each on average) compared
  with TIN's 150M; a 15.8M-document tier is cached and has not been run. On a 1M-document resample of the documents (8 clients, median of four interleaved runs on a host at load 14-27, `and` / `or` / `phrase` / `mixed` / `count` / `filter` QPS, ParadeDB in brackets) sift is at 1498 (1698), 660 (550), 1056 (1359), 970 (635), 516 (718) and 732 (582): ahead on `or`, `mixed` and `filter`, behind on `and`, `phrase` and `count` (the last two recheck phrases against the heap);
  with `phrase_index = pairs` `count` is about 1440 QPS (2x ParadeDB) at an index of 175 MB against ParadeDB's 187 MB (sift default 72 MB).
  Under 1000 writes/s sift keeps 527 QPS of mixed reads against 190.
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
