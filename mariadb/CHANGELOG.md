# Changelog of the MariaDB extension

`sift` for MariaDB (release artifact `sift-mariadb`) is versioned with the repository: it is released under the repository tag
(`v1.4.0` is `sift-mariadb` 1.4.0) and `mariadb/VERSION` holds the version, which `sift_version()` reports. Changes of the PostgreSQL
extension are in [../CHANGELOG.md](../CHANGELOG.md), those of the SQLite extension in [../sqlite/CHANGELOG.md](../sqlite/CHANGELOG.md).

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
