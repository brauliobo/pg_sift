# sift.table_maintain never waits for a reader or a writer: a reader's snapshot holds the horizon, so the rows deleted after it are not
# cleared and it keeps seeing them; once it ends the next call clears them; a VACUUM or another pass that holds the table makes the call
# return busy at once.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_m (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_m_id ON iso_m (id);
	INSERT INTO iso_m SELECT g, 'row ' || md5(g::text) FROM generate_series(1, 30000) g;
	SELECT busy FROM sift.table_maintain('iso_m');
}

# a scan that stops at its 100th row until the advisory lock 1 is free
setup
{
	CREATE FUNCTION iso_scan() RETURNS bigint LANGUAGE plpgsql AS $$
	DECLARE n bigint := 0; r record;
	BEGIN
		FOR r IN SELECT id FROM iso_m LOOP
			n := n + 1;
			IF n = 100 THEN PERFORM pg_advisory_lock(1); PERFORM pg_advisory_unlock(1); END IF;
		END LOOP;
		RETURN n;
	END $$;
}

teardown
{
	DROP FUNCTION iso_scan();
	DROP TABLE iso_m;
}

session r
step rscan	{ SELECT iso_scan() AS rows_seen; }
step rb		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS seen FROM iso_m; }
step rc		{ SELECT count(*) AS seen FROM iso_m; }
step re		{ COMMIT; }

session w
step wd		{ DELETE FROM iso_m WHERE id % 2 = 0; }
step wi		{ INSERT INTO iso_m SELECT g, 'again' FROM generate_series(30001, 40000) g; }

session m
step mlock	{ SELECT pg_advisory_lock(1); }
step munlock	{ SELECT pg_advisory_unlock(1); }
step mm		{ SELECT busy, dead_rows FROM sift.table_maintain('iso_m'); }
step mcount	{ SELECT count(*) AS live FROM iso_m; }

session l
step lb		{ BEGIN; LOCK TABLE iso_m IN SHARE UPDATE EXCLUSIVE MODE; }
step le		{ COMMIT; }

# the reader's snapshot holds the horizon: nothing cleared, nothing waits, the reader still sees every row; after it the rows are cleared
permutation rb wd mm rc re mm mcount
# the same with writers during and after
permutation rb wd wi mm rc re mm mcount
# a holder of the table's lock: busy at once, and the call works after it
permutation lb wd mm le mm mcount
# the pass rewrites the half-dead segment under a scan that began after the deletes, and under writers
permutation wd mlock rscan mm munlock mcount
permutation wd mlock rscan wi mm munlock mcount
