-- A top-N by an order array of the build's choice ('auto: ..') walks the arrays (Global Order, no unit read) while a small promotion is alive: the walk reads the array of
-- every segment, so an overlay without one would send the scan back to the units (all the rows). Answers are the heap's, with dead rows and after the merge.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET sift.col_auto_merge = off;
CREATE TABLE gw (id int, g int, ts timestamp, amount int) WITH (autovacuum_enabled = off);
INSERT INTO gw SELECT i, i % 50, TIMESTAMP '2026-01-01' + (i::bigint * 7919 % 100003) * INTERVAL '1 minute', i % 100 FROM generate_series(1, 200000) i;
CREATE INDEX gw_ix ON gw USING sift_col (id, g, ts, amount) WITH (key_cut = 'off', order_columns = 'id', order_array_columns = 'auto: ts');
VACUUM (FREEZE, ANALYZE) gw;

CREATE FUNCTION gw_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; walk bool := false; units int;
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		walk := walk OR r LIKE '%Global Order: true%';
		IF r LIKE '%Units: %' THEN units := substring(r FROM 'Units: (\d+) read')::int; END IF;
	END LOOP;
	RETURN CASE WHEN walk THEN 'array walk, ' || units || ' units read' ELSE 'no array walk' END;
END $$;
CREATE FUNCTION gw_answers() RETURNS text LANGUAGE sql AS $$
  SELECT md5(concat_ws('|',
    (SELECT string_agg(id::text, ',' ORDER BY ts DESC, id) FROM (SELECT id, ts FROM gw ORDER BY ts DESC, id LIMIT 30) x),
    (SELECT string_agg(id::text, ',' ORDER BY ts, id) FROM (SELECT id, ts FROM gw ORDER BY ts, id LIMIT 30 OFFSET 10) x))) $$;
\set top 'SELECT id, ts FROM gw ORDER BY ts DESC LIMIT 20'

SELECT gw_plan(:'top') AS no_overlay;
INSERT INTO gw SELECT 200000 + i, i % 50, TIMESTAMP '2026-03-01' + i * INTERVAL '1 minute', i % 100 FROM generate_series(1, 2000) i;
SELECT sift.col_promote('gw_ix') >= 0 AS promoted;
SELECT count(*) AS overlays FROM sift.col_segments('gw_ix') WHERE kind = 'immutable' AND rows < 5000;
SELECT gw_plan(:'top') AS small_overlay;
SET sift.enable_agg = off;
SELECT gw_answers() AS native \gset
RESET sift.enable_agg;
SELECT gw_answers() = :'native' AS same_answers_overlay;

DELETE FROM gw WHERE id % 7 = 0 AND id > 190000;
VACUUM gw;
SELECT gw_plan(:'top') AS small_overlay_dead;
SET sift.enable_agg = off;
SELECT gw_answers() AS native_dead \gset
RESET sift.enable_agg;
SELECT gw_answers() = :'native_dead' AS same_answers_dead;

SELECT sift.col_merge('gw_ix', true) > 0 AS merged;
SELECT gw_plan(:'top') AS merged_plan;
SELECT gw_answers() = :'native_dead' AS same_answers_merged;
SELECT sift.col_verify('gw_ix', true);

DROP TABLE gw;
DROP FUNCTION gw_plan(text), gw_answers();
DROP EXTENSION pg_sift;
