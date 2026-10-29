# Changelog of the SQLite extension

`sift` for SQLite (release artifact `sift-sqlite`) is versioned with the repository: it is released under the repository tag
(`v1.3.0` is `sift-sqlite` 1.3.0) and `sqlite/VERSION` holds the version, which `sift_version()` reports. Changes of the PostgreSQL
extension are in [../CHANGELOG.md](../CHANGELOG.md).

## 3.6.2

- Rebuilt with the PostgreSQL release 3.6.2.

## 3.6.1

- Rebuilt with the PostgreSQL release 3.6.1.

## 3.6.0

- Rebuilt with the PostgreSQL release 3.6.0.

## 3.5.3

- Rebuilt with the PostgreSQL release 3.5.3.

## 3.5.2

- Rebuilt with the PostgreSQL release 3.5.2.

## 3.5.1

- Rebuilt with the PostgreSQL release 3.5.1.

## 3.5.0

- Rebuilt with the PostgreSQL release 3.5.0.

## 3.4.2

- Rebuilt with the PostgreSQL release 3.4.2.

## 3.4.1

- Rebuilt with the PostgreSQL release 3.4.1.

## 3.4.0

- Rebuilt with the PostgreSQL release 3.4.0.

## 3.3.0

- Rebuilt with the PostgreSQL release 3.3.0.

## 3.2.3

- Rebuilt with the PostgreSQL release 3.2.3.

## 3.2.2

- Rebuilt with the PostgreSQL release 3.2.2.

## 3.2.1

- Rebuilt with the PostgreSQL release 3.2.1.

## 3.2.0

- Rebuilt with the PostgreSQL release 3.2.0.

## 3.1.0

- No change to the storage format (2) or the SQL interface: tables of 3.0.2 open as they are, and this extension still writes BM25 segment format 1. The extension is rebuilt with the PostgreSQL release 3.1.0, and `sift_version()` reports engine format 1.3 (what the build reads: the segment format 3 that PostgreSQL 3.1 writes is read by the shared core, which this extension never writes).
- Fixed, shared core: a wide wildcard, regex or fuzzy leaf (more terms than the expansion limit) next to an optional phrase (`/lev.*/ OR "two words"`) lost its score for the documents that matched it, so the ranking differed from the documented one; counts and the set of matches were right. The files of the shared core (`src/portable.list`) that changed are the BM25 segment, dictionary, posting, bitmap, query and evaluator code, for the PostgreSQL index format 3 and this fix. No speed-up or slow-down is claimed for this extension. The other changes of 3.1 (the column engine, the aggregate pushdown, the `sift_table` write path, field weights and facet counts, the Vulkan backend) are PostgreSQL features and are not in the SQLite extension.

## 3.0.2

- No change to the storage format (2) or the SQL interface: tables of 3.0.1 open as they are. The extension is rebuilt with the PostgreSQL release 3.0.2. One file of the shared core (`src/portable.list`) changed: `bm25/sift_eval.c`, where a ranked query with no limit (`k` = 0 of `sift_rank_begin`) now ranks every match in one search instead of searching again for 1024 hits, then 16 times as many; the ranked rows and their scores are the same. No speed-up or slow-down is claimed for this extension. The other fixes of 3.0.2 (the column scans, the aggregate pushdown, the keyed ranked scan) are PostgreSQL features and are not in the SQLite extension.

## 3.0.1

- No change to the storage format (2) or the SQL interface: tables of 3.0.0 open as they are. The extension is rebuilt with the PostgreSQL release 3.0.1; no file of the shared core (`src/portable.list`) changed, so nothing in the SQLite extension differs from 3.0.0 except the version `sift_version()` reports. The fixes of 3.0.1 (the column scans, the aggregate pushdown, `sift_table`) are PostgreSQL features and are not in this extension.

## 3.0.0

- No change to the storage format (2) or the SQL interface: tables of 2.0.x open as they are (the host keeps writing segment format 1). The extension is
  rebuilt on the shared core of the PostgreSQL extension 3.0.0, which now lives under `src/core`, `src/bm25` and `src/col` (`src/portable.list`),
  and the write buffer is named as in PostgreSQL. The release gate builds it, checks its instruction set and runs its smoke,
  span and column smoke tests; the full test suite (`sqlite/test.sh`) was not rerun for this release. No speed change is claimed: the measurements are the
  PostgreSQL extension's. The column engine and the aggregate pushdown are PostgreSQL features and are not in the SQLite extension.

## 2.0.2

- No change to the storage format (2) or the SQL interface: tables of 2.0.1 open as they are. The extension is rebuilt on the shared core of
  the PostgreSQL extension 2.0.2, whose posting decoder (`sift_bitmap.c`, `sift_posting.c`) now checks the lengths, counts and offsets of
  the term blob it reads. Security: bytes of a table file that were damaged (a bad disk, a truncated or edited file; the files are only
  writable by the process that owns them, no query can write them) could make a query read outside the blob, write to the stack or never
  end. Such a table now fails the query with a data-corruption error ('corrupt posting data') instead. Nothing to rebuild; a damaged table still has to be
  rebuilt from its source. `tests/test_sift.py` and `tests/test_features.py` pass under ASan and UBSan with the new checks, and the
  stress test under TSan and ASan. No speed change is claimed: the measurements (the instruction gate of the PostgreSQL extension) are not
  SQLite's. The VACUUM fix of the PostgreSQL extension does not apply (SQLite has no shared page store).

## 2.0.1

- No change to the storage format (2) or the SQL interface: tables of 2.0.0 open as they are. The extension is rebuilt on the shared core of
  the PostgreSQL extension 2.0.1, which changes how the shared engine counts documents of segments with vacuumed (deleted) documents: the
  same results. No SQLite speed-up is claimed: the measurements (user instructions per `count(*)` over vacuumed documents, 0.21x for a
  term) are the PostgreSQL extension's. The row-level security fix of the PostgreSQL extension does not apply
  (SQLite has no row-level security).

## 2.0.0

- No change to the storage format (2) or the SQL interface: tables of 1.4.0 open as they are, nothing to rebuild. The extension is rebuilt on the
  shared core of the PostgreSQL extension 2.0.0, whose fixes it takes:
- Queries: a wildcard, regex or fuzzy leaf that expands to more than 512 terms matches every word it matches and scores as one term, like a
  prefix, instead of failing (the shared engine, as in the PostgreSQL extension); a regular expression cannot have back-references (the query
  fails at parse time: rewrite it without one);
  `ORDER BY rank LIMIT` above 67 million rows works.
- Ranking: the score bounds that prune a ranked scan stay valid for queries with more than 256 scored terms.
- Reading: a dictionary whose terms share their first 8 bytes (fixed-width ids, hashes with a common prefix) is searched by bisection instead of
  block by block; a cursor over a sparse term allocates what its rows need instead of 32 kB; an index with damaged offsets in a sparse term
  reports a corrupt term instead of crashing.
- Merges: unchanged. This extension merges with its own streaming code and the shared policy, whose new parameters (`merge_fanin`, the dead
  percentage) keep the values it always used; the memory bound of the PostgreSQL extension's merge (140 MB down to 16 MB for 4 segments of 3M
  documents) is that extension's own and not a SQLite figure.

## 1.4.0

- No change to the storage format (2), the SQL interface or the results: tables of 1.3.x open as they are. The extension now runs on the portable core
  it shares with the PostgreSQL and MariaDB extensions: merges use the shared sorted-run and merge code (`src/sift_runs.c`, `src/sift_merge.c`), deletes the
  shared segment code, and the fake PostgreSQL headers of the old shim are replaced by the host in `sqlite/host/`. Index tables are byte identical to 1.3.1's
  and scores are bit equal (1,242 queries); queries and merges run at the same speed (merge 3.55 s against 3.65 s, peak memory 62 MiB against 68 MiB).
- README: install, usage and the numbers against FTS5, measured without CPU pinning (ranked top-10 AND 89x, OR 242x, phrase 10x faster than FTS5 on 218,058 articles).

## 1.3.1

- No change to the extension; released with the PostgreSQL extension 1.3.1.

## 1.3.0

- First release. `sift` is a SQLite virtual table over the search engine of pg_sift: boolean, phrase, prefix, wildcard, regex and fuzzy queries
  (SiftQL), exact BM25 ranking with block-max pruning pushed into `ORDER BY rank LIMIT k [OFFSET n]`, `sift_count`, highlights, snippets and
  facets. On 218,058 Wikipedia articles ranked top-10 queries run 18x to 287x faster than FTS5 (AND 59x, OR 287x, phrase 18x) with an index 1.05x
  larger ([numbers](README.md#numbers-against-fts5)).
- Content modes: the text inside the table, an external table (`content='tbl'`) or no text at all (`content=''`); the last two store only the
  index (73 percent less space).
- Storage in shadow tables, bounded page cache (`cache_size`), immutable segments published in the SQLite transaction (rollback, savepoints, crash
  and WAL readers are consistent), tiered streaming merges whose memory follows the live documents of the inputs, `optimize`, `merge`, `flush`,
  `rebuild`, `integrity-check`. Storage format 2.
- Threads: one connection per thread, or a connection shared in serialized mode; optional parallel scans (`parallel=N`).
- AVX2 and AVX-512 kernels chosen at load from the CPU (`SIFT_SIMD`, `sift_simd()`), identical scores on every path; the baseline build runs on
  any x86-64 (`make isa-check`), `make SIMD=none` links no vector code.
- Release: `sift-sqlite-<version>-linux-x86_64.tar.gz` (`sift.so`, LICENSE, NOTICE, README) with the release's `SHA256SUMS`, built on Debian 12 for
  glibc 2.34 and newer and smoke-tested on Debian 12, Ubuntu 24.04 and AlmaLinux 10 (`scripts/package-sqlite.sh`). Needs SQLite 3.38 or newer.
- Fixed before release: the baseline build called libgcc's `__popcountdi2`, which a distribution may build with POPCNT (illegal instruction on a
  CPU without it); it now uses the engine's own bit count.
