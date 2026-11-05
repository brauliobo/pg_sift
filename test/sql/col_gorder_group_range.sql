-- count(*), sums and the rows of WHERE g = x AND k BETWEEN a AND b, with k an array grouped by g (order_array_group), come from the entries of the group between the two keys: a count reads
-- no unit, the other aggregates read the rows the entries name. A group that is not there, an empty or open range, NULL keys, a write buffer, a promotion, deleted, updated and rolled back
-- rows and VACUUM keep the heap's answers; so does a group order that sits in the first slot (an explicit group) or the second (the build's own).
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE gr (id int, ent_id int, created timestamp, score int, pad text);
ALTER TABLE gr ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gr SELECT i, abs(hashint4(i)) % 400, CASE WHEN i % 41 = 0 THEN NULL ELSE timestamp '2024-01-01' + abs(hashint4(i + 7)) % 5000000 * interval '1 second' END,
	   abs(hashint4(i + 5)) % 5000, md5(i::text) || md5(i::text || 'x') || md5(i::text || 'y')
FROM generate_series(1, 120000) i;
CREATE TABLE gs (LIKE gr);
ALTER TABLE gs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gs SELECT * FROM gr;
CREATE INDEX gr_i ON gr USING sift_col (id, ent_id, created, score, pad) WITH (key_cut = 'off', order_columns = 'created', order_array_columns = 'created', order_array_group = 'ent_id');
CREATE INDEX gs_i ON gs USING sift_col (id, ent_id, created, score, pad) WITH (key_cut = 'off', order_columns = 'created', order_array_columns = 'created', order_array_group = 'auto: ent_id');
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
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
