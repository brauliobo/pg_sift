-- count(*) WHERE g = x of an integer that is the group of the second order of the arrays (order_array_group 'auto: g', no array of its own) is the entries of the group in the block
-- of a segment whose units are all exact, not the units. A group with rows of a NULL date, a key that is not there, a write buffer, a promotion, deleted, updated and rolled back rows
-- and VACUUM keep the heap's answers.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE gc (id int, ent_id int, created timestamp, score int, pad text);
ALTER TABLE gc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gc SELECT i, abs(hashint4(i)) % 400, CASE WHEN abs(hashint4(i)) % 400 = 13 AND i % 5 = 0 THEN NULL ELSE timestamp '2024-01-01' + abs(hashint4(i + 7)) % 5000000 * interval '1 second' END,
	   abs(hashint4(i + 5)) % 5000, md5(i::text) || md5(i::text || 'x') || md5(i::text || 'y')
FROM generate_series(1, 120000) i;
INSERT INTO gc SELECT 200000 + i, NULL, timestamp '2024-02-01', i, 'n' FROM generate_series(1, 50) i;
CREATE INDEX gc_i ON gc USING sift_col (id, ent_id, created, score, pad) WITH (key_cut = 'off', key_order = 'off');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) gc;
SELECT reloptions FROM pg_class WHERE relname = 'gc_i';

-- the count is the heap's; 'counted' says the scan read no unit but the rows of a write buffer (fewer than 5,000)
CREATE FUNCTION gc_chk(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.count_check_rows_scanned_under_5000(q) $$;
CREATE FUNCTION gc_all() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT gc_chk(format('SELECT count(*) FROM gc WHERE %s', w)), 1::bigint
   FROM unnest(ARRAY['ent_id = 7', 'ent_id = 0', 'ent_id = 399', 'ent_id = 13', 'ent_id = 400', 'ent_id = -1', 'ent_id IS NULL', 'ent_id >= 390', 'ent_id = 7 AND score < 100',
					 'ent_id = 7 AND created IS NOT NULL', 'ent_id IN (7, 8)', 'ent_id = 7 OR ent_id = 8']) w $$;
SELECT result, count(*) FROM gc_all() GROUP BY 1 ORDER BY 1;

-- a write buffer next to the segment, then a promotion
INSERT INTO gc SELECT 300000 + i, i % 400, timestamp '2024-06-01' + i * interval '1 second', i % 5000, 'w' FROM generate_series(1, 1500) i;
SELECT result, count(*) FROM gc_all() GROUP BY 1 ORDER BY 1;
SELECT sift.col_promote('gc_i') >= 0 AS promoted;
VACUUM gc;
SELECT result, count(*) FROM gc_all() GROUP BY 1 ORDER BY 1;

-- rows deleted and updated (the units that hold them are read), an update and an insert rolled back
DELETE FROM gc WHERE id % 53 = 0;
UPDATE gc SET ent_id = (ent_id + 1) % 400 WHERE id % 97 = 0;
BEGIN;
UPDATE gc SET ent_id = 7 WHERE id % 3 = 0;
INSERT INTO gc SELECT 400000 + i, 7, timestamp '2025-01-01', i, 'r' FROM generate_series(1, 300) i;
ROLLBACK;
SELECT result, count(*) FROM gc_all() GROUP BY 1 ORDER BY 1;
VACUUM gc;
SELECT result, count(*) FROM gc_all() GROUP BY 1 ORDER BY 1;
SELECT sift.col_verify('gc_i');

DROP TABLE gc;
DROP FUNCTION gc_chk(text), gc_all();
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
