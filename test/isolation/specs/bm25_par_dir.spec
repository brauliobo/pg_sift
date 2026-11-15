# The participants of a parallel unranked SiftScan bind the index on one directory: the leader's, exported before the workers start. The leader holds on the test
# gate once it exported, rows are inserted, sealed, promoted, deleted and vacuumed, then the workers start and read the write buffers and segments the exported
# directory names: the result is the reference matches of the scan's snapshot.
# requires: build insert vacuum scan promote verify score gates

setup
{
	CREATE TABLE iso_par (id int, body text, pad text) WITH (autovacuum_enabled = off, parallel_workers = 2);
	ALTER TABLE iso_par ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO iso_par SELECT g, CASE WHEN g % 3 = 0 THEN 'common rare' ELSE 'common' END || ' text ' || g, repeat('x', 1500)
		FROM generate_series(1, 3000) g;
	CREATE INDEX iso_par_idx ON iso_par USING sift (body) WITH (write_buffer_limit = 64);
	CREATE FUNCTION iso_par_parallel(q text) RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		plan text := '';
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
			plan := plan || l;
		END LOOP;
		RETURN current_setting('iso.mode', true) IS DISTINCT FROM 'sift' OR plan LIKE '%Parallel Custom Scan (SiftScan)%';
	END $$;
}

teardown
{
	DROP TABLE iso_par;
	DROP FUNCTION iso_par_parallel(text);
}

session s1
setup		{ SET max_parallel_workers_per_gather = 2; SET sift.parallel_scan = on;
			  SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_index_scan_size = 0; SET sift.test_export_gate = on; }
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) > 0 AS snapshot FROM iso_par; }
step s1plan	{ SELECT iso_par_parallel('SELECT id FROM iso_par WHERE body @@> ''common''') AS parallel_scan; }
step s1scan	{ SELECT count(*) AS n, sum(id) AS ids FROM iso_par WHERE body @@> 'common'; }
step s1ref	{ SELECT count(*) AS n, sum(id) AS ids FROM iso_par WHERE sift_ref.match(body, 'common'); }
step s1c	{ COMMIT; }

session s2
step s2lock	{ SELECT pg_advisory_lock(20983, 5); }
step s2unlock	{ SELECT pg_advisory_unlock(20983, 5); }

session s3
step s3i	{ INSERT INTO iso_par SELECT g, 'common fresh ' || g || repeat(' filler', 20), repeat('x', 1500) FROM generate_series(5001, 5600) g; }
step s3p	{ SELECT sift.promote('iso_par_idx') >= 0 AS promoted; }
step s3d	{ DELETE FROM iso_par WHERE id % 3 = 0; }
step s3v	{ VACUUM iso_par; }

permutation s2lock s1b s1plan s1scan s3i s3p s3d s3v s2unlock s1ref s1c
