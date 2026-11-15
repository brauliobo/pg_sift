-- An IN list on the key of a top-N, an OFFSET or a page is read from the order array one value after another: a seek to the entries of each value in every source (the segments and the
-- write buffer), not a range from the first value to the last that tests every entry between, or the units. The rows are the heap's in both directions, with ties (a key of many rows), NULL
-- keys, values the table does not have, repeated values, a bound or another condition beside the list, rows in a write buffer, updated, deleted and vacuumed rows; the walk decodes
-- the entries of the values it reads, not those of the table.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET synchronous_commit = on;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gi (id int PRIMARY KEY, u bigint, k int, pad text DEFAULT repeat('x', 60)) WITH (autovacuum_enabled = off);
ALTER TABLE gi ALTER COLUMN pad SET STORAGE PLAIN;
-- u: unique and scattered; k: 300 keys of about 330 rows, a tenth NULL
INSERT INTO gi (id, u, k) SELECT i, (i * 104729::bigint) % 1000003, CASE WHEN i % 10 = 0 THEN NULL ELSE (i * 7919::bigint) % 300 END FROM generate_series(1, 100000) i;
CREATE INDEX gi_i ON gi USING sift_col (id, u, k) WITH (order_columns = 'u, k', order_array_columns = 'u, k', key_cut = 'off');
VACUUM (FREEZE, ANALYZE) gi;

-- the lists: 2, 5, 17 and 40 values of the table, 40 that it has not, 1,000 and 1,025 of it, and 10 keys of many rows
CREATE TABLE gi_l AS
SELECT 'u' || n AS name, (SELECT string_agg((((i * (80000 / n) + 1) * 104729::bigint) % 1000003)::text, ',') FROM generate_series(1, n) i) AS vals FROM unnest(ARRAY[2, 5, 17, 40, 1000, 1025]) n
UNION ALL SELECT 'absent', string_agg(((i * 25 + 7) % 1000003)::text, ',') FROM generate_series(1, 40) i
UNION ALL SELECT 'k10', string_agg(((i * 29) % 300)::text, ',') FROM generate_series(1, 10) i;
CREATE FUNCTION gi_l(n text) RETURNS text LANGUAGE sql STABLE AS $$ SELECT vals FROM gi_l WHERE name = n $$;

CREATE FUNCTION gi_rows(q text, via_sift boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', via_sift::text, true), set_config('sift.enable_col_scan', via_sift::text, true), set_config('sift.agg_force', via_sift::text, true),
			set_config('enable_seqscan', (NOT via_sift)::text, true), set_config('enable_indexscan', 'false', true), set_config('enable_bitmapscan', 'false', true),
			set_config('enable_indexonlyscan', 'false', true), set_config('enable_sort', (NOT via_sift)::text, true);
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY ord)) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO r;
	RETURN r;
END $$;
-- the rows through the array and through the heap, the plan, and the entries the walk decoded (max_decoded: none given, not asked)
CREATE FUNCTION gi_check(q text, max_decoded int8 DEFAULT NULL) RETURNS TABLE (global_order boolean, same_rows boolean, nrows int8, decodes_few boolean) LANGUAGE plpgsql AS $$
DECLARE a text; b text; decoded int8; pl text; walk boolean := false; n int8;
BEGIN
	PERFORM sift._test_gorder_decoded(true);
	a := gi_rows(q, true);
	decoded := sift._test_gorder_decoded(true);
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP walk := walk OR pl LIKE '%Global Order: true%'; END LOOP;
	b := gi_rows(q, false);
	EXECUTE 'SELECT count(*) FROM (' || q || ') z' INTO n;
	RETURN QUERY SELECT walk, a IS NOT DISTINCT FROM b, n, max_decoded IS NULL OR decoded <= max_decoded;
END $$;

CREATE TABLE gi_q (n serial, shape text, q text);
INSERT INTO gi_q (shape, q) VALUES
	('2 values', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u LIMIT 100', gi_l('u2'))),
	('5 values desc', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u DESC LIMIT 100', gi_l('u5'))),
	('17 values', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u LIMIT 100', gi_l('u17'))),
	('40 values', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u LIMIT 100', gi_l('u40'))),
	('40 values limit', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u LIMIT 7', gi_l('u40'))),
	('40 values desc offset', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u DESC LIMIT 10 OFFSET 3', gi_l('u40'))),
	('40 values and absent', format('SELECT u FROM gi WHERE u IN (%s, %s) ORDER BY u LIMIT 100', gi_l('u40'), gi_l('absent'))),
	('absent', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u LIMIT 100', gi_l('absent'))),
	('repeated', format('SELECT u FROM gi WHERE u IN (%s, %s, 7, 7) ORDER BY u LIMIT 100', gi_l('u5'), gi_l('u5'))),
	('a bound beside', format('SELECT u FROM gi WHERE u IN (%s) AND u > 400000 ORDER BY u LIMIT 100', gi_l('u40'))),
	('both bounds beside desc', format('SELECT u FROM gi WHERE u IN (%s) AND u BETWEEN 200000 AND 700000 ORDER BY u DESC LIMIT 100', gi_l('u40'))),
	('another column beside', format('SELECT u, k FROM gi WHERE u IN (%s) AND k < 150 ORDER BY u LIMIT 100', gi_l('u40'))),
	('two lists', format('SELECT u FROM gi WHERE u IN (%s) AND u IN (%s) ORDER BY u LIMIT 100', gi_l('u40'), gi_l('u5'))),
	('1000 values', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u LIMIT 100', gi_l('u1000'))),
	('1025 values', format('SELECT u FROM gi WHERE u IN (%s) ORDER BY u LIMIT 100', gi_l('u1025'))),
	('ties', format('SELECT k FROM gi WHERE k IN (%s) ORDER BY k LIMIT 50', gi_l('k10'))),
	('ties desc offset', format('SELECT k FROM gi WHERE k IN (%s) ORDER BY k DESC LIMIT 50 OFFSET 400', gi_l('k10')));

-- every shape through the array and through the heap
CREATE FUNCTION gi_run() RETURNS TABLE (shape text, global_order boolean, same_rows boolean, nrows int8) LANGUAGE sql AS $$
	SELECT q.shape, c.global_order, c.same_rows, c.nrows FROM gi_q q, LATERAL gi_check(q.q) c ORDER BY q.n
$$;

SELECT * FROM gi_run();
-- a walk decodes the chunks of the values it takes (160, 1,700 and 3,800 entries for 5, 40 and 1,000 values), not the entries of the table (100,000)
SELECT * FROM gi_check((SELECT q FROM gi_q WHERE shape = '40 values'), 3000);
SELECT * FROM gi_check((SELECT q FROM gi_q WHERE shape = '5 values desc'), 400);
SELECT * FROM gi_check((SELECT q FROM gi_q WHERE shape = '1000 values'), 8000);

-- rows in the write buffer, updated rows (new keys, some of them in the lists), deleted rows
INSERT INTO gi (id, u, k) SELECT 100000 + i, (i * 31337::bigint) % 1000003, i % 300 FROM generate_series(1, 2000) i;
INSERT INTO gi SELECT 200000, u, 5 FROM (SELECT (regexp_split_to_array(gi_l('u40'), ','))[3]::bigint AS u) v;
UPDATE gi SET u = (u + 1) % 1000003 WHERE id % 7 = 0;
UPDATE gi SET u = (regexp_split_to_array(gi_l('u17'), ','))[1 + id % 17]::bigint WHERE id % 50 = 1;
DELETE FROM gi WHERE id % 19 = 0;
SELECT * FROM gi_run();
-- after a promotion and a VACUUM
SELECT sift.col_promote('gi_i'::regclass) >= 0 AS promoted;
SELECT * FROM gi_run();
VACUUM gi;
SELECT * FROM gi_run();

DROP TABLE gi, gi_l, gi_q;
DROP FUNCTION gi_rows(text, boolean), gi_check(text, int8), gi_run(), gi_l(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
