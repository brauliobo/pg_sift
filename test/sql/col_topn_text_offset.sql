-- ORDER BY text LIMIT n OFFSET m over the global order of a text key: the OFFSET is cut at a string, the last one of the sources that at most m entries come before, instead of walked
-- (the sources are two segments and a write buffer, in a C and a locale collation, ascending and descending, with NULLs, repeated strings, deleted, updated and rolled back rows). The answers are the heap's.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION to_str(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT (ARRAY['customer-', 'Customer-', 'order-', 'Zeta-', 'été-', 'a', 'zz'])[1 + (i * 7) % 7] || lpad(((i::bigint * 7919) % 2500)::text, 4, '0') $$;
CREATE TABLE to_t (id int, c text COLLATE "C", l text COLLATE "en-x-icu", pad text DEFAULT repeat('x', 100));
ALTER TABLE to_t ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO to_t (id, c, l) SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE to_str(i) END, CASE WHEN i % 43 = 0 THEN NULL ELSE to_str(i * 3) END FROM generate_series(1, 6000) i;
CREATE INDEX to_i ON to_t USING sift_col (id sift.col_int4_ops, c sift.col_text_ops, l sift.col_text_ops) WITH (order_array_columns = 'c, l', write_buffer_limit = 65536);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) to_t;
INSERT INTO to_t (id, c, l) SELECT 6000 + i, CASE WHEN i % 41 = 0 THEN NULL ELSE to_str(i * 5) END, CASE WHEN i % 43 = 0 THEN NULL ELSE to_str(i * 11) END FROM generate_series(1, 5000) i;
SELECT sift.col_promote('to_i') >= 0 AS promoted;
INSERT INTO to_t (id, c, l) SELECT 11000 + i, to_str(i * 13), to_str(i * 17) FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) to_t;
SELECT count(*) AS segments, count(*) FILTER (WHERE kind = 'buffer') AS buffers FROM sift.col_segments('to_i');

-- the rows are the plan's without the extension, in order; 'global order' says the scan walked the global order
CREATE FUNCTION to_chk(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.topn_check_ordered_rows_with_nulls(q) $$;
CREATE FUNCTION to_all() RETURNS TABLE (col text, result text, n bigint) LANGUAGE sql AS
$$ SELECT c, to_chk(format('SELECT %1$s FROM to_t %5$s ORDER BY %1$s %2$s %3$s LIMIT 10 OFFSET %4$s', c, d, nl, o, replace(w, '@', c))), 1::bigint
   FROM unnest(ARRAY['c', 'l']) c, unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['NULLS FIRST', 'NULLS LAST']) nl, unnest(ARRAY[2047, 2048, 4000, 7500, 12000, 13990]) o,
		unnest(ARRAY['', 'WHERE @ >= ''customer-1''', 'WHERE @ < ''order-1'' AND @ >= ''Zeta-0''']) w $$;
SELECT col, result, count(*) FROM to_all() GROUP BY 1, 2 ORDER BY 1, 2;

-- deleted and updated rows (their pages are not all-visible: the cut stops where an entry needs the heap), an update and an insert rolled back
DELETE FROM to_t WHERE id % 53 = 0;
UPDATE to_t SET c = 'u' || c, l = 'u' || l WHERE id % 97 = 0;
BEGIN;
UPDATE to_t SET c = 'a', l = 'a' WHERE id % 3 = 0;
INSERT INTO to_t (id, c, l) SELECT 50000 + i, 'customer-', 'customer-' FROM generate_series(1, 300) i;
ROLLBACK;
SELECT col, result, count(*) FROM to_all() GROUP BY 1, 2 ORDER BY 1, 2;
VACUUM to_t;
SELECT col, result, count(*) FROM to_all() GROUP BY 1, 2 ORDER BY 1, 2;
SELECT sift.col_verify('to_i');

DROP FUNCTION to_all(), to_chk(text), to_str(int);
DROP TABLE to_t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
