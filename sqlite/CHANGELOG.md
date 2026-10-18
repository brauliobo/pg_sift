# Changelog of the SQLite extension

`sift` for SQLite (release artifact `sift-sqlite`) is versioned with the repository: it is released under the repository tag
(`v1.3.0` is `sift-sqlite` 1.3.0) and `sqlite/VERSION` holds the version, which `sift_version()` reports. Changes of the PostgreSQL
extension are in [../CHANGELOG.md](../CHANGELOG.md).

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
