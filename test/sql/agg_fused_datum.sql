-- A grouped scan of a column read as datums (macaddr, inet, timetz) that runs next to a scalar sub-select over the same index answers with the rows of PostgreSQL's own plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE fd (id int, g int, mac macaddr, tz timetz);
INSERT INTO fd SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE (ARRAY['08:00:2b:01:02:03', 'aa:bb:cc:dd:ee:ff', 'ff:ff:ff:ff:ff:ff'])[1 + i % 3]::macaddr END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE ((i % 24) || ':00:00+01')::timetz END FROM generate_series(1, 6000) i;
CREATE INDEX fd_col ON fd USING sift_col (id, g, mac, tz);
VACUUM ANALYZE fd;

CREATE TABLE got AS SELECT (SELECT count(*) FROM (SELECT tz FROM fd GROUP BY tz) t) AS tz_groups, (SELECT count(*) FROM (SELECT mac FROM fd GROUP BY mac) m) AS mac_groups,
	(SELECT count(*) FROM (SELECT g FROM fd GROUP BY g) k) AS g_groups, (SELECT count(*) FROM fd WHERE mac = 'ff:ff:ff:ff:ff:ff') AS ff_rows;
SET sift.enable_agg = off;
CREATE TABLE want AS SELECT (SELECT count(*) FROM (SELECT tz FROM fd GROUP BY tz) t) AS tz_groups, (SELECT count(*) FROM (SELECT mac FROM fd GROUP BY mac) m) AS mac_groups,
	(SELECT count(*) FROM (SELECT g FROM fd GROUP BY g) k) AS g_groups, (SELECT count(*) FROM fd WHERE mac = 'ff:ff:ff:ff:ff:ff') AS ff_rows;
RESET sift.enable_agg;

SELECT * FROM got;
SELECT (SELECT count(*) FROM (TABLE got EXCEPT TABLE want) a) AS extra;
SELECT count(*) AS mac_groups, (SELECT count(*) FROM (SELECT tz FROM fd GROUP BY tz) g) AS tz_groups FROM (SELECT mac FROM fd GROUP BY mac) g;

DROP TABLE got, want, fd;
DROP EXTENSION pg_sift;
