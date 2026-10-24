# Changelog of the MariaDB extension

`sift` for MariaDB (release artifact `sift-mariadb`) is versioned with the repository: it is released under the repository tag
(`v1.4.0` is `sift-mariadb` 1.4.0) and `mariadb/VERSION` holds the version, which `sift_version()` reports. Changes of the PostgreSQL
extension are in [../CHANGELOG.md](../CHANGELOG.md), those of the SQLite extension in [../sqlite/CHANGELOG.md](../sqlite/CHANGELOG.md).

## Unreleased

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

- No change to the index format (2) or the SQL files: an index of 3.0.2 opens as it is, and this extension still writes BM25 segment format 1. The library is rebuilt with the PostgreSQL release 3.1.0 and `sift_version()` reports pages 1.3 (what the build reads: the segment format 3 that PostgreSQL 3.1 writes is read by the shared core, which this extension never writes).
- Fixed, shared core: a wide wildcard, regex or fuzzy leaf (more terms than the expansion limit) next to an optional phrase (`/lev.*/ OR "two words"`) lost its score for the documents that matched it, so the ranking differed from the documented one; counts and the set of matches were right. The files of the shared core (`src/portable.list`) that changed are the BM25 segment, dictionary, posting, bitmap, query and evaluator code, for the PostgreSQL index format 3 and this fix. No speed-up or slow-down is claimed for this extension. The other changes of 3.1 (the column engine, the aggregate pushdown, the `sift_table` write path, field weights and facet counts, the Vulkan backend) are PostgreSQL features and are not in the MariaDB extension.

## 3.0.2

- No change to the index format (2) or the SQL files: an index of 3.0.1 opens as it is. The library is rebuilt with the PostgreSQL release 3.0.2. One file of the shared core (`src/portable.list`) changed: `bm25/sift_eval.c`, where a ranked query with no limit (`k` = 0 of `sift_rank_begin`) now ranks every match in one search instead of searching again for 1024 hits, then 16 times as many; the ranked rows and their scores are the same. No speed-up or slow-down is claimed for this extension. The other fixes of 3.0.2 (the column scans, the aggregate pushdown, the keyed ranked scan) are PostgreSQL features and are not in the MariaDB extension.

## 3.0.1

- No change to the index format (2) or the SQL files: an index of 3.0.0 opens as it is. The library is rebuilt with the PostgreSQL release 3.0.1; no file of the shared core (`src/portable.list`) changed, so nothing in the MariaDB extension differs from 3.0.0 except the version `sift_version()` reports. The fixes of 3.0.1 (the column scans, the aggregate pushdown, `sift_table`) are PostgreSQL features and are not in this extension.

## 3.0.0

- No change to the index format (2) or the SQL files: an index of 2.0.x opens as it is (the host keeps writing segment format 1). The library is
  rebuilt on the shared core of the PostgreSQL extension 3.0.0, which now lives under `src/core`, `src/bm25` and `src/col` (`src/portable.list`), and the write
  buffer is named as in PostgreSQL (`docs/NAMING.md`). The release gate builds it in the `mariadb:11.8` image and loads it in that server, and the
  release workflow builds every series; the full MariaDB test suite (`mariadb/test.sh`) was not rerun for this release. No speed change is claimed: the
  measurements are the PostgreSQL extension's. The column engine and the aggregate pushdown are PostgreSQL features and are not in the MariaDB extension.

## 2.0.2

- No change to the index format (2) or the SQL files: an index of 2.0.1 opens as it is. The library is rebuilt on the shared core of the
  PostgreSQL extension 2.0.2, whose posting decoder (`sift_bitmap.c`, `sift_posting.c`) now checks the lengths, counts and offsets of the
  term blob it reads. Security: segment files that were damaged (a bad disk, a truncated or edited file; they sit in the data directory,
  outside InnoDB, and are only writable by the server's OS user) could make a query read outside the blob, write to the stack or never end.
  Such an index now fails the query with a data-corruption error ('corrupt posting data') instead, and `CALL sift_rebuild` rebuilds it from the table. Nothing
  to rebuild otherwise. The functional, live (kill -9 during a merge) and stress tests pass on MariaDB 13.0.2. No speed change is claimed:
  the measurements (the instruction gate of the PostgreSQL extension) are not MariaDB's. The VACUUM fix of the PostgreSQL extension does not
  apply (MariaDB has no shared page store).

## 2.0.1

- No change to the index format (2) or the SQL files: an index of 2.0.0 opens as it is, nothing to rebuild with `sift_rebuild`. The library is
  rebuilt on the shared core of the PostgreSQL extension 2.0.1, which changes how the shared engine counts documents of segments with
  vacuumed (deleted) documents: the same results. No MariaDB speed-up is claimed: the measurements (user
  instructions per `count(*)` over vacuumed documents, 0.21x for a term) are the PostgreSQL extension's. The row-level security fix of the
  PostgreSQL extension does not apply (the planner hook is PostgreSQL's).

## 2.0.0

- No change to the index format (2) or the SQL files: an index of 1.4.0 opens as it is, nothing to rebuild with `sift_rebuild`. The library is rebuilt
  on the shared core of the PostgreSQL extension 2.0.0, whose fixes it takes:
- Queries: a wildcard, regex or fuzzy leaf that expands to more than 512 terms matches every word it matches and scores as one term, like a
  prefix, instead of failing; a regular expression cannot have back-references (the query fails at parse time: rewrite it without one).
- Ranking: the score bounds that prune a ranked scan stay valid for queries with more than 256 scored terms.
- Reading: a dictionary whose terms share their first 8 bytes (fixed-width ids, hashes with a common prefix) is searched by bisection instead of
  block by block; a cursor over a sparse term allocates what its rows need instead of 32 kB; an index with damaged offsets in a sparse term
  reports a corrupt term instead of crashing.
- Merges: unchanged. This extension merges with its own streaming code (memory bounded by the number of segments merged at once, as in 1.4.0) and
  the shared policy, whose new parameters (`merge_fanin`, the dead percentage) keep the values it always used; the memory bound of the PostgreSQL
  extension's merge (140 MB down to 16 MB for 4 segments of 3M documents) is that extension's own and not a MariaDB figure.

## 1.4.0

- First release. `sift` is a UDF library (`libsift_udf.so`) and two SQL files that add the search engine of pg_sift to MariaDB: boolean,
  phrase, prefix, wildcard, regex and fuzzy queries (SiftQL), exact BM25 ranking with block-max pruning (`sift_topk`), `sift_count`,
  `sift_matches`, filters and facets, over any InnoDB table with an integer, text, UUID or composite key. For the same text and options the
  counts, the top-10 order and the float32 scores are bit identical to pg_sift's (5,098 queries on 218,058 articles). Ranked top-10 queries
  run 200x to 1,000x faster than InnoDB FULLTEXT with an index 1.7x smaller ([numbers](README.md#numbers-against-innodb-fulltext)).
- A trigger-fed queue keeps the index in step with the table; reads see committed DML at once (`fresh`, `recheck`), `sift_sync` drains the
  queue into immutable segments, tiered streaming merges (memory follows the live documents), crash-safe commit by manifest rename, replicas
  rebuild from the binary log.
- Built on the portable core shared with the PostgreSQL and SQLite extensions (`src/portable.list`), against the host in `mariadb/host/`:
  the sources of the core are compiled as they are, there is no PostgreSQL code or header in the build.
- Release: `sift-mariadb-<version>-linux-x86_64.tar.gz` with one `libsift_udf.so` per MariaDB series (the library calls the server's own
  symbols and refuses another series), the two SQL files, LICENSE, NOTICE, README and `BUILD-INFO` (server version and glibc of each
  library), built in the stock `mariadb:<series>` images against the headers of their servers and loaded into a server of the same image
  before release (`scripts/package-mariadb.sh`).
