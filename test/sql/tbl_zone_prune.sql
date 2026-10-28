-- The aggregate scan of a sift_table skips the units whose zones exclude the filters of the statement, as it does for a sift_col index: a range, an IN list, NULL
-- tests, an OR, on a table whose superblocks are ordered by the filter column. Rows in the write buffer, deleted rows, and a column added after the first
-- groups were written (those units hold it as NULLs, which a zone cannot tell) give the rows of the normal plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

-- a page holds one row of the padding: about 12,000 rows to a superblock of a table, so 6 superblocks
CREATE TABLE zp (id int, k int, j int, v bigint) USING sift_table;
INSERT INTO zp SELECT i, i, CASE WHEN i % 5000 < 1000 THEN NULL ELSE i % 100 END, i * 3 FROM generate_series(1, 400000) i;
VACUUM (FREEZE, ANALYZE) zp;

\ir test/agg_rows_helpers.sql
CREATE FUNCTION units_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; pruned text := '0';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l ~ '(\d+) pruned' THEN pruned := substring(l from '(\d+) pruned'); END IF;
	END LOOP;
	RETURN CASE WHEN r = 'normal' THEN r WHEN pruned::int > 0 THEN 'SiftAgg, units pruned' ELSE 'SiftAgg, none pruned' END;
END $$;

CREATE TABLE zp_q (q text);
INSERT INTO zp_q VALUES
	('SELECT count(*), sum(v) FROM zp WHERE k BETWEEN 200000 AND 201000'),
	('SELECT j, count(*) FROM zp WHERE k >= 399000 GROUP BY j'),
	('SELECT count(*) FROM zp WHERE k IN (5, 150000, 399999)'),
	('SELECT count(*) FROM zp WHERE k < 100 OR k > 399900'),
	('SELECT count(*), sum(v) FROM zp WHERE k = 123456'),
	('SELECT count(*), sum(v) FROM zp WHERE k > 1000000'),
	('SELECT count(*) FROM zp WHERE j IS NULL AND k > 390000'),
	('SELECT count(*) FROM zp WHERE j IS NOT NULL AND k < 2000');

SELECT units_of(q) AS plan, agg_rows(q, true) = agg_rows(q, false) AS same FROM zp_q ORDER BY q;

-- write buffer rows and tombstones
INSERT INTO zp SELECT 400000 + i, 400000 + i, i % 100, i FROM generate_series(1, 3000) i;
DELETE FROM zp WHERE k BETWEEN 200100 AND 200200;
UPDATE zp SET k = 5 WHERE k BETWEEN 300000 AND 300010;
SELECT units_of(q) AS plan, agg_rows(q, true) = agg_rows(q, false) AS same FROM zp_q ORDER BY q;

-- a column added later: the groups written before it hold it as NULLs (and the default of ADD COLUMN .. DEFAULT)
ALTER TABLE zp ADD COLUMN w int;
ALTER TABLE zp ADD COLUMN u int DEFAULT 7;
INSERT INTO zp SELECT 500000 + i, 500000 + i, i % 100, i, i, i FROM generate_series(1, 20000) i;
VACUUM zp;
DELETE FROM zp_q;
INSERT INTO zp_q VALUES
	('SELECT count(*), sum(v) FROM zp WHERE w BETWEEN 100 AND 200'),
	('SELECT count(*) FROM zp WHERE w IS NULL'),
	('SELECT count(*) FROM zp WHERE w IS NULL AND k < 1000'),
	('SELECT count(*) FROM zp WHERE u = 7 AND k < 1000'),
	('SELECT count(*) FROM zp WHERE u BETWEEN 5 AND 8 OR k > 510000'),
	('SELECT count(*) FROM zp WHERE (w > 19000 OR u < 3) AND k > 300000'),
	('SELECT count(*), sum(v) FROM zp WHERE k BETWEEN 200000 AND 201000'),
	('SELECT count(*) FROM zp WHERE k < 100 OR w > 19990');
SELECT units_of(q) AS plan, agg_rows(q, true) = agg_rows(q, false) AS same FROM zp_q ORDER BY q;

DROP TABLE zp_q, zp;
DROP FUNCTION agg_pushed(text), agg_rows(text, bool), units_of(text);
DROP EXTENSION pg_sift;
