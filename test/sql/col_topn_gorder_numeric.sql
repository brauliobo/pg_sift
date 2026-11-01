-- ORDER BY a numeric(p <= 18, s) column LIMIT n [OFFSET m] is read from the global order of the column (its key is the number scaled by 10^s) like an integer key: the answers are those of
-- a copy of the table without an index (the oracle) over NULLs, ties, DESC, NULLS FIRST and LAST, a second key, OFFSET, negative values, scales 0 and 6, deleted and updated rows, the write
-- buffer and a promotion; a column with a NaN, a bound on the key and a wide numeric read the units.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE tt AS SELECT i AS id, CASE WHEN i % 31 = 0 THEN NULL ELSE (((i::bigint * 7919) % 30011) - 15000)::numeric / 100 END::numeric(12,2) AS n,
	((((i::bigint * 104729) % 2003) - 1000)::numeric / 7)::numeric(18,6) AS m, ((i * 13) % 17)::numeric(5,0) AS z, i % 5 AS g
FROM generate_series(1, 30000) i;
CREATE INDEX tt_i ON tt USING sift_col (id, n, m, z, g) WITH (order_columns = 'n, m, z', order_array_columns = 'n, m, z', write_buffer_limit = 256);
VACUUM ANALYZE tt;
CREATE TABLE tp AS SELECT * FROM tt;
ANALYZE tp;

CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Global Order: true' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(id::text, '','') FROM (%s) x', replace(q, '@', 'tt')) INTO a;
	EXECUTE format('SELECT string_agg(id::text, '','') FROM (%s) x', replace(q, '@', 'tp')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: %', q, left(a, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE TABLE tq (n int, q text);
INSERT INTO tq SELECT row_number() OVER (), q FROM unnest(ARRAY[
 'SELECT id FROM @ ORDER BY n, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ ORDER BY n DESC, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ ORDER BY n NULLS FIRST, id LIMIT 20 OFFSET 40',
 'SELECT id FROM @ ORDER BY n DESC NULLS LAST, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ ORDER BY n, id LIMIT 500', 'SELECT id FROM @ ORDER BY n, id DESC LIMIT 20 OFFSET 3000',
 'SELECT id FROM @ ORDER BY n LIMIT 20 OFFSET 12000', 'SELECT id FROM @ ORDER BY n, id LIMIT 20 OFFSET 29900', 'SELECT id FROM @ ORDER BY m, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ ORDER BY m DESC, id LIMIT 20 OFFSET 2500',
 'SELECT id FROM @ ORDER BY z, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ ORDER BY z DESC, m, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ ORDER BY n, m, id LIMIT 20 OFFSET 40',
 'SELECT id, n FROM @ ORDER BY n, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ WHERE g = 2 ORDER BY n, id LIMIT 20 OFFSET 40', 'SELECT id FROM @ WHERE n > 0 ORDER BY n, id LIMIT 20 OFFSET 40',
 'SELECT id FROM @ WHERE n IS NOT NULL ORDER BY n, id LIMIT 20 OFFSET 40']) q;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;

-- rows that changed: updated, deleted, a rolled-back update, the write buffer, a promotion
UPDATE tt SET n = 3.5 WHERE id % 500 = 3;
UPDATE tp SET n = 3.5 WHERE id % 500 = 3;
UPDATE tt SET n = NULL WHERE id % 900 = 4;
UPDATE tp SET n = NULL WHERE id % 900 = 4;
DELETE FROM tt WHERE id % 700 = 9;
DELETE FROM tp WHERE id % 700 = 9;
BEGIN; UPDATE tt SET n = -999 WHERE id % 300 = 7; ROLLBACK;
INSERT INTO tt SELECT 30000 + i, CASE WHEN i % 10 = 0 THEN NULL ELSE (i % 100)::numeric / 10 END, ((i % 7)::numeric / 3)::numeric(18,6), i % 17, 3 FROM generate_series(1, 400) i;
INSERT INTO tp SELECT 30000 + i, CASE WHEN i % 10 = 0 THEN NULL ELSE (i % 100)::numeric / 10 END, ((i % 7)::numeric / 3)::numeric(18,6), i % 17, 3 FROM generate_series(1, 400) i;
SELECT n, same(q) AS answers FROM tq ORDER BY n;
SELECT sift.col_promote('tt_i') >= 0 AS promoted;
SELECT n, same(q) AS answers FROM tq ORDER BY n;

-- a NaN leaves its segment without an array, and a NaN in the write buffer reads the units: the answers are still the oracle's, NaN sorting after every number
INSERT INTO tt VALUES (40001, 'NaN', 1, 1, 1), (40002, 12.5, 1, 1, 1);
INSERT INTO tp VALUES (40001, 'NaN', 1, 1, 1), (40002, 12.5, 1, 1, 1);
SELECT n, same(q) AS answers FROM tq WHERE n IN (1, 2, 5, 7) ORDER BY n;
SELECT sift.col_promote('tt_i') >= 0 AS promoted;
SELECT n, same(q) AS answers FROM tq WHERE n IN (1, 2, 5, 7) ORDER BY n;

DROP TABLE tq, tt, tp;
DROP FUNCTION global_order(text), same(text);
DROP EXTENSION pg_sift;
