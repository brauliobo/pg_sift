## MariaDB: BM25 search as a UDF library

`mariadb/` brings the pg_sift engine to MariaDB 10.5 to 13.x (tested on 13.0.2) as `libsift_udf.so` plus two SQL files: ranked
top-k, counts, phrases, prefix / wildcard / fuzzy / regular expression terms, boosts, fields, filters and facets, on InnoDB
tables, with results (counts, top-10 order, scores) identical to pg_sift. No storage engine, no server patch: triggers feed a
queue, the index lives in files next to the data directory, queries see writes at once. Full guide: [mariadb/README.md](mariadb/README.md).

```sh
nice make -C mariadb -j4 OUT=/tmp/sift-build               # needs the MariaDB and PostgreSQL server headers
cp /tmp/sift-build/libsift_udf.so "$(mariadb -NBe 'SELECT @@plugin_dir')"/
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

Against InnoDB FULLTEXT (218,058 Simple English Wikipedia articles, one client, warm cache, the host as it is, median of 200
queries per class, host load average 11 to 20 so absolute values move by about 50%):

| class | sift | InnoDB FULLTEXT | InnoDB / sift |
| --- | --- | --- | --- |
| AND top-10 | 0.30 ms | 62 ms | 205x |
| OR top-10 | 1.25 ms | 908 to 1,232 ms | 725x to 984x |
| phrase top-10 | 0.24 ms | 175 ms | 726x |
| count AND / OR | 0.10 / 0.27 ms | 65 / 916 ms | 632x / 3,411x |
| build | 7.0 s | 44.9 s | 6.4x |
| index size | 135.9 MB | 232.0 MB | 1.7x smaller |

Writes are visible to the next query by default; that costs 20 to 170 us per top-10 (15 to 75 us per count), and an index
created with `fresh=false, recheck=false` is as fast as the version without it. MariaDB, SQLite and PostgreSQL numbers are
measured on different systems and are not comparable with one another.
