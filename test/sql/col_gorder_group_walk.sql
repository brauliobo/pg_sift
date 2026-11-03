-- ORDER BY g, key [LIMIT n OFFSET m], with g the column a second order is grouped by (order_array_group) and no order of its own: the second order lists the rows group after group, so the walk
-- of the order array serves it (Global Order: true) as an order array of g would, the OFFSET thrown away unread. The answers are the table's (a copy without an index is the oracle), whatever
-- the direction, the keys after the first, the NULL keys, a write buffer, a promotion, deleted and updated rows. A table with rows that have no g (they are not in the second order) does not
-- use it and answers all the same.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
\set VERBOSITY terse

CREATE TABLE gw (id int, g int, created timestamp, score int);
INSERT INTO gw SELECT i, 1 + abs(hashint4(i)) % 300, CASE WHEN i % 29 = 0 THEN NULL ELSE timestamp '2024-01-01' + abs(hashint4(i + 7)) % 5000000 * interval '1 second' END, abs(hashint4(i + 3)) % 1000
FROM generate_series(1, 40000) i;
CREATE TABLE gn (LIKE gw);
INSERT INTO gn SELECT id, CASE WHEN id % 41 = 0 THEN NULL ELSE g END, created, score FROM gw;
CREATE INDEX gw_i ON gw USING sift_col (id, g, created, score) WITH (key_cut = 'off', order_columns = 'created', order_array_columns = 'created', order_array_group = 'g');
CREATE INDEX gn_i ON gn USING sift_col (id, g, created, score) WITH (key_cut = 'off', order_columns = 'created', order_array_columns = 'created', order_array_group = 'g');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) gw;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) gn;
CREATE TABLE gp AS SELECT * FROM gw;
CREATE TABLE gq AS SELECT * FROM gn;
ANALYZE gp;
ANALYZE gq;

-- the answer is the oracle's; 'walk' says the order array served it
CREATE FUNCTION gw_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; walk bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || replace(q, '@', 'gw') LOOP
		walk := walk OR pl ~ 'Global Order: true';
	END LOOP;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gw') || ') t' INTO ra;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gp') || ') t' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN walk THEN 'walk' ELSE 'units' END;
END $$;
CREATE FUNCTION gw_all() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT gw_chk(format('SELECT id, g, created FROM @ ORDER BY %s LIMIT %s OFFSET %s', o, l, f)), 1::bigint
   FROM unnest(ARRAY['g, created, id', 'g DESC, created DESC, id', 'g DESC, id', 'g, score, id']) o,
		unnest(ARRAY[1, 20]) l, unnest(ARRAY[0, 7, 1000, 20000, 39990, 100000]) f $$;
SELECT result, count(*) FROM gw_all() GROUP BY 1 ORDER BY 1;
SELECT gw_chk('SELECT g FROM @ ORDER BY g LIMIT 50 OFFSET 3000') AS g_alone, gw_chk('SELECT g FROM @ ORDER BY g DESC LIMIT 50') AS g_alone_desc;

-- a condition on another column: the second order lists every row and tests nothing, so the walk is not used (the units are read, as for any other key) and the answers are the table's;
-- a condition that bounds g still walks. The conditions leave a tenth of the rows, a half, none, and the rows of a range of g
CREATE FUNCTION gw_flt() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT gw_chk(format('SELECT id, g, created FROM @ WHERE %s ORDER BY %s LIMIT %s OFFSET %s', w, o, l, f)), 1::bigint
   FROM unnest(ARRAY['score < 100', 'created > timestamp ''2024-02-01''', 'score < 0', 'id % 7 = 0 OR score = 5', 'score < 100 AND g > 250', 'g BETWEEN 100 AND 110 AND score < 500']) w,
		unnest(ARRAY['g, id', 'g DESC, created, id', 'g, score, id']) o, unnest(ARRAY[1, 20]) l, unnest(ARRAY[0, 700]) f $$;
SELECT result, count(*) FROM gw_flt() GROUP BY 1 ORDER BY 1;

-- the same with the sorted runs of g (order_columns): the scan that does not walk reads them, on and off, under the mask of the condition
CREATE TABLE gr (LIKE gw);
INSERT INTO gr SELECT * FROM gw;
CREATE INDEX gr_i ON gr USING sift_col (id, g, created, score) WITH (key_cut = 'off', order_columns = 'g, created', order_array_columns = 'created', order_array_group = 'g');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) gr;
CREATE FUNCTION gr_chk(q text, runs bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	EXECUTE format('SET LOCAL sift.topn_runs = %s', runs);
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gr') || ') t' INTO ra;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gp') || ') t' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q || ' runs ' || runs ELSE 'same' END;
END $$;
SELECT gr_chk(format('SELECT id, g FROM @ WHERE %s ORDER BY %s LIMIT %s OFFSET %s', w, o, l, f), r) AS result, count(*)
FROM unnest(ARRAY['score < 100', 'score < 0', 'created > timestamp ''2024-02-01''']) w, unnest(ARRAY['g, id', 'g DESC, id', 'g, created, id']) o, unnest(ARRAY[1, 100]) l, unnest(ARRAY[0, 300]) f, unnest(ARRAY[true, false]) r GROUP BY 1;

-- rows with no g are not in the second order: the walk is not used, the answers are the table's
CREATE FUNCTION gn_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; walk bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || replace(q, '@', 'gn') LOOP
		walk := walk OR pl ~ 'Global Order: true';
	END LOOP;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gn') || ') t' INTO ra;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'')) FROM (' || replace(q, '@', 'gq') || ') t' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN walk THEN 'walk' ELSE 'units' END;
END $$;
SELECT gn_chk('SELECT id, g, created FROM @ WHERE score < 100 ORDER BY g, created, id LIMIT 20 OFFSET 100') AS nulls_filtered, gn_chk('SELECT id, g, created FROM @ ORDER BY g, created, id LIMIT 20 OFFSET 1000') AS nulls_last, gn_chk('SELECT id, g, created FROM @ ORDER BY g DESC, created, id LIMIT 20 OFFSET 39000') AS desc_nulls_first;

-- a write buffer next to the segment, then a promotion, then rows deleted and updated
INSERT INTO gw SELECT 100000 + i, 1 + i % 330, timestamp '2024-03-01' + i * interval '1 second', i % 1000 FROM generate_series(1, 2500) i;
INSERT INTO gp SELECT * FROM gw WHERE id > 100000;
SELECT result, count(*) FROM gw_all() GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM gw_flt() GROUP BY 1 ORDER BY 1;
SELECT sift.col_promote('gw_i') >= 0 AS promoted;
VACUUM gw;
SELECT result, count(*) FROM gw_all() GROUP BY 1 ORDER BY 1;
DELETE FROM gw WHERE id % 53 = 0;
DELETE FROM gp WHERE id % 53 = 0;
UPDATE gw SET g = (g + 1) % 330 WHERE id % 97 = 0;
UPDATE gp SET g = (g + 1) % 330 WHERE id % 97 = 0;
SELECT result, count(*) FROM gw_all() GROUP BY 1 ORDER BY 1;
VACUUM gw;
SELECT result, count(*) FROM gw_all() GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM gw_flt() GROUP BY 1 ORDER BY 1;

DROP TABLE gw, gn, gp, gq, gr;
DROP FUNCTION gw_chk(text), gw_all(), gn_chk(text), gw_flt(), gr_chk(text, bool);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
