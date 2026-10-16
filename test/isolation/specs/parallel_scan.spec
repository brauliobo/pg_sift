# Inserts (sealing the mutable segment), promotion, deletes and VACUUM while a parallel scan is
# running. The scan's workers wait in a residual qual on a lock another session holds, after they
# bound the index; the result must be the reference matches of the scan's snapshot.
# requires: build insert vacuum scan promote verify score

setup
{
	CREATE TABLE iso_par (id int, body text, pad text) WITH (autovacuum_enabled = off, parallel_workers = 2);
	ALTER TABLE iso_par ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO iso_par SELECT g, CASE WHEN g % 3 = 0 THEN 'common rare' ELSE 'common' END || ' text ' || g, repeat('x', 1500)
		FROM generate_series(1, 3000) g;
	CREATE INDEX iso_par_idx ON iso_par USING sift (body) WITH (mutable_limit = 64);
	CREATE TABLE iso_gate ();
	CREATE FUNCTION iso_gate(i int) RETURNS bool LANGUAGE plpgsql STABLE PARALLEL SAFE
		AS $$ BEGIN PERFORM 1 FROM iso_gate; RETURN true; END $$;
	CREATE FUNCTION iso_par_ok() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		ok bool;
	BEGIN
		SELECT bool_and(g.ids IS NOT DISTINCT FROM CASE g.kind WHEN 'count' THEN ARRAY[cardinality(ref.ids)] ELSE ref.ids END) INTO ok
		FROM pg_temp.got g, (SELECT array_agg(id ORDER BY id) AS ids FROM iso_par WHERE sift_ref.match(body, 'common')) ref;
		RETURN ok;
	END $$;
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
	DROP TABLE iso_par, iso_gate;
	DROP FUNCTION iso_gate(int), iso_par_ok(), iso_par_parallel(text);
}

session s1
setup		{ CREATE TEMP TABLE IF NOT EXISTS got (kind text, ids int[]); TRUNCATE got; SET max_parallel_workers_per_gather = 2; SET sift.parallel_scan = on;
			  SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_index_scan_size = 0; }
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) > 0 AS snapshot FROM iso_par; }
step s1plan	{ SELECT iso_par_parallel('SELECT id FROM iso_par WHERE body @@> ''common'' AND iso_gate(id)') AS parallel_scan,
			  iso_par_parallel('SELECT id FROM iso_par WHERE body @@> ''common'' AND iso_gate(id) ORDER BY sift.score(ctid) DESC LIMIT 100000') AS parallel_rank; }
step s1scan	{ INSERT INTO got SELECT 'scan', array_agg(id ORDER BY id) FROM iso_par WHERE body @@> 'common' AND iso_gate(id); }
step s1rank	{ INSERT INTO got SELECT 'rank', array_agg(id ORDER BY id) FROM
			  (SELECT id FROM iso_par WHERE body @@> 'common' AND iso_gate(id) ORDER BY sift.score(ctid) DESC LIMIT 100000) t; }
step s1count	{ INSERT INTO got SELECT 'count', ARRAY[count(*)] FROM iso_par WHERE body @@> 'common' AND iso_gate(id); }
step s1check	{ SELECT iso_par_ok() AS ok; }
step s1c	{ COMMIT; }

session s2
step s2lock	{ BEGIN; LOCK TABLE iso_gate IN ACCESS EXCLUSIVE MODE; }
step s2unlock	{ COMMIT; }

session s3
step s3i	{ INSERT INTO iso_par SELECT g, 'common fresh ' || g || repeat(' filler', 20), repeat('x', 1500) FROM generate_series(5001, 5600) g; }
step s3p	{ SELECT sift.promote('iso_par_idx') >= 0 AS promoted; }
step s3d	{ DELETE FROM iso_par WHERE id % 3 = 0; }
step s3v	{ VACUUM iso_par; }
step s3check	{ SELECT iso_agree('iso_par', 'common') AS agree, iso_agree('iso_par', 'rare') AS agree_rare, iso_verify('iso_par_idx') AS verified; }

# rows inserted, sealed, promoted, deleted and vacuumed after the scan's snapshot do not change its result
permutation s2lock s1b s1plan s1scan s3i s3p s3d s3v s2unlock s1check s1c s3check
permutation s2lock s1b s1plan s1rank s3i s3p s3d s3v s2unlock s1check s1c s3check
permutation s2lock s1b s1plan s1count s3i s3p s3d s3v s2unlock s1check s1c s3check
