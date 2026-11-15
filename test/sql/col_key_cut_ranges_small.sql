-- A table of a run or two past the memory of a run (under three of them) is built by ranges of the key as a larger one is: its runs do not overlap, so a key is
-- in one unit and nothing is due after the build (sift_col build spill.c). The runs of the table's order (the setting off) overlap and are rewritten.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 1500;

CREATE TABLE ks (id int, tenant_id int, note text, price numeric(10,2), pad text) WITH (autovacuum_enabled = false);
INSERT INTO ks SELECT i, (i::bigint * 7919 % 300)::int, 'note ' || (i % 50), (i % 1000) / 4.0, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 36000) i;
VACUUM (FREEZE, ANALYZE) ks;
SELECT pg_relation_size('ks') < 3 * 1500 * 1024 AS under_three_runs, pg_relation_size('ks') > 1500 * 1024 AS over_one_run;

CREATE FUNCTION ks_units(ix regclass) RETURNS numeric LANGUAGE sql AS $$
	SELECT round(point_units::numeric, 1) FROM sift.col_clustering(ix, 1) WHERE path = 'tenant_id' $$;

CREATE FUNCTION ks_same() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY['SELECT count(*), sum(price), min(note) FROM ks WHERE tenant_id = 42', 'SELECT tenant_id, count(*), sum(id) FROM ks WHERE tenant_id < 5 GROUP BY 1 ORDER BY 1'] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO b;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;

SET sift.col_build_key_ranges = off;
CREATE INDEX ks_runs ON ks USING sift_col (id, tenant_id, note, price, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT count(*) > 1 AS several_runs FROM sift.col_segments('ks_runs') WHERE kind = 'immutable';
SELECT ks_units('ks_runs') > 1 AS units_in_every_run;
DROP INDEX ks_runs;

SET sift.col_build_key_ranges = on;
CREATE INDEX ks_idx ON ks USING sift_col (id, tenant_id, note, price, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT count(*) > 1 AS several_runs FROM sift.col_segments('ks_idx') WHERE kind = 'immutable';
SELECT ks_units('ks_idx') AS units_for_a_key;
SELECT sift.col_maintenance_due('ks_idx') AS ranges_due;
SELECT sift.col_verify('ks_idx', true);
SELECT ks_same() AS same_rows;

RESET sift.test_col_segment_kb;
DROP TABLE ks;
DROP FUNCTION ks_units(regclass), ks_same();
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
