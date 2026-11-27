# SERIALIZABLE write skew through the deletion vector a projection reads past: s2 deletes a row of a sift_table and commits, a vacuum folds the delete into the deletion
# vector of the part while s1 still holds the snapshot that sees the row, and s1 reads the table through its pre-aggregate (SiftAggV6, the dead rows subtracted) or its sort
# projection (SiftProjSortScan, the dead rows left out). s1 read past a delete it does not see, s2 read what s1 then writes: s1 fails with 40001, as the read of the table does.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	DO $$ BEGIN
		IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
			PERFORM set_config('sift.test_create_format1', 'off', false);
		END IF;
	END $$;
	CREATE TABLE iso_pd (id int PRIMARY KEY, k int, v int) USING sift_table;
	INSERT INTO iso_pd SELECT i, i % 4, i % 10 FROM generate_series(1, 6000) i;
	SELECT sift.create_projection('iso_pd', 'pd_agg', 'GROUP BY k MEASURES count(*), sum(v)');
	SELECT sift.create_projection('iso_pd', 'pd_sort', 'ORDER BY v, id');
	CREATE TABLE iso_pc (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_pc SELECT i, i % 4, i % 10 FROM generate_series(1, 6000) i;
	CREATE INDEX iso_pc_idx ON iso_pc USING sift_col (id, k, v) WITH (key_order = 'id');
	SELECT sift.create_projection('iso_pc_idx', 'pc_agg', 'GROUP BY k MEASURES count(*), sum(v)');
	SELECT sift.create_projection('iso_pc_idx', 'pc_sort', 'ORDER BY v, id');
	CREATE TABLE iso_pw (id int PRIMARY KEY);
}

setup
{
	VACUUM iso_pd;
}

setup
{
	VACUUM (FREEZE) iso_pc;
}

setup
{
	CREATE FUNCTION iso_pd_node(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE l text; r text := 'table';
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			IF l ~ 'Custom Scan \(Sift' THEN r := substring(l from 'Custom Scan \((Sift[A-Za-z0-9]+)\)'); END IF;
			IF l ~ 'Projection: ' THEN r := r || ', ' || trim(l); END IF;
		END LOOP;
		RETURN r;
	END $$;
}

teardown
{
	DROP TABLE iso_pd, iso_pc, iso_pw;
	DROP FUNCTION iso_pd_node(text);
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET sift.agg_force = on; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) FROM iso_pw; }
step s1agg	{ SELECT iso_pd_node($$SELECT k, count(*), sum(v) FROM iso_pd GROUP BY k$$) AS node; SELECT k, count(*), sum(v) FROM iso_pd GROUP BY k ORDER BY k; }
step s1sort	{ SELECT iso_pd_node($$SELECT id FROM iso_pd WHERE v = 8 ORDER BY v, id LIMIT 2$$) AS node; SELECT id FROM iso_pd WHERE v = 8 ORDER BY v, id LIMIT 2; }
step s1cagg	{ SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SELECT iso_pd_node($$SELECT k, count(*), sum(v) FROM iso_pc GROUP BY k$$) AS node; SELECT k, count(*), sum(v) FROM iso_pc GROUP BY k ORDER BY k; }
step s1csort	{ SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SELECT iso_pd_node($$SELECT id FROM iso_pc WHERE v = 8 ORDER BY v, id LIMIT 2$$) AS node; SELECT id FROM iso_pc WHERE v = 8 ORDER BY v, id LIMIT 2; }
step s1w	{ INSERT INTO iso_pw VALUES (1); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) FROM iso_pw; }
step s2d	{ DELETE FROM iso_pd WHERE id = 8; }
step s2cd	{ DELETE FROM iso_pc WHERE id = 8; }
step s2c	{ COMMIT; }

session s3
step s3v	{ VACUUM iso_pd; }
step s3cv	{ VACUUM iso_pc; }

permutation s1b s2b s2d s2c s3v s1agg s1w s1c
permutation s1b s2b s2d s2c s3v s1sort s1w s1c
permutation s1b s2b s2cd s2c s3cv s1cagg s1w s1c
permutation s1b s2b s2cd s2c s3cv s1csort s1w s1c
