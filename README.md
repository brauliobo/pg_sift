# sift

sift is a full-text search index access method for PostgreSQL 18. Postings are keyed directly by
heap ctid and stored as two-level bitmaps (heap page, then line offset), so boolean queries and
`count(*)` are bitmap operations, ranking is exact BM25 with block-max pruning, and MVCC is
handled through the visibility map instead of a doc-id mapping. The design follows PlanetScale's

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

## Entity search

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
attributes (`bench/places.sh`, one client, warm) sift answers every class in 0.4-2.4 ms at p50 where ParadeDB
`edge_ngram(1, 15)` takes 1.5-11 ms (boosted top-20: 0.7-2.4 vs 5.8-11.3 ms; filtered top-20: 0.6-1.5 vs 7.7-31.5 ms;
`count(*)`: 0.35-0.63 vs 1.8-6.7 ms), cold-cache medians are 1.5-6x lower, and the index is 25 MB against 107 MB and builds in 1.1 s against 2.1 s.

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
  1.8x faster and 2.8x smaller than ParadeDB.
- The baseline corpus is small (300k and 2.6M documents, 227 characters each on average) compared
  with TIN's 150M; a 15.8M-document tier is cached and has not been run. On a 1M-document resample of the documents
  (one client, `and`/`or`/`phrase`/`mixed`/`count`/`filter` QPS, ParadeDB in brackets) sift is at 1009 (1954), 490 (584), 840 (1418), 703 (939), 426 (821) and 605 (715);
  with `phrase_index = pairs` 1247, 629, 1082, 904, 1735 and 668, at an index of 175 MB against ParadeDB's 187 MB (sift default 72 MB).
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
|  | architecture and module contracts |
| `src/` | the extension |
| `test/`, `scripts/` | regression, isolation, TAP, differential and fuzz tests |
| `bench/` | benchmark and correctness harness |
| `docs/` | codec and kernel notes, `BASELINE.md` |
| `packaging/` | Arch Linux package |
