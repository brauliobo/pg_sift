-- ORDER BY a text column, id LIMIT n (the page of a list with a stable order) is read from the global order of the text column like ORDER BY the column alone: the strings of the array
-- are told apart by their bytes, a group of equal strings is fetched whole and put in order by the keys after the first, and the rows are the heap's. The answers are those of a copy
-- of the table without an index (the oracle) over NULLs, ties, DESC, NULLS FIRST and LAST, bounds on the string, OFFSET, deleted and updated rows, the write buffer, a promotion;
-- a collation that calls different strings equal keeps the scan that reads the units.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SELECT setseed(0.42);

CREATE TABLE tt AS SELECT i AS id, CASE WHEN i % 23 = 0 THEN NULL ELSE 't' || lpad(((i::bigint * 7919) % 3001)::text, 5, '0') END AS s,
	CASE WHEN i % 29 = 0 THEN NULL ELSE ((i * 13) % 17) END AS g, (random() * 100)::int AS pad
FROM generate_series(1, 30000) i;
CREATE INDEX tt_i ON tt USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, g sift.col_int4_ops, pad sift.col_int4_ops) WITH (order_array_columns = 's', write_buffer_limit = 256);
VACUUM ANALYZE tt;
CREATE TABLE tp AS SELECT * FROM tt;
ANALYZE tp;

CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_reads_global_order(q) $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.ids_agree_tt_tp(q) $$;

CREATE TABLE tq (n int, q text);
INSERT INTO tq SELECT row_number() OVER (), q FROM unnest(ARRAY[
 'SELECT id FROM @ ORDER BY s, id LIMIT 20', 'SELECT id FROM @ ORDER BY s, id LIMIT 500', 'SELECT id FROM @ ORDER BY s DESC, id LIMIT 20', 'SELECT id FROM @ ORDER BY s, id DESC LIMIT 20',
 'SELECT id FROM @ ORDER BY s NULLS FIRST, id LIMIT 20', 'SELECT id FROM @ ORDER BY s DESC NULLS LAST, id LIMIT 20', 'SELECT id FROM @ ORDER BY s, g, id LIMIT 30', 'SELECT id FROM @ ORDER BY s, g DESC NULLS FIRST, id LIMIT 30',
 'SELECT id FROM @ WHERE s >= ''t01500'' ORDER BY s, id LIMIT 20', 'SELECT id FROM @ WHERE s > ''t01500'' ORDER BY s, id LIMIT 20 OFFSET 7', 'SELECT id FROM @ WHERE s < ''t00100'' ORDER BY s, id LIMIT 50',
 'SELECT id FROM @ WHERE s BETWEEN ''t00500'' AND ''t00520'' ORDER BY s, id LIMIT 100', 'SELECT id FROM @ WHERE s = ''t00007'' ORDER BY s, id LIMIT 20', 'SELECT id FROM @ ORDER BY s, id LIMIT 20 OFFSET 1000',
 'SELECT id FROM @ ORDER BY s, id LIMIT 20 OFFSET 29000', 'SELECT id FROM @ WHERE s >= ''zzz'' ORDER BY s, id LIMIT 20', 'SELECT id FROM @ WHERE s IS NOT NULL ORDER BY s, id LIMIT 20',
 'SELECT id FROM @ WHERE g = 3 ORDER BY s, id LIMIT 20', 'SELECT id FROM @ WHERE s LIKE ''t01%'' ORDER BY s, id LIMIT 20', 'SELECT id FROM @ ORDER BY s COLLATE "C", id LIMIT 20']) q;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;

-- rows that changed: updated, deleted, a rolled-back update, the write buffer, a promotion
UPDATE tt SET s = 't01500' WHERE id % 500 = 3;
UPDATE tp SET s = 't01500' WHERE id % 500 = 3;
DELETE FROM tt WHERE id % 700 = 9;
DELETE FROM tp WHERE id % 700 = 9;
BEGIN; UPDATE tt SET s = 't00001' WHERE id % 300 = 7; ROLLBACK;
INSERT INTO tt SELECT 30000 + i, CASE WHEN i % 10 = 0 THEN NULL ELSE 't01500' END, 5, 0 FROM generate_series(1, 400) i;
INSERT INTO tp SELECT 30000 + i, CASE WHEN i % 10 = 0 THEN NULL ELSE 't01500' END, 5, 0 FROM generate_series(1, 400) i;
SELECT n, same(q) AS answers FROM tq ORDER BY n;
SELECT sift.col_promote('tt_i') >= 0 AS promoted;
SELECT n, same(q) AS answers FROM tq ORDER BY n;

-- a collation that calls different strings equal: the strings of a group would be told apart by their bytes, so the units are read
CREATE COLLATION ci_ai (provider = icu, locale = 'und-u-ks-level1', deterministic = false);
SELECT global_order('SELECT id FROM tt ORDER BY s COLLATE ci_ai, id LIMIT 20') AS nondeterministic_collation_global_order;
SELECT (SELECT string_agg(id::text, ',') FROM (SELECT id FROM tt ORDER BY s COLLATE ci_ai, id LIMIT 20) x) =
       (SELECT string_agg(id::text, ',') FROM (SELECT id FROM tp ORDER BY s COLLATE ci_ai, id LIMIT 20) x) AS nondeterministic_collation_answers;

DROP COLLATION ci_ai;
DROP TABLE tq, tt, tp;
DROP FUNCTION global_order(text), same(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
