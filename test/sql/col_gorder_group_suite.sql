-- The global order of a sift_col index. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
CREATE EXTENSION pg_sift_helpers;

-- >>> col_gorder_group_packed
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- The table of groups of a grouped order array (order_array_group 'auto: ent_id') of 1,024 groups or more, with no NULL key, is packed to the bits of the distance of a group from the first and of the
-- start of its entries (word 7 of the block, column format minor 44): a block with NULL keys keeps the 24 bytes of each group. The answers are the heap's in both, for a group the table has, between
-- two of them, before the first and after the last, and after writes, a merge and a REINDEX.
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
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< col_gorder_group_packed

-- >>> col_gorder_group_price
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- The build prices the second order of an array by the table of groups each segment writes: 24 bytes for every group the segment holds, or, from 1,024 groups on and with no NULL key in the
-- segment, the bits of a distance from the first group and of the start of its entries (col_gorder_group_packed). An entity of 60,000 values with 5 rows apiece costs the packed table about
-- 2 bytes a row (the second order is chosen, the b-tree of the table names it); the same table with a NULL timestamp in one row of fifty costs the 24-byte table of each segment, about 10
-- bytes a row, and the weighted formula refuses it.
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET maintenance_work_mem = '64MB';
\set VERBOSITY terse

CREATE TABLE gp_whole (id int, ent_id int, ts timestamp);
INSERT INTO gp_whole SELECT i, abs(hashint4(i)) % 60000, timestamp '2024-01-01' + (abs(hashint4(i + 7)) % 20000000) * interval '1 second' FROM generate_series(1, 300000) i;
CREATE TABLE gp_nulls (LIKE gp_whole);
INSERT INTO gp_nulls SELECT id, ent_id, CASE WHEN id % 50 = 0 THEN NULL ELSE ts END FROM gp_whole;
CREATE INDEX gp_whole_bt ON gp_whole (ent_id, ts);
CREATE INDEX gp_nulls_bt ON gp_nulls (ent_id, ts);
VACUUM (FREEZE, ANALYZE) gp_whole;
VACUUM (FREEZE, ANALYZE) gp_nulls;
CREATE INDEX gp_whole_ix ON gp_whole USING sift_col (id, ent_id, ts) WITH (key_cut = 'off');
CREATE INDEX gp_nulls_ix ON gp_nulls USING sift_col (id, ent_id, ts) WITH (key_cut = 'off');
SELECT relname, (SELECT option_value FROM pg_options_to_table(reloptions) WHERE option_name = 'order_array_group') AS order_array_group
FROM pg_class WHERE relname IN ('gp_whole_ix', 'gp_nulls_ix') ORDER BY 1;

DROP TABLE gp_whole, gp_nulls;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< col_gorder_group_price

-- >>> col_gorder_group_probe_dead
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- A group array read of a table whose rows were updated and vacuumed takes the exact key of an entry from the heap, and asks of the dead rows of the segment only those of the unit of the
-- entry's page: the top rows of one group are a few pages of a table that has sixty thousand dead rows, and listing them all for each statement was most of its cost.
SET client_min_messages = warning;
SET synchronous_commit = on;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gd (id int PRIMARY KEY, g int, ts timestamp, pad text DEFAULT repeat('x', 60)) WITH (autovacuum_enabled = off);
ALTER TABLE gd ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gd (id, g, ts) SELECT i, i % 50, timestamp '2026-01-01' + i * interval '1 minute' FROM generate_series(1, 200000) i;
CREATE INDEX gd_i ON gd USING sift_col (id, g, ts) WITH (order_array_columns = 'ts', order_array_group = 'g', key_cut = 'off');
VACUUM (FREEZE, ANALYZE) gd;
UPDATE gd SET ts = ts + interval '3 hours', g = (g + 1) % 50 WHERE id % 10 < 3;
VACUUM gd;
SELECT (SELECT count(*) FROM sift.col_segments('gd_i') WHERE kind = 'immutable') AS segments, (SELECT sum(dead_rows) FROM sift.col_segments('gd_i')) > 50000 AS many_dead;

CREATE FUNCTION gd_rows(q text, via_sift boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', via_sift::text, true), set_config('sift.enable_col_scan', via_sift::text, true), set_config('sift.agg_force', via_sift::text, true),
			set_config('enable_seqscan', (NOT via_sift)::text, true), set_config('enable_indexscan', 'false', true), set_config('enable_bitmapscan', 'false', true),
			set_config('enable_indexonlyscan', 'false', true), set_config('enable_sort', (NOT via_sift)::text, true);
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY ord)) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO r;
	RETURN r;
END $$;
-- the rows through the array and through the heap, the plan, and the dead addresses the walk listed
CREATE FUNCTION gd_check(q text, max_dead int) RETURNS TABLE (global_order boolean, same_rows boolean, dead_it_lists boolean) LANGUAGE plpgsql AS $$
DECLARE a text; b text; listed int8; pl text; walk boolean := false;
BEGIN
	PERFORM sift._test_probe_dead(true);
	a := gd_rows(q, true);
	listed := sift._test_probe_dead();
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP walk := walk OR pl LIKE '%Global Order: true%'; END LOOP;
	b := gd_rows(q, false);
	RETURN QUERY SELECT walk, a = b, listed <= max_dead;
END $$;

SELECT 'top desc' AS shape, * FROM gd_check('SELECT ts FROM gd WHERE g = 7 ORDER BY ts DESC LIMIT 20', 10000);
SELECT 'top asc' AS shape, * FROM gd_check('SELECT ts FROM gd WHERE g = 8 ORDER BY ts LIMIT 20', 10000);
SELECT 'offset' AS shape, * FROM gd_check('SELECT ts FROM gd WHERE g = 9 ORDER BY ts DESC LIMIT 20 OFFSET 200', 10000);

DROP TABLE gd;
DROP FUNCTION gd_rows(text, boolean), gd_check(text, int);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< col_gorder_group_probe_dead

-- >>> col_gorder_group_range
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- count(*), sums and the rows of WHERE g = x AND k BETWEEN a AND b, with k an array grouped by g (order_array_group), come from the entries of the group between the two keys: a count reads
-- no unit, the other aggregates read the rows the entries name. A group that is not there, an empty or open range, NULL keys, a write buffer, a promotion, deleted, updated and rolled back
-- rows and VACUUM keep the heap's answers; so does a group order that sits in the first slot (an explicit group) or the second (the build's own).
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE gr (id int, ent_id int, created timestamp, score int, pad text);
ALTER TABLE gr ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gr SELECT i, abs(hashint4(i)) % 400, CASE WHEN i % 41 = 0 THEN NULL ELSE timestamp '2024-01-01' + abs(hashint4(i + 7)) % 5000000 * interval '1 second' END,
	   abs(hashint4(i + 5)) % 5000, md5(i::text) || md5(i::text || 'x') || md5(i::text || 'y')
FROM generate_series(1, 30000) i;
CREATE TABLE gs (LIKE gr);
ALTER TABLE gs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gs SELECT * FROM gr;
CREATE INDEX gr_i ON gr USING sift_col (id, ent_id, created, score, pad) WITH (key_cut = 'off', order_array_columns = 'created', order_array_group = 'ent_id');
CREATE INDEX gs_i ON gs USING sift_col (id, ent_id, created, score, pad) WITH (key_cut = 'off', order_array_columns = 'created', order_array_group = 'auto: ent_id');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) gr;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) gs;
SELECT relname, reloptions FROM pg_class WHERE relname IN ('gr_i', 'gs_i') ORDER BY 1;

-- a scalar sub-select that gives one row (created = (SELECT min(created) ...)) is a constant of the scan, so its group is narrowed like a literal one
-- the answer is the heap's; 'counted' says no unit was read by the scan of the statement (a count from the entries), 'narrowed' that units were read for the rows of the entries only
CREATE FUNCTION gr_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; counted bool := false; narrowed bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		-- the scan of the statement only: the sub-select's own scan (an InitPlan, printed after it) is not what is asked: after the rows are read (an INSERT .. SELECT of them) PostgreSQL 19 has set the visibility bits of their pages while pruning, and its units answer min() from pre-aggregates, where 17 and 18 do that only after a VACUUM
		EXIT WHEN pl ~ 'InitPlan';
		counted := counted OR coalesce(substring(pl from 'Rows: ([0-9]+) scanned')::int < 5000, false);
		narrowed := narrowed OR pl ~ 'from point directory';
	END LOOP;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (' || q || ') t' INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (' || q || ') t' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN counted THEN 'counted' WHEN narrowed THEN 'narrowed' ELSE 'units' END;
END $$;
CREATE FUNCTION gr_all(t text) RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT gr_chk(format('SELECT %s FROM %s WHERE %s', sel, t, w)), 1::bigint
   FROM unnest(ARRAY['count(*)', 'count(*), sum(score), max(id)', 'min(created), max(created)', 'id, created']) sel,
		unnest(ARRAY['ent_id = 7 AND created BETWEEN ''2024-02-01'' AND ''2024-02-15''',
					 'ent_id = 7 AND created >= ''2024-03-01''',
					 'ent_id = 7 AND created < ''2024-01-20''',
					 'ent_id = 7 AND created = (SELECT min(created) FROM gr WHERE ent_id = 7)',
					 'ent_id = 7 AND created BETWEEN ''2024-02-15'' AND ''2024-02-01''',
					 'ent_id = 7 AND created > ''2030-01-01''',
					 'ent_id = 400 AND created BETWEEN ''2024-02-01'' AND ''2024-02-15''',
					 'ent_id = 0 AND created BETWEEN ''2024-01-01'' AND ''2024-01-02''',
					 'ent_id = 399 AND created BETWEEN ''2024-01-01'' AND ''2025-01-01''',
					 'ent_id >= 7 AND ent_id <= 7 AND created > ''2024-04-01'' AND created < ''2024-04-20''',
					 'ent_id BETWEEN 7 AND 9 AND created BETWEEN ''2024-02-01'' AND ''2024-02-15''',
					 'ent_id = 7 AND created IS NULL',
					 'ent_id = 7 AND created BETWEEN ''2024-02-01'' AND ''2024-02-15'' AND score < 2500']) w
   WHERE NOT (sel = 'id, created' AND w ~ '2030|2025-01-01') $$;
SELECT result, count(*) FROM gr_all('gr') GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM gr_all('gs') GROUP BY 1 ORDER BY 1;

-- a write buffer next to the segment, then a promotion
INSERT INTO gr SELECT 300000 + i, i % 400, timestamp '2024-02-05' + i * interval '1 second', i % 5000, 'w' FROM generate_series(1, 1500) i;
INSERT INTO gs SELECT * FROM gr WHERE id > 300000;
SELECT result, count(*) FROM gr_all('gr') GROUP BY 1 ORDER BY 1;
SELECT sift.col_promote('gr_i') >= 0 AS promoted_r, sift.col_promote('gs_i') >= 0 AS promoted_s;
VACUUM gr;
VACUUM gs;
SELECT result, count(*) FROM gr_all('gr') GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM gr_all('gs') GROUP BY 1 ORDER BY 1;

-- rows deleted and updated (the units that hold them are read), an update and an insert rolled back
DELETE FROM gr WHERE id % 53 = 0;
DELETE FROM gs WHERE id % 53 = 0;
UPDATE gr SET ent_id = (ent_id + 1) % 400 WHERE id % 97 = 0;
UPDATE gs SET ent_id = (ent_id + 1) % 400 WHERE id % 97 = 0;
BEGIN;
UPDATE gr SET created = timestamp '2024-02-10' WHERE id % 3 = 0;
INSERT INTO gr SELECT 400000 + i, 7, timestamp '2024-02-10', i, 'r' FROM generate_series(1, 300) i;
ROLLBACK;
SELECT result, count(*) FROM gr_all('gr') GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM gr_all('gs') GROUP BY 1 ORDER BY 1;
VACUUM gr;
VACUUM gs;
SELECT result, count(*) FROM gr_all('gr') GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM gr_all('gs') GROUP BY 1 ORDER BY 1;
SELECT sift.col_verify('gr_i'), sift.col_verify('gs_i');

DROP TABLE gr, gs;
DROP FUNCTION gr_chk(text), gr_all(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< col_gorder_group_range

-- >>> col_gorder_text_buffer
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- The write buffer of a text column is sorted for the global order like a segment's entries, whatever its size (here 6,000 rows): under a collation that
-- orders by bytes its rows are cut into ranges by their first 8 bytes and only the ranges a read reaches are sorted. The strings share their first 8 bytes ('customer-...'), are shorter than 8,
-- hold accents (bytes past 0x7f), repeat, are empty or NULL. The answers are the heap's, in both directions, NULLS FIRST and LAST, with OFFSET and bounds, after rows of the buffer are deleted,
-- updated and rolled back, and after a promotion. A column under a locale (en-x-icu) sorts its buffer whole up to 16,384 rows (more reads the units): its answers are the heap's too.
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION tx_str(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 13 WHEN 0 THEN '' WHEN 1 THEN 'ab' ELSE '' END
		|| (ARRAY['customer-', 'customer-0', 'order-', 'Zeta-', 'été-', 'a', 'zz'])[1 + (i * 7) % 7] || lpad(((i::bigint * 7919) % 3000)::text, 4, '0') $$;
CREATE TABLE tx (id int, c text COLLATE "C", l text COLLATE "en-x-icu", pad int);
INSERT INTO tx SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE tx_str(i) END, CASE WHEN i % 43 = 0 THEN NULL ELSE tx_str(i * 3) END, 0 FROM generate_series(1, 8000) i;
CREATE INDEX tx_i ON tx USING sift_col (id sift.col_int4_ops, c sift.col_text_ops, l sift.col_text_ops, pad sift.col_int4_ops) WITH (order_array_columns = 'c, l', write_buffer_limit = 65536);
VACUUM tx;
INSERT INTO tx SELECT 8000 + i, CASE WHEN i % 41 = 0 THEN NULL ELSE tx_str(i * 5) END, CASE WHEN i % 43 = 0 THEN NULL ELSE tx_str(i * 11) END, 0 FROM generate_series(1, 6000) i;
SELECT sum(rows) > 4096 AS buffer_over_the_old_limit FROM sift.col_segments('tx_i') WHERE kind = 'buffer';
ANALYZE tx;

CREATE FUNCTION tx_chk(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.topn_check_ordered_rows_with_nulls(q) $$;
CREATE FUNCTION tx_all() RETURNS TABLE (col text, result text, n bigint) LANGUAGE sql AS
$$ SELECT c, tx_chk(format('SELECT %1$s FROM tx %6$s ORDER BY %1$s %2$s %3$s LIMIT %4$s OFFSET %5$s', c, d, nl, lim, o, w)), 1::bigint
   FROM unnest(ARRAY['c', 'l']) WITH ORDINALITY c(c, ci), unnest(ARRAY['ASC', 'DESC']) WITH ORDINALITY d(d, di), unnest(ARRAY['NULLS FIRST', 'NULLS LAST']) WITH ORDINALITY nl(nl, nli),
		unnest(ARRAY[1, 10, 100, 2000]) WITH ORDINALITY lim(lim, limi), unnest(ARRAY[0, 7]) WITH ORDINALITY o(o, oi),
		unnest(ARRAY['', 'WHERE id > 0', 'WHERE c >= ''customer-0100'' AND c < ''customer-0300''', 'WHERE c >= ''order-''', 'WHERE c < ''a''']) WITH ORDINALITY w(w, wi)
   WHERE (ci + di + nli + limi + oi + wi) % 3 = 0 $$;
SELECT col, result, count(*) FROM tx_all() GROUP BY 1, 2 ORDER BY 1, 2;

-- rows of the buffer deleted, updated (the key moves), an insert and an update rolled back
DELETE FROM tx WHERE id > 8000 AND id % 7 = 0;
UPDATE tx SET c = 'customer-' || id::text, l = 'Zeta-' || id::text WHERE id > 8000 AND id % 11 = 0;
BEGIN;
UPDATE tx SET c = 'a' WHERE id > 8000 AND id % 3 = 0;
INSERT INTO tx SELECT 40000 + i, 'customer-', 'customer-', 1 FROM generate_series(1, 200) i;
ROLLBACK;
ANALYZE tx;
SELECT sum(rows) > 4096 AS buffer_over_the_old_limit FROM sift.col_segments('tx_i') WHERE kind = 'buffer';
SELECT col, result, count(*) FROM tx_all() GROUP BY 1, 2 ORDER BY 1, 2;

SELECT sift.col_promote('tx_i') >= 0 AS promoted;
INSERT INTO tx SELECT 50000 + i, tx_str(i * 13), tx_str(i * 17), 0 FROM generate_series(1, 5000) i;
ANALYZE tx;
SELECT col, result, count(*) FROM tx_all() GROUP BY 1, 2 ORDER BY 1, 2;
-- a buffer past the limit of a locale column: the units answer, the answers are the heap's
INSERT INTO tx SELECT 100000 + i, tx_str(i * 19), tx_str(i * 23), 0 FROM generate_series(1, 17000) i;
ANALYZE tx;
SELECT sum(rows) > 16384 AS buffer_over_the_locale_limit FROM sift.col_segments('tx_i') WHERE kind = 'buffer';
SELECT col, result, count(*) FROM tx_all() GROUP BY 1, 2 ORDER BY 1, 2;
SELECT sift.col_verify('tx_i');

DROP FUNCTION tx_all(), tx_chk(text), tx_str(int);
DROP TABLE tx;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< col_gorder_text_buffer

-- >>> col_gorder_text_count
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
-- count(*) WHERE text <op> const (equality, a half line, a prefix) or two bounds of a range of a column with a text order array and no count_columns: a segment whose units are all exact counts the entries between the ranks of
-- the bounds instead of reading its units. The strings repeat, share prefixes, end in 0xFF bytes, are empty or NULL, and the segment is under the C collation (the bytes are the order); a column under a
-- locale reads the units. The answers are the heap's after deleted, updated and rolled back rows, VACUUM, a write buffer and a promotion.
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION tc_str(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 17 WHEN 0 THEN '' WHEN 1 THEN 'ab' ELSE '' END
		|| (ARRAY['customer-', 'customer-0', 'order-', 'Zeta-', 'été-', 'a', 'zz', 'ab', 'abc'])[1 + (i * 7) % 9] || lpad(((i::bigint * 7919) % 700)::text, 3, '0') $$;
CREATE TABLE tc (id int, c text COLLATE "C", l text COLLATE "en-x-icu", pad text DEFAULT repeat('x', 100));
ALTER TABLE tc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO tc (id, c, l) SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE tc_str(i) END, CASE WHEN i % 43 = 0 THEN NULL ELSE tc_str(i * 3) END FROM generate_series(1, 16000) i;
INSERT INTO tc (id, c, l) VALUES (40001, convert_from('\xc3bf', 'LATIN1'), 'x'), (40002, '', ''), (40003, 'ab', 'ab'), (40004, 'ab', 'ab');
CREATE INDEX tc_i ON tc USING sift_col (id sift.col_int4_ops, c sift.col_text_ops, l sift.col_text_ops) WITH (order_array_columns = 'c, l');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) tc;

-- the count is the heap's; 'counted' says the scan read no unit but the rows of a write buffer (fewer than 5,000)
CREATE FUNCTION tc_chk(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.count_check_rows_scanned_under_5000(q) $$;
CREATE FUNCTION tc_all() RETURNS TABLE (col text, result text, n bigint) LANGUAGE sql AS
$$ SELECT c, tc_chk(format('SELECT count(*) FROM tc WHERE %s', replace(w, '@', c))), 1::bigint
   FROM unnest(ARRAY['c', 'l']) c,
		unnest(ARRAY['@ = ''ab''', '@ = ''''', '@ = ''nothing''', '@ < ''customer-0''', '@ <= ''customer-0''', '@ > ''order-''', '@ >= ''order-''', '@ < ''''', '@ >= ''''', '@ > ''zzz''', '@ > ''ab''', '@ < ''ab''', '@ <= ''ab''', '@ >= ''ab''', '@ > ''''', '@ <= ''''',
					 '@ LIKE ''a%''', '@ LIKE ''ab%''', '@ LIKE ''customer-%''', '@ LIKE ''customer-0%''', '@ LIKE ''%''', '@ LIKE ''nothing%''', '@ LIKE ''é%''', '@ LIKE ''Z%''', '@ LIKE ''zz%''',
					 '@ >= ''customer-0'' AND @ < ''order-''', '@ > ''ab'' AND @ <= ''abc''', '@ >= ''a'' AND @ <= ''a''', '@ < ''order-'' AND @ >= ''customer-0''', '@ > ''z'' AND @ < ''a''', '@ >= '''' AND @ < ''zz''']) WITH ORDINALITY w(w, wi)
   WHERE wi % 2 = 1 OR w LIKE '%LIKE%' $$;
SELECT col, result, count(*) FROM tc_all() GROUP BY 1, 2 ORDER BY 1, 2;

-- a write buffer next to the segment, then a promotion
INSERT INTO tc (id, c, l) SELECT 60000 + i, tc_str(i * 5), tc_str(i * 11) FROM generate_series(1, 1500) i;
SELECT col, result, count(*) FROM tc_all() GROUP BY 1, 2 ORDER BY 1, 2;
SELECT sift.col_promote('tc_i') >= 0 AS promoted;
VACUUM tc;
SELECT col, result, count(*) FROM tc_all() GROUP BY 1, 2 ORDER BY 1, 2;

-- rows deleted and updated (the units that hold them are read), an update and an insert rolled back
DELETE FROM tc WHERE id % 53 = 0;
UPDATE tc SET c = 'u' || c, l = 'u' || l WHERE id % 97 = 0;
BEGIN;
UPDATE tc SET c = 'a', l = 'a' WHERE id % 3 = 0;
INSERT INTO tc (id, c, l) SELECT 50000 + i, 'customer-', 'customer-' FROM generate_series(1, 300) i;
ROLLBACK;
SELECT col, result, count(*) FROM tc_all() GROUP BY 1, 2 ORDER BY 1, 2;
VACUUM tc;
SELECT col, result, count(*) FROM tc_all() GROUP BY 1, 2 ORDER BY 1, 2;

SELECT sift.col_verify('tc_i');

DROP FUNCTION tc_all(), tc_chk(text), tc_str(int);
DROP TABLE tc;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< col_gorder_text_count

-- >>> col_gorder_dead_cache
CREATE SCHEMA s7;
GRANT USAGE ON SCHEMA s7 TO PUBLIC;
SET search_path = s7, public;
-- The lists of dead rows that an OFFSET over the global order asks for (the rows a segment lost to VACUUM, by unit) are kept by the backend for the next scan: the second query finds them
-- (sift._test_col_gdead_stats), and a query after more rows were cleared (DELETE and VACUUM, an UPDATE, a promotion, a merge, new rows at the freed addresses) makes them again. Every answer is the
-- table's without the index (p takes the same writes and is the oracle), between two queries of the same backend, and through a script of random writes.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SELECT setseed(0.42);

CREATE TABLE t (id int PRIMARY KEY, k int, u int, pad text DEFAULT repeat('x', 100));
ALTER TABLE t ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO t (id, k, u) SELECT i, (hashint4(i) & 16777215) % 2000, (hashint4(i + 5) & 16777215) % 1000003 FROM generate_series(1, 30000) i;
-- the segment keeps its dead rows (no rewrite below half of them): at 5% the price of a rewrite and the cost of the scans of the dirty units are within 2% of each other
CREATE INDEX t_ix ON t USING sift_col (id, k, u) WITH (order_array_columns = 'k, u', write_buffer_limit = 64, dead_rewrite_pct = 50);
CREATE TABLE p AS TABLE t;
ANALYZE t;
ANALYZE p;
SET sift.agg_force = on;

CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 'p')) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
-- ties (k has 2,000 values: the group the offset ends in is fetched, the groups before it are counted), a key with no repeats, descending, a filtered and a deep one
CREATE FUNCTION allq() RETURNS boolean LANGUAGE sql AS $$
	SELECT same('SELECT id, k FROM @ ORDER BY k, id LIMIT 20 OFFSET 4000') AND same('SELECT id, u FROM @ ORDER BY u, id LIMIT 20 OFFSET 6000')
	   AND same('SELECT id, k FROM @ ORDER BY k DESC, id LIMIT 20 OFFSET 12000') AND same('SELECT id, u FROM @ WHERE k < 1000 ORDER BY u DESC, id LIMIT 20 OFFSET 3000')
	   AND same('SELECT id, k FROM @ ORDER BY k, id LIMIT 20 OFFSET 20000') $$;
CREATE TABLE res (step text, equal boolean, hits int8, builds int8);
CREATE FUNCTION step(name text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE h0 int8; b0 int8; h1 int8; b1 int8; ok boolean;
BEGIN
	SELECT hits, builds INTO h0, b0 FROM sift._test_col_gdead_stats();
	ok := allq();
	SELECT hits, builds INTO h1, b1 FROM sift._test_col_gdead_stats();
	INSERT INTO res VALUES (name, ok, h1 - h0, b1 - b0);
END $$;

SELECT step('clean');
-- the first query after rows were cleared makes the lists (builds), the second finds them (hits)
DELETE FROM t WHERE id % 37 = 0;
DELETE FROM p WHERE id % 37 = 0;
VACUUM t;
SELECT step('delete, vacuum');
SELECT step('again');
UPDATE t SET k = k + 1 WHERE id % 41 = 0;
UPDATE p SET k = k + 1 WHERE id % 41 = 0;
SELECT step('update, no vacuum');
VACUUM t;
SELECT step('update, vacuum');
SELECT step('again');
-- new rows take the freed addresses
INSERT INTO t (id, k, u) SELECT 40000 + i, (hashint4(i + 9) & 16777215) % 2000, (hashint4(i + 13) & 16777215) % 1000003 FROM generate_series(1, 3000) i;
INSERT INTO p SELECT * FROM t WHERE id >= 40000;
SELECT step('insert');
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
SELECT step('promotion');
DELETE FROM t WHERE id % 43 = 0;
DELETE FROM p WHERE id % 43 = 0;
VACUUM t;
SELECT step('delete, vacuum');
SELECT step('again');
SELECT sift.col_merge('t_ix', true) >= 0 AS merged;
SELECT step('merge');
DELETE FROM t WHERE id % 47 = 0;
DELETE FROM p WHERE id % 47 = 0;
VACUUM t;
SELECT step('delete, vacuum');
SELECT step('again');
SELECT step, equal, hits > 0 AS found, builds > 0 AS made FROM res ORDER BY ctid;

-- a script of random writes, each followed by the queries (a statement of its own: VACUUM cannot share a transaction)
CREATE TABLE script (n serial, stmt text);
CREATE TABLE randres (n int, equal boolean);
INSERT INTO script (stmt)
SELECT stmt FROM (
	SELECT i, o, stmt FROM (
		SELECT i, CASE (random() * 5)::int
			WHEN 0 THEN ARRAY[format('DELETE FROM t WHERE id %% %s = %s', m, r), format('DELETE FROM p WHERE id %% %s = %s', m, r)]
			WHEN 1 THEN ARRAY[format('UPDATE t SET k = (hashint4(id + %s) & 16777215) %% 2000 WHERE id %% %s = %s', i, m, r), format('UPDATE p SET k = (hashint4(id + %s) & 16777215) %% 2000 WHERE id %% %s = %s', i, m, r)]
			WHEN 2 THEN ARRAY[format('INSERT INTO t (id, k, u) SELECT %s + g, (hashint4(g + %s) & 16777215) %% 2000, (hashint4(g + %s) & 16777215) %% 1000003 FROM generate_series(1, %s) g', 100000 + i * 5000, i, i + 3, 200 + m * 20),
						   format('INSERT INTO p SELECT * FROM t WHERE id > %s AND id <= %s', 100000 + i * 5000, 100000 + i * 5000 + 200 + m * 20)]
			WHEN 3 THEN ARRAY['SELECT sift.col_promote(''t_ix'') >= 0']
			ELSE ARRAY['VACUUM t'] END AS stmts
		FROM (SELECT i, 31 + (random() * 20)::int AS m, (random() * 10)::int AS r FROM generate_series(1, 12) i) s
	) a, unnest(a.stmts) WITH ORDINALITY u(stmt, o)
	UNION ALL SELECT i, 9, format('INSERT INTO randres SELECT %s, allq()', i) FROM generate_series(1, 12) i
) w ORDER BY i, o;
SELECT stmt FROM script ORDER BY n \gexec
SELECT count(*) AS runs, count(*) FILTER (WHERE equal) AS equal FROM randres;
SELECT sift.col_verify('t_ix');
DROP TABLE t, p, res, script, randres;
DROP FUNCTION same(text), allq(), step(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s7 CASCADE;
RESET client_min_messages;
-- <<< col_gorder_dead_cache

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
