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

## Multi-field autocomplete

Short prefixes, fields and name boosts, as in a place or product search over names, regions and countries:

```sql
CREATE INDEX places_sift ON places USING sift ((name || chr(29) || admin1 || ' ' || country))
  WITH (fields = 'name', prefix_index = 3);                 -- chr(29) separates the sections, fields names them

SELECT id FROM places WHERE (name || chr(29) || admin1 || ' ' || country) @@> '(gr* ho*) AND (name:{gr* ho*}^3 OR (gr* ho*))'
ORDER BY sift.score(ctid) DESC LIMIT 20;                    -- every word a prefix, name matches boosted x3
```

`prefix_index = n` indexes the prefixes of 1..n letters of every word, so 1-2 letter prefixes are one posting
list instead of a dictionary expansion; `fields` names the sections of the indexed text so `name:word` and
`name:{...}` search and boost one section. Accents and case are folded by default.

## Benchmarks

`bench/ab.sh` and `bench/places.sh` compare sift, sift with `phrase_index = pairs` and ParadeDB pg_search on two
public datasets, Simple English Wikipedia articles (full text) and GeoNames places (multi-field autocomplete),
with the same data, queries, clients and cache state, interleaved round by round. Tables, host load and
build/size figures are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md); datasets, licenses and checksums in
[bench/README.md](bench/README.md).

Articles (218,058 articles, 8 clients, throughput relative to pg_search 0.26.1, medians of 6 interleaved rounds at host
load 38-58; index 48 MiB, `pairs` 267 MiB, pg_search 164-185 MiB):

| class | sift | sift `phrase_index = pairs` |
|---|---:|---:|
| and | 2.30x | 2.32x |
| or | 2.52x | 2.45x |
| phrase | 0.81x | 3.32x |
| mixed | 2.44x | 3.32x |
| count(*) | 0.82x | 3.34x |
| filter | 1.06x | 1.11x |
| join | 3.82x | 6.74x |

Places (236,192 places, single client, `sift.max_expansions = 100000`): sift is ahead of pg_search `edge_ngram(1, 15)` in all
45 class/workload cells, from 1.4x (one-letter prefixes) to 75x (filtered whole-word queries) in median latency; index 43 MiB
against 140 MiB.

```sh
scripts/pgtest.sh install                 # sift into /tmp/sift-arch/root; pg_search comes from pg_config
bench/datasets/fetch.sh all               # download, verify and convert both datasets
bench/ab.sh quick                         # articles, 150k documents
bench/places.sh quick                     # places, 34k rows
```

`bench/oracle.sh ENGINE` checks an engine's match sets and `count(*)` against a tsvector oracle, and
`bench/rank_check.sh ENGINE` compares its top-10 with pgx-bm25's BM25.

## Build and install

Requires PostgreSQL 18 with server headers (`pg_config` on `PATH`) and gcc or clang on x86-64.
SIMD kernels are selected at run time (AVX-512, AVX2 or scalar).

```sh
make USE_PGXS=1 -j4
make USE_PGXS=1 install
make USE_PGXS=1 installcheck                # regression tests
make check                                  # quick tier: every suite, small corpora, about 2.5 minutes
make check-full                             # full tier: ASan, long soak and fuzz, all mutants (about 22 minutes)
scripts/pgtest.sh install && scripts/pgtest.sh start   # throwaway cluster, no system install
```

`make check` and `make check-full` run `scripts/ci.sh quick|full`: the suites in four parallel lanes, each with its own
cluster, in one scratch directory that is removed on success and capped at `CI_MAX_MB` (`SANITIZE=1 make check` runs the
quick tier on the ASan build).

Arch Linux packaging is in `packaging/`.

## Limitations

- Phrases are rechecked against the heap unless the index is built with `phrase_index = pairs` (a larger
  index); `count(*)` and ranked scans of common-word phrases pay for it, and stay at 0.8x of pg_search on the
  articles benchmark with the default index.
- The benchmark corpora are small compared with TIN's 150M documents, and the hosts are shared machines, so
  absolute numbers carry noise; compare engines measured in the same run.
- Tokenizers differ between engines (CJK runs, apostrophes, hyphenation), so result sets match exactly only
  for engines using PostgreSQL's parser; BM25 top-10 overlap between BM25 engines is 91-96%.
- Meilisearch and Typesense adapters exist but are not compared: they are typo-tolerant and do not rank by BM25.

## Layout

| path | content |
|---|---|
| `DESIGN.md` | architecture and module contracts |
| `src/` | the extension |
| `test/`, `scripts/` | regression, isolation, TAP, differential and fuzz tests |
| `bench/` | benchmark and correctness harness |
| `docs/` | codec and kernel notes, `BENCHMARKS.md` |
| `packaging/` | Arch Linux package |
