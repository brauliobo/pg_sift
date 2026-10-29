-- The ids of a dictionary's entries (the key of a group by a text column, the values count(DISTINCT) counts) are made when a tuple first reads the entry, not for the whole
-- dictionary at the start of the statement. The rows must be those of the normal plan with NULLs, empty strings, long strings, a dictionary of one entry, codes that
-- the first chunk does not read (the later ones do) and groups that only a filter keeps.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, grp int, word text, one text, tail text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, i % 7,
       CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN '' WHEN i % 997 = 0 THEN repeat('long', 300) || i ELSE 'w' || (abs(hashint4(i)) % 3000) END,
       'only',
       CASE WHEN i < 20000 THEN 'early' WHEN i % 5 = 0 THEN NULL ELSE 'late' || i % 40 END,
       i % 100
FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (grp, word, one, tail, amount);
VACUUM (FREEZE, ANALYZE) ev;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM (TABLE r_on EXCEPT TABLE r_off) x) + (SELECT count(*) FROM (TABLE r_off EXCEPT TABLE r_on) x);
	RETURN NEXT;
END $$;

SELECT * FROM same('SELECT word, count(*), sum(amount) FROM ev GROUP BY 1');
SELECT * FROM same('SELECT word, grp, count(*) FROM ev GROUP BY 1, 2');
SELECT * FROM same('SELECT one, count(*) AS n, count(DISTINCT word) AS w FROM ev GROUP BY 1');
SELECT * FROM same('SELECT tail, count(*), sum(amount) FROM ev GROUP BY 1');
SELECT * FROM same('SELECT tail, count(DISTINCT word) AS w, count(DISTINCT tail) AS t FROM ev GROUP BY 1');
SELECT * FROM same('SELECT word, count(*) FROM ev WHERE id > 50000 AND amount < 30 GROUP BY 1');
SELECT * FROM same('SELECT grp, count(DISTINCT word) AS w, count(DISTINCT tail) AS t FROM ev GROUP BY ROLLUP (1)');
SELECT * FROM same('SELECT grp, word, count(*) FROM ev GROUP BY GROUPING SETS ((grp), (word), (grp, word))');
SET sift.agg_force = on;
SELECT * FROM same('SELECT word, count(*), sum(amount) FROM ev GROUP BY 1');
SELECT * FROM same('SELECT tail, count(DISTINCT word) FROM ev WHERE grp = 3 GROUP BY 1');
RESET sift.agg_force;
EXPLAIN (COSTS OFF) SELECT word, count(*) FROM ev GROUP BY 1;

DROP FUNCTION same(text);
DROP TABLE ev;
DROP EXTENSION pg_sift;
