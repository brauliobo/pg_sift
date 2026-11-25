-- Expression caches must retain every input, including NULLs and inputs after the third.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, a int, b int, c int, d int, e int, f int, g int, h int, payload text);
INSERT INTO events
SELECT i, i % 7, i % 3, i % 5, i % 2, i % 7, i % 3, i % 5,
       CASE WHEN i % 11 = 0 THEN NULL ELSE i % 2 END, repeat('payload', 50)
FROM generate_series(1, 50000) i;
CREATE INDEX events_col ON events USING sift_col (id, a, b, c, d, e, f, g, h);
CREATE TABLE labels (id int, label text);
INSERT INTO labels SELECT i, 'v' || i FROM generate_series(0, 6) i;
INSERT INTO labels VALUES (1, 'duplicate'), (NULL, NULL);
VACUUM (FREEZE, ANALYZE) events;
ANALYZE labels;
\ir test/agg_check_pushed.sql
SELECT * FROM check_query($q$
 SELECT concat_ws(':', a, b, c, d) AS key, count(*) AS n, count(DISTINCT id) AS ids
 FROM events GROUP BY 1
$q$);
SELECT * FROM check_query($q$
 SELECT concat_ws(':', a, b, c, d, e, f, g, h) AS key, count(*) AS n,
        count(DISTINCT id) FILTER (WHERE concat_ws(':', a, b, c, d, e, f, g, h) LIKE '0:%') AS ids
 FROM events GROUP BY 1
$q$);
SELECT * FROM check_query($q$
 SELECT concat_ws(':', f.a, f.b, f.c, f.d, f.e, f.f, f.g, f.h, l.label) AS key,
        count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f LEFT JOIN labels l ON l.id = f.a GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE events, labels;
DROP EXTENSION pg_sift;
