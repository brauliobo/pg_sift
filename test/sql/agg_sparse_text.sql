-- A text test the source runs on the encoded form (LIKE '%x%') comes before the conjuncts that decode a leaf, and the leaves are then read, and
-- their dictionaries filled, for the rows it left (a url NOT LIKE tested on a few rows of a unit, not on every distinct url of the segment). The
-- answers stay the heap's whether a unit keeps none of its rows, a few, or most, for a leaf of strings of its own in each unit and a leaf of a few
-- values, with NULLs and empty strings, and when the same leaf is read again for other rows of the unit.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

CREATE TABLE pg (id int PRIMARY KEY, title text, url text, kind text, uid int, phrase text) WITH (autovacuum_enabled = false);
INSERT INTO pg
SELECT i,
       CASE WHEN (i / 700) % 5 = 0 OR i % 331 = 0 THEN 'The Google page ' || i ELSE 'page ' || i END,
       CASE WHEN i % 53 = 0 THEN NULL WHEN i % 101 = 0 THEN '' WHEN i % 7 = 0 THEN 'http://x.google.com/a' || i ELSE 'http://y.example.com/b' || i END,
       CASE WHEN i % 11 = 0 THEN NULL ELSE 'k' || (i % 6) || CASE WHEN i % 4 = 0 THEN '.google.' ELSE '' END END,
       i % 97, 'ph' || (i % 13)
FROM generate_series(1, 30000) i;
CREATE INDEX pg_col ON pg USING sift_col (id, title, url, kind, uid, phrase);
VACUUM ANALYZE pg;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_preagg = off;
SET sift.agg_force = on;

CREATE TABLE s_q (name text, q text);
INSERT INTO s_q VALUES
 ('few rows kept, a leaf of strings', 'SELECT phrase, min(url) AS u, min(title) AS t, count(*) AS n, count(DISTINCT uid) AS d FROM pg WHERE title LIKE ''%Google%'' AND url NOT LIKE ''%.google.%'' AND phrase <> '''' GROUP BY 1'),
 ('few rows kept, a leaf of few values', 'SELECT phrase, min(kind) AS k, count(*) AS n, count(DISTINCT uid) AS d FROM pg WHERE title LIKE ''%Google%'' AND kind NOT LIKE ''%.google.%'' GROUP BY 1'),
 ('no row kept in any unit', 'SELECT phrase, min(url) AS u, count(*) AS n FROM pg WHERE title LIKE ''%nothing here%'' AND url NOT LIKE ''%.google.%'' GROUP BY 1'),
 ('most rows kept', 'SELECT phrase, min(url) AS u, count(*) AS n, count(DISTINCT uid) AS d FROM pg WHERE title LIKE ''%page%'' AND url NOT LIKE ''%.google.%'' GROUP BY 1'),
 ('the tested leaf is the key', 'SELECT url, count(*) AS n FROM pg WHERE title LIKE ''%Google%'' AND url NOT LIKE ''%.google.%'' GROUP BY 1'),
 ('the tested leaf is read for all rows by an OR', 'SELECT phrase, count(*) AS n FROM pg WHERE title LIKE ''%Google%'' AND (url NOT LIKE ''%.google.%'' OR kind LIKE ''k1%'' OR id % 5 = 0) GROUP BY 1'),
 ('the tested leaf is read again for more rows', 'SELECT phrase, count(*) FILTER (WHERE url NOT LIKE ''%.google.%'') AS f, count(DISTINCT url) AS d FROM pg WHERE title LIKE ''%Google%'' GROUP BY 1'),
 ('null and empty strings', 'SELECT url IS NULL AS isnull, url = '''' AS e, count(*) AS n FROM pg WHERE title LIKE ''%Google%'' AND (url NOT LIKE ''%.google.%'' OR url IS NULL) GROUP BY 1, 2'),
 ('two text tests and a number', 'SELECT phrase, count(*) AS n, sum(uid) AS s FROM pg WHERE title LIKE ''%Google%'' AND title NOT LIKE ''%331%'' AND url NOT LIKE ''%/b1%'' AND uid > 10 GROUP BY 1'),
 ('count without a group', 'SELECT count(*) AS n, count(DISTINCT uid) AS u, min(url) AS m FROM pg WHERE title LIKE ''%Google%'' AND url NOT LIKE ''%.google.%''');

-- the scan runs (SiftAgg), and its rows are the heap's
CREATE FUNCTION s_scan(qq text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || qq LOOP
		IF l ~ 'SiftAgg' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE FUNCTION s_diff(qq text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.differences_agg_temp(qq) $$;
SELECT name, s_scan(q) AS scan, s_diff(q) AS differing_rows FROM s_q ORDER BY name;

RESET sift.enable_agg;
DROP FUNCTION s_scan(text), s_diff(text);
DROP TABLE s_q, pg;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
