-- ORDER BY k LIMIT n with a condition on another column (a soft-delete flag, a status, a boolean, a number) is read from the global order of k: the heap row of each entry is
-- tested, as a b-tree's scan does, and the scan gives the rows to the units when the pass rate says that the n will take more fetches than the units cost (a quarter of the heap's
-- blocks, never less than four a row wanted). The answers are those of a copy of the table without an index (the oracle) over IS NULL and IS NOT NULL, equality on a column of
-- few values, a boolean, a range of another column, ties of the key, DESC, OFFSET, rows that were updated, deleted or rolled back, the write buffer and a promotion.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE tt AS SELECT i AS id, ((i::bigint * 7919) % 60001)::int AS k, ((i::bigint * 104729 + 13) % 10007)::int AS u, (i % 10) AS st, (i % 3 = 0) AS act,
	CASE WHEN i % 50 = 0 THEN now() END AS gone, CASE WHEN i % 100 = 0 THEN NULL ELSE now() END AS live, (i % 977) AS tie
FROM generate_series(1, 60000) i;
CREATE INDEX tt_i ON tt USING sift_col (id, k, u, st, act, gone, live, tie) WITH (order_columns = 'k, tie', order_array_columns = 'k, tie', write_buffer_limit = 256);
VACUUM ANALYZE tt;
CREATE TABLE tp AS SELECT * FROM tt;
ANALYZE tp;

CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_reads_global_order(q) $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.ids_agree_tt_tp(q) $$;

CREATE TABLE tq (n int, q text);
INSERT INTO tq SELECT row_number() OVER (), q FROM unnest(ARRAY[
 'SELECT id FROM @ WHERE gone IS NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE gone IS NULL ORDER BY k DESC, id LIMIT 20', 'SELECT k AS id FROM @ WHERE gone IS NULL ORDER BY k LIMIT 20 OFFSET 3000',
 'SELECT id FROM @ WHERE live IS NOT NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE live IS NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE gone IS NULL AND live IS NOT NULL ORDER BY k, id LIMIT 50',
 'SELECT id FROM @ WHERE st = 3 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE st IN (1, 4) ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE act ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE NOT act ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE NOT act AND gone IS NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE u < 9900 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE u < 5000 ORDER BY k, id LIMIT 20 OFFSET 500',
 'SELECT id FROM @ WHERE u < 1000 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE u < 9900 AND k > 20000 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE gone IS NULL ORDER BY tie, id LIMIT 30',
 'SELECT id FROM @ WHERE gone IS NULL ORDER BY tie DESC, k, id LIMIT 30', 'SELECT k AS id FROM @ WHERE u < 9900 ORDER BY k LIMIT 20', 'SELECT id FROM @ WHERE gone IS NULL ORDER BY k, id LIMIT 100000',
 -- selective: the n take far more fetches than the units cost
 'SELECT id FROM @ WHERE u < 5 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE st = 3 AND u < 3 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE u < 100 ORDER BY k, id LIMIT 20 OFFSET 100']) q;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;

-- the walk is forced (every fetch allowed), and cut short at once (the units answer after the first fetches): the same rows
SET sift.test_topn_gbudget = 1000000000;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq WHERE n IN (12, 14, 19, 20, 21) ORDER BY n;
SET sift.test_topn_gbudget = 1;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;
RESET sift.test_topn_gbudget;

-- the walk queues the rows of the LIMIT and the OFFSET before the first goes out: a page longer than work_mem holds, or an OFFSET thrown away by walking, reads the units
SET work_mem = '64kB';
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq WHERE n IN (3, 13, 19) ORDER BY n;
RESET work_mem;

-- rows that changed (few enough for the write buffer to be read with the array): a key updated (the entry of the old key is stale), a flag set, deleted, a rolled-back update, a promotion
UPDATE tt SET k = k + 7 WHERE id % 311 = 1;
UPDATE tp SET k = k + 7 WHERE id % 311 = 1;
UPDATE tt SET gone = now() WHERE id % 197 = 5;
UPDATE tp SET gone = now() WHERE id % 197 = 5;
DELETE FROM tt WHERE id % 701 = 9;
DELETE FROM tp WHERE id % 701 = 9;
INSERT INTO tt SELECT 60000 + i, i % 90, i % 10007, i % 10, i % 2 = 0, CASE WHEN i % 3 = 0 THEN now() END, now(), i % 977 FROM generate_series(1, 300) i;
INSERT INTO tp SELECT 60000 + i, i % 90, i % 10007, i % 10, i % 2 = 0, CASE WHEN i % 3 = 0 THEN now() END, now(), i % 977 FROM generate_series(1, 300) i;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;

-- a rolled-back update that moved rows to the front of the order: the first entries are all dead, and the walk that finds none of the n gives the rows to the units
BEGIN; UPDATE tt SET gone = now(), k = 0 WHERE id % 211 = 3; ROLLBACK;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;
SELECT sift.col_promote('tt_i') >= 0 AS promoted;
SELECT n, same(q) AS answers FROM tq ORDER BY n;

DROP TABLE tq, tt, tp;
DROP FUNCTION global_order(text), same(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift CASCADE;
