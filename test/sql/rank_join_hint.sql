-- A join above a ranked scan keeps only some of the rows the scan hands up, so the scan is asked for more rows than the LIMIT: the
-- fraction the other relations' restrictions leave, with a margin, in one search. With the limit as the hint the scan was drained past
-- it, and every further search scored every block again (10, then 160, then 2560 hits).
CREATE EXTENSION pg_sift;

CREATE FUNCTION limit_hint(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Limit Hint: ' THEN RETURN substring(l from 'Limit Hint: (\d+)')::int; END IF;
	END LOOP;
	RETURN 0;
END $$;

CREATE TABLE cat (id int PRIMARY KEY, region int);
INSERT INTO cat SELECT i, i % 10 FROM generate_series(1, 200) i;
CREATE TABLE jd (id int PRIMARY KEY, cat_id int, body text) WITH (autovacuum_enabled = off);
INSERT INTO jd SELECT i, 1 + i % 200, 'common w' || i % 100 || ' x' || i % 7 || ' y' || i % 13 FROM generate_series(1, 20000) i;
CREATE INDEX jd_idx ON jd USING sift (body);
ANALYZE cat, jd;

-- no restriction on the joined table: the LIMIT; a restriction that keeps a tenth of it: about ten times as many rows, never fewer
SELECT limit_hint($$SELECT jd.id FROM jd JOIN cat ON cat.id = jd.cat_id WHERE jd.body @@> 'common w5 OR x3' ORDER BY sift.score(jd.ctid) DESC LIMIT 10$$) AS hint_no_restriction;
SELECT limit_hint($$SELECT jd.id FROM jd JOIN cat ON cat.id = jd.cat_id WHERE jd.body @@> 'common w5 OR x3' AND cat.region = 3 ORDER BY sift.score(jd.ctid) DESC LIMIT 10$$) BETWEEN 100 AND 400 AS hint_tenth;
SELECT limit_hint($$SELECT jd.id FROM jd WHERE jd.body @@> 'common w5 OR x3' ORDER BY sift.score(jd.ctid) DESC LIMIT 10$$) AS hint_no_join;
-- a left join keeps every row of the scan
SELECT limit_hint($$SELECT jd.id FROM jd LEFT JOIN cat ON cat.id = jd.cat_id AND cat.region = 3 WHERE jd.body @@> 'common w5 OR x3' ORDER BY sift.score(jd.ctid) DESC LIMIT 10$$) AS hint_left_join;

-- the rows are the best ones: the same as ranking every match and joining afterwards
SELECT count(*) AS queries,
       count(*) FILTER (WHERE got IS DISTINCT FROM want) AS different
FROM (SELECT q.w, q.r,
             (SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT jd.id, sift.score(jd.ctid) s FROM jd JOIN cat ON cat.id = jd.cat_id
                WHERE jd.body @@> q.w AND cat.region = q.r ORDER BY sift.score(jd.ctid) DESC LIMIT 10) t) AS got,
             (SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, s FROM (SELECT jd.id, sift.score(jd.ctid) s, cat.region FROM jd JOIN cat ON cat.id = jd.cat_id
                WHERE jd.body @@> q.w ORDER BY sift.score(jd.ctid) DESC LIMIT 1000000) a WHERE region = q.r ORDER BY s DESC, id LIMIT 10) t) AS want
      FROM (SELECT 'common w' || a || ' OR x' || b AS w, r FROM generate_series(0, 9) a, generate_series(0, 6) b, generate_series(0, 9, 3) r) q) z;

DROP FUNCTION limit_hint(text);
DROP TABLE jd, cat;
DROP EXTENSION pg_sift;
