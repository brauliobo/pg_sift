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
  index); `count(*)` and ranked scans of common-word phrases pay for it, and lose to pg_search by half on the
  articles benchmark with the default index.
- The benchmark corpora are small compared with TIN's 150M documents, and the hosts are shared machines, so
  absolute numbers carry noise; compare engines measured in the same run.
- Tokenizers differ between engines (CJK runs, apostrophes, hyphenation), so result sets match exactly only
  for engines using PostgreSQL's parser; BM25 top-10 overlap between BM25 engines is 91-96%.
- Meilisearch and Typesense adapters exist but are not compared: they are typo-tolerant and do not rank by BM25.

## Layout

| path | content |
|---|---|
|  | architecture and module contracts |
| `src/` | the extension |
| `test/`, `scripts/` | regression, isolation, TAP, differential and fuzz tests |
| `bench/` | benchmark and correctness harness |
| `docs/` |  |
| `packaging/` | Arch Linux package |
