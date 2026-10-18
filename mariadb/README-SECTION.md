## MariaDB: BM25 search as a UDF library

`mariadb/` brings the pg_sift engine to MariaDB 10.5 to 13.x as `libsift_udf.so` plus two SQL files (release tarballs carry a tested
library for 10.11, 11.4, 11.8, 12.1 and 13.0): ranked top-k, counts, phrases, prefix / wildcard / fuzzy / regular expression terms,
boosts, fields, filters and facets, on InnoDB tables, with results (counts, top-10 order, scores) identical to pg_sift. It is the portable
core (`src/portable.list`) compiled unmodified over a small host in `mariadb/host/`. No storage engine, no server patch: triggers feed a
queue, the index lives in files next to the data directory, queries see writes at once. Full guide: mariadb/README.md.

```sh
nice make -C mariadb -j4 OUT=/tmp/sift-build               # needs the MariaDB server headers (libmariadbd-dev), no PostgreSQL
cp /tmp/sift-build/libsift_udf.so "$(mariadb -NBe 'SELECT @@plugin_dir')"/     # or lib/<series>/ of a release tarball
mariadb mydb < mariadb/sql/functions.sql && mariadb mydb < mariadb/sql/sift.sql
```

```sql
CALL sift_attach('news', 'articles', 'id', "CONCAT_WS(' ', t.title, t.body)", 32);   -- index, queue, triggers
SELECT a.id, a.title, j.s
FROM (SELECT sift_topk('news', 'solar OR "power plant"', 10) AS r) x,
     JSON_TABLE(x.r, '$[*]' COLUMNS (n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')) j
JOIN articles a ON a.id = j.id ORDER BY j.n;
SELECT sift_count('news', 'power AND plant');          CALL sift_sync('news');   -- drain the queue into a segment
```

| class | sift | InnoDB FULLTEXT | InnoDB / sift |
| --- | --- | --- | --- |
| AND top-10 | 0.48 ms | 87 ms | 183x |
| OR top-10 | 1.30 ms | 944 to 1,033 ms | 725x to 794x |
| phrase top-10 | 0.42 ms | 214 ms | 504x |
| count AND / OR | 0.11 / 0.27 ms | 113 / 1,506 ms | 1,025x / 5,608x |
| build | 6.6 s rebuild, 10.9 s first drain | 58.9 s | 8.9x, 5.4x |
| index size | 135.9 MB | 232.0 MB | 1.7x smaller |

Writes are visible to the next query by default; that costs 70 to 180 us per top-10 (20 to 30 us per count), and an index created with
`fresh=false, recheck=false` skips it. Moving the engine onto the shared core changed nothing measurable: interleaved before / after
rounds put every cell within the noise (mean ratio 0.97), the index is the same size, and the 5,098-query comparison with pg_sift has 0
differences. MariaDB, SQLite and PostgreSQL numbers are measured on different systems and are not comparable with one another.
