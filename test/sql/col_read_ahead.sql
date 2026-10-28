-- A scan whose pages are not in shared buffers reads the heads of the units it will read next through a ReadStream (sift_sr_ahead), across the
-- few units a morsel holds. sift.test_ahead_cold makes every reader behave as one that missed, so the stream runs here: the rows must be those of the
-- normal plan whether the scan keeps all units, a few far apart (the ranges below skip most of them) or only some rows of each, serially or with workers,
-- and with a second segment in the index.
CREATE EXTENSION pg_sift;

SET sift.agg_preagg = off;
SET sift.agg_force = on;
SET sift.test_ahead_cold = on;

CREATE TABLE ev (id int PRIMARY KEY, grp int, amount int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 7, (i * 31) % 1000, 'p' FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, grp, amount);
VACUUM (FREEZE, ANALYZE) ev;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM (TABLE r_on EXCEPT TABLE r_off) x) + (SELECT count(*) FROM (TABLE r_off EXCEPT TABLE r_on) x);
	RESET sift.enable_agg;
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE q (name text, q text);
INSERT INTO q VALUES
	('all units', 'SELECT grp, count(*), sum(amount) FROM ev GROUP BY 1'),
	('one range', 'SELECT grp, count(*), sum(amount) FROM ev WHERE id BETWEEN 5000 AND 9000 GROUP BY 1'),
	('ranges far apart', 'SELECT grp, count(*), sum(amount) FROM ev WHERE id BETWEEN 100 AND 300 OR id BETWEEN 12000 AND 12200 OR id BETWEEN 25000 AND 25300 OR id > 39800 GROUP BY 1'),
	('filter on a measure', 'SELECT grp, count(*), sum(amount) FROM ev WHERE amount < 40 GROUP BY 1');

SELECT name, (same(q)).* FROM q ORDER BY name;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SET max_parallel_workers_per_gather = 2;
SELECT name, (same(q)).* FROM q ORDER BY name;
RESET max_parallel_workers_per_gather;

-- a second segment
INSERT INTO ev SELECT i, i % 7, (i * 31) % 1000, 'p' FROM generate_series(40001, 60000) i;
VACUUM ev;
SELECT name, (same(q)).* FROM q ORDER BY name;

DROP FUNCTION same(text);
DROP TABLE q, ev;
DROP EXTENSION pg_sift;
