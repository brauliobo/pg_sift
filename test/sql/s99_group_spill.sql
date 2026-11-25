-- The groups of a partition that no level of partitions makes smaller (every record of it has the hash of one group: a group whose strings alone are more than the
-- budget, 60 kB of them at work_mem 64kB) are sorted and merged one hash at a time, so that a statement ends with the rows of PostgreSQL's plan and not with 54000, at work_mem 64kB; a LIMIT and an
-- error in the rows that follow leave no temporary file.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '60s';

CREATE TABLE w (id int PRIMARY KEY, k int NOT NULL, d int NOT NULL, v int NOT NULL, t text NOT NULL);
INSERT INTO w SELECT i, i % 20, i, i % 1000, md5((i / 2)::text) || repeat('x', 60000) FROM generate_series(1, 200) i;
CREATE INDEX w_col ON w USING sift_col (id, k, d, v, t);
VACUUM (FREEZE, ANALYZE) w;

-- 50,000 groups of 4 rows: the 200,000 rows are what makes the groups outgrow the hash memory of 64kB many times over
CREATE TABLE m AS SELECT i AS id, i % 50000 AS k, i % 7 AS d, i % 1000 AS v, md5((i % 50000)::text) AS t FROM generate_series(1, 200000) i;
CREATE INDEX m_col ON m USING sift_col (id, k, d, v, t);
VACUUM (FREEZE, ANALYZE) m;

SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET sift.agg_force = on;

-- the rows of a query, by md5, with SiftAggV6 and with PostgreSQL's aggregate
CREATE FUNCTION rows_md5(q text, sift boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN sift THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT md5(string_agg(x::text, %L ORDER BY x::text)) FROM (%s) x', ',', q) INTO r;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN r;
END $$;

CREATE TABLE cases (name text PRIMARY KEY, q text);
INSERT INTO cases VALUES
	('min of wide strings',   'SELECT k, count(*) AS n, min(t) AS lo FROM w GROUP BY k'),
	('min and max, sums',     'SELECT k, count(*) AS n, sum(v) AS s, min(t) AS lo, max(t) AS hi FROM w GROUP BY k'),
	('wide keys',             'SELECT t, count(*) AS n, sum(v) AS s FROM w GROUP BY t'),
	('distinct, 50,000 groups', 'SELECT k, count(*) AS n, sum(v) AS s, count(DISTINCT d) AS dd FROM m GROUP BY k'),
	('distinct, 3 big groups',  'SELECT id % 3 AS k, count(*) AS n, sum(v) AS s, count(DISTINCT d) AS dd FROM m GROUP BY id % 3');

SELECT name, rows_md5(q, true) = rows_md5(q, false) AS same_rows FROM cases ORDER BY name;

SELECT count(*) AS files_after_full FROM pg_ls_tmpdir();

-- a LIMIT ends the scan of the sorted groups early
SELECT count(*) AS rows_limited FROM (SELECT k, min(t) FROM w GROUP BY k LIMIT 1) q;
SELECT count(*) AS files_after_limit FROM pg_ls_tmpdir();

-- an error in the rows that follow a group ends it as well
DO $$
BEGIN
	PERFORM k, min(t), 1 / (k - 17) FROM w GROUP BY k;
	RAISE NOTICE 'no error';
EXCEPTION WHEN division_by_zero THEN
	RAISE WARNING 'division_by_zero';
END $$;
SELECT count(*) AS files_after_error FROM pg_ls_tmpdir();

DROP FUNCTION rows_md5(text, boolean);
DROP TABLE cases, w, m;
DROP EXTENSION pg_sift;
