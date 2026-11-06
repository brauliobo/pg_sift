-- ORDER BY key LIMIT n OFFSET m over the global order of the key, in units that hold dead rows or whose pages are not all-visible: the entries the OFFSET throws away are
-- confirmed without the heap when their page is all-visible and the unit's liveness says the row is live (the dead rows of a unit are listed once); the others are fetched.
-- The rows are the plan's without the extension after deleted, updated (HOT and not), rolled back and vacuumed rows, and for a cursor opened before the update.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

CREATE TABLE od (id int PRIMARY KEY, n int, s text COLLATE "C", pad text DEFAULT repeat('x', 200));
ALTER TABLE od ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO od (id, n, s) SELECT i, (i * 7919) % 1000, 'k' || lpad(((i::bigint * 7919) % 100003)::text, 6, '0') FROM generate_series(1, 12000) i;
-- pages with gaps (the rows of a page are then a bitmap of its offsets in the unit)
DELETE FROM od WHERE id % 5 = 2;
VACUUM od;
CREATE INDEX od_i ON od USING sift_col (id sift.col_int4_ops, n sift.col_int4_ops, s sift.col_text_ops) WITH (order_columns = 'id, n', order_array_columns = 'id, n, s', track_changes = 'off');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) od;
SET sift.test_topn_heads = off;

-- the rows are the plan's without the extension, in order; the answer says 'skipped' when the scan fetched no more than the LIMIT and an eighth of the OFFSET more, 'fetched' when it fetched more
CREATE FUNCTION od_chk(q text, lim int, off int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.topn_offset_check_heap_fetches(q, lim, off) $$;

CREATE FUNCTION od_all(lim int DEFAULT 10, offs int[] DEFAULT ARRAY[1500, 6000]) RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT od_chk(format('SELECT id, n, s FROM od ORDER BY %s %s LIMIT %s OFFSET %s', k, d, lim, o), lim, o) AS result, count(*)
   FROM unnest(ARRAY['s', 'n, id']) k, unnest(ARRAY['ASC', 'DESC']) d, unnest(offs) o GROUP BY 1 ORDER BY 1 $$;

SELECT * FROM od_all();
-- deleted and updated rows: their pages are not all-visible, the others are
DELETE FROM od WHERE id % 53 = 0;
UPDATE od SET n = n + 1, s = 'u' || s WHERE id % 97 = 0;
SELECT * FROM od_all();
-- a VACUUM flags the dead rows of every unit and frees their addresses
VACUUM od;
SELECT * FROM od_all();
-- a HOT update (no indexed column changes), an update rolled back and a cursor opened before an update
UPDATE od SET pad = repeat('y', 200) WHERE id % 31 = 0;
BEGIN;
UPDATE od SET n = n + 5, s = 'r' || s WHERE id % 101 = 0;
ROLLBACK;
SELECT * FROM od_all();
VACUUM od;
SELECT * FROM od_all();
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET LOCAL sift.enable_agg = off;
CREATE TEMP TABLE od_before AS SELECT id, n, s FROM od ORDER BY s LIMIT 5 OFFSET 6000;
SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
DECLARE od_c CURSOR FOR SELECT id, n, s FROM od ORDER BY s LIMIT 5 OFFSET 6000;
UPDATE od SET s = 'a' || s WHERE id % 3 = 0;
FETCH ALL FROM od_c;
SELECT * FROM od_before;
COMMIT;
SELECT * FROM od_all();
VACUUM od;
SELECT * FROM od_all();

DROP FUNCTION od_all(int, int[]), od_chk(text, int, int);
DROP TABLE od;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
