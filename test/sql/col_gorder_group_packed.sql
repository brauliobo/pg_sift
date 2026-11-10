-- The table of groups of a grouped order array (order_array_group 'auto: ent_id') of 1,024 groups or more, with no NULL key, is packed to the bits of the distance of a group from the first and of the
-- start of its entries (word 7 of the block, column format minor 44): a block with NULL keys keeps the 24 bytes of each group. The answers are the heap's in both, for a group the table has, between
-- two of them, before the first and after the last, and after writes, a merge and a REINDEX.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

-- 3,000 groups spread over 2 billion values, some negative, 20 rows each; gn has one row with a NULL key
CREATE TABLE gk (id int, ent_id int, ts timestamp);
INSERT INTO gk SELECT i, ((i % 3000) - 1500) * 700001, timestamp '2024-01-01' + (abs(hashint4(i)) % 200000) * interval '1 minute' FROM generate_series(1, 60000) i;
CREATE TABLE gn (LIKE gk);
INSERT INTO gn SELECT * FROM gk;
INSERT INTO gn VALUES (60001, 700001, NULL);
CREATE TABLE gz (LIKE gk);
INSERT INTO gz SELECT * FROM gk;
ANALYZE gk;
ANALYZE gn;
ANALYZE gz;
CREATE INDEX gk_ix ON gk USING sift_col (id, ent_id, ts) WITH (key_cut = 'off', order_array_columns = 'ts', order_array_group = 'auto: ent_id');
CREATE INDEX gn_ix ON gn USING sift_col (id, ent_id, ts) WITH (key_cut = 'off', order_array_columns = 'ts', order_array_group = 'auto: ent_id');
SELECT sift.col_verify('gk_ix'), sift.col_verify('gn_ix');
SELECT (SELECT sum(bytes) FROM sift.col_sections('gn_ix') WHERE section = 'order_arrays') - (SELECT sum(bytes) FROM sift.col_sections('gk_ix') WHERE section = 'order_arrays') > 40000 AS table_packed;

CREATE FUNCTION gz_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gk') || ') t' INTO ra;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gz') || ') t' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q ELSE 'same' END;
END $$;
CREATE FUNCTION gz_all() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT gz_chk(format(q, g)), 1::bigint
   FROM unnest(ARRAY['SELECT count(*), min(ts), max(ts) FROM @ WHERE ent_id = %s',
					 'SELECT id, ts FROM @ WHERE ent_id = %s ORDER BY ts, id LIMIT 5',
					 'SELECT id, ts FROM @ WHERE ent_id = %s ORDER BY ts DESC, id DESC LIMIT 5',
					 'SELECT id, ts FROM @ WHERE ent_id = %s ORDER BY ts, id LIMIT 5 OFFSET 7',
					 'SELECT id, ts FROM @ WHERE ent_id IN (%s, 700001, -1050001500) ORDER BY ts, id LIMIT 12']) q,
		unnest(ARRAY[-1049998499, -1050001500, -1050001499, 0, 700001, 1049998499, 1049999999, 1050000000, 7]) g $$;
SELECT result, count(*) FROM gz_all() GROUP BY 1 ORDER BY 1;

-- writes after the build, a merge and a REINDEX
INSERT INTO gk SELECT 100000 + i, ((i % 3100) - 1550) * 700001, timestamp '2024-01-01' + (i * 7919 % 200000) * interval '1 minute' FROM generate_series(1, 4000) i;
INSERT INTO gz SELECT 100000 + i, ((i % 3100) - 1550) * 700001, timestamp '2024-01-01' + (i * 7919 % 200000) * interval '1 minute' FROM generate_series(1, 4000) i;
SELECT result, count(*) FROM gz_all() GROUP BY 1 ORDER BY 1;
SELECT sift.col_promote('gk_ix') >= 0 AS promoted;
SELECT sift.col_merge('gk_ix', true) >= 0 AS merged;
SELECT result, count(*) FROM gz_all() GROUP BY 1 ORDER BY 1;
REINDEX INDEX gk_ix;
SELECT sift.col_verify('gk_ix');
SELECT result, count(*) FROM gz_all() GROUP BY 1 ORDER BY 1;

DROP TABLE gk, gn, gz;
DROP FUNCTION gz_all();
DROP FUNCTION gz_chk(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
