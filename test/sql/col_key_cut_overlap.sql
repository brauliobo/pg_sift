-- The runs a build writes when the rows of the table outgrow the memory of its segment writer are each sorted by the key of a key_cut index, and each holds rows
-- of every key: a lookup of one key reads a unit in each run. On an index larger than sift.vacuum_rewrite_limit runs as large as the partitions of a rewrite were
-- taken for partitions, so nothing ever merged them: the rewrite is due while two of them overlap in the key, and cuts the key range into partitions that do not.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.vacuum_rewrite_limit = 1;
SET sift.test_col_segment_kb = 7000;
SET sift.col_build_key_ranges = off;

CREATE TABLE ko (id int, tenant_id int, d date, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ko SELECT i, (i * 7919) % 400, date '2026-01-01' + (i * 31) % 300, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 168960) i;
VACUUM (FREEZE, ANALYZE) ko;
CREATE INDEX ko_idx ON ko USING sift_col (id, tenant_id, d, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off);

-- four runs of 42,240 rows, as large as partitions (a rewrite of this index cuts at 1 MB) and holding rows of every tenant
SELECT count(*) AS runs, sum(units) AS units, min(bytes) > 524288 AS partition_sized FROM sift.col_segments('ko_idx') WHERE kind = 'immutable';

CREATE FUNCTION ko_same() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY['SELECT count(*), sum(amount), min(id), max(d) FROM ko WHERE tenant_id = 42', 'SELECT tenant_id, count(*), sum(amount) FROM ko WHERE tenant_id IN (3, 77, 399) GROUP BY 1 ORDER BY 1', 'SELECT count(*), sum(amount) FROM ko WHERE tenant_id BETWEEN 100 AND 130'] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO b;
		SET LOCAL sift.enable_agg = on;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;
CREATE FUNCTION ko_units() RETURNS text LANGUAGE sql AS $$
	SELECT sift_helpers.plan_units_lines('SELECT count(*), sum(amount) FROM ko WHERE tenant_id = 42') $$;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;

-- a key is read in a unit of each run, and the rewrite is due though no segment is a small overlay or holds a dead row; the WAL budget of the churn does not hold it
SELECT ko_units() AS before;
SELECT ko_same() AS same_before;
SELECT sift.col_maintenance_due('ko_idx', true) AS due;

-- the rewrite cuts the key range into partitions: a key is in one of them, and nothing is due again
SELECT sift.col_maintain('ko_idx') AS rewritten;
SELECT sift.col_verify('ko_idx', true);
SELECT ko_units() AS after;
SELECT ko_same() AS same_after;
SELECT sift.col_maintenance_due('ko_idx', true) AS due_again;
SELECT sift.col_maintain('ko_idx') AS rewritten_again;
DROP TABLE ko;
DROP FUNCTION ko_same(), ko_units();
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
