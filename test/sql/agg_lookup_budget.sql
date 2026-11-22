-- The rows of a sub-select joined to the aggregate scan are bounded by memory (half of work_mem: the estimated rows times their width plus 64 bytes a row), not by a
-- row count: a sub-select the planner expects to return 2,000,000 rows (the statistics of the table are set to that) is a lookup table of the scan once work_mem allows it.
-- The rows are the normal plan's in every case.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, n int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 500, i % 100 FROM generate_series(1, 20000) i;
CREATE INDEX ev_col ON ev USING sift_col (n, amount);
CREATE TABLE bigdim (id int, k int) WITH (autovacuum_enabled = false);
INSERT INTO bigdim SELECT i, i % 7 FROM generate_series(0, 999) i;
VACUUM (FREEZE, ANALYZE) ev, bigdim;
-- the planner expects 2,000,000 rows of it: reltuples over the pages it has
UPDATE pg_class SET reltuples = 2000000 WHERE relname = 'bigdim';

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0;
END $$;
SELECT format('SELECT d.k, count(*) AS c, sum(amount) AS s FROM ev JOIN (SELECT id, k FROM bigdim LIMIT %s) d ON d.id = ev.n GROUP BY d.k', 5000000) AS q \gset

SHOW work_mem;
SELECT arm, used, slug FROM sift.explain_why(:'q') WHERE arm = 'single';
SELECT same(:'q') AS default_budget;

-- 2,000,000 rows of 8 bytes and the 64 of a row are 137 MB: half of 128 MB is under them, half of 512 MB over
SET work_mem = '128MB';
SELECT arm, used, slug FROM sift.explain_why(:'q') WHERE arm = 'single';
SELECT same(:'q') AS small_budget;
SET work_mem = '512MB';
SELECT arm, used, slug FROM sift.explain_why(:'q') WHERE arm = 'single';
RESET work_mem;

DROP FUNCTION same(text);
DROP TABLE ev, bigdim;
DROP EXTENSION pg_sift;
