-- ORDER BY ts DESC, id DESC LIMIT N of the hits of a text query (memo @@> 'cable wire') in Custom Scan (SiftAggV6) when the hits tie on ts across several granules: the best rows are the hits of the
-- query with the largest ids of the newest day. Each statement runs with the node and without it (sift.enable_agg off) and the rows must be the same.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE tm (id int PRIMARY KEY, ts date NOT NULL, memo text NOT NULL);
INSERT INTO tm SELECT i, date '2025-01-01' + i % 3, CASE i % 4 WHEN 0 THEN 'cable wire' WHEN 1 THEN 'cable' ELSE 'plug' END FROM generate_series(1, 45000) i;
CREATE INDEX tm_col ON tm USING sift_col (id, ts);
CREATE INDEX tm_bm ON tm USING sift (memo);
VACUUM (FREEZE, ANALYZE) tm;

CREATE FUNCTION rows_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r record; node text; a text := ''; b text := '';
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP node := coalesce(node, substring(l FROM 'Custom Scan \((SiftAggV6\w*)\)')); END LOOP;
	FOR r IN EXECUTE q LOOP a := a || r::text || ';'; END LOOP;
	SET LOCAL sift.enable_agg = off;
	FOR r IN EXECUTE q LOOP b := b || r::text || ';'; END LOOP;
	RETURN CASE WHEN a <> b THEN 'DIFFERS: ' || a || ' vs ' || b WHEN node IS NULL THEN 'not engaged' ELSE 'same rows, ' || node END;
END $$;

SELECT q, rows_chk(q) FROM unnest(ARRAY[
	$$SELECT id, ts FROM tm WHERE memo @@> 'cable wire' ORDER BY ts DESC, id DESC LIMIT 8$$,
	$$SELECT id, ts FROM tm WHERE memo @@> 'cable' ORDER BY ts, id DESC LIMIT 8$$,
	$$SELECT id, ts FROM tm WHERE memo @@> 'cable' ORDER BY ts DESC, id DESC LIMIT 8 OFFSET 5$$
]) q;

DROP FUNCTION rows_chk(text);
DROP TABLE tm;
DROP EXTENSION pg_sift;
