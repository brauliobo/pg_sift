# A statement that scans a sift_table and writes it reads the tombstone log again at every row. A VACUUM that runs in the middle of it clears
# the liveness of the rows a committed DELETE removed and then writes the log without their entries: the unit the scan listed before still
# has them, nothing hides them any more, and the UPDATE would fail fetching a row it was just given ("failed to fetch tuple being updated"),
# as a background defrag pass made it do. The scan has to list its unit again once the statement has read the new log.
# The row 1500 waits for the advisory lock 1 with built-in functions only: a function of the language plpgsql would let the backend process
# the invalidation the VACUUM sends, and the statement would then start from a new state.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_e (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_e_id ON iso_e (id);
	INSERT INTO iso_e SELECT g, 'row ' || md5(g::text) FROM generate_series(1, 3000) g;
}

# in a transaction of its own: the rows settle into a column group
setup
{
	SELECT busy FROM sift.table_maintain('iso_e');
}

teardown
{
	DROP TABLE iso_e;
}

session r
step rseq	{ SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
		  UPDATE iso_e SET v = CASE WHEN id <> 1500 THEN 'u' WHEN pg_advisory_lock(1)::text <> '' OR NOT pg_advisory_unlock(1) THEN 'x' ELSE 'u' END WHERE id BETWEEN 1 AND 3000; }
step rbmp	{ SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = on; SET LOCAL enable_indexscan = off;
		  UPDATE iso_e SET v = CASE WHEN id <> 1500 THEN 'u' WHEN pg_advisory_lock(1)::text <> '' OR NOT pg_advisory_unlock(1) THEN 'x' ELSE 'u' END WHERE id BETWEEN 1 AND 3000; }
step ridx	{ SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = on;
		  UPDATE iso_e SET v = CASE WHEN id <> 1500 THEN 'u' WHEN pg_advisory_lock(1)::text <> '' OR NOT pg_advisory_unlock(1) THEN 'x' ELSE 'u' END WHERE id BETWEEN 1 AND 3000; }

session w
step wd		{ DELETE FROM iso_e WHERE id % 20 = 1; }

session m
step mlock	{ SELECT pg_advisory_lock(1); }
step munlock	{ SELECT pg_advisory_unlock(1); }
step mcount	{ SELECT count(*) AS live, count(*) FILTER (WHERE v = 'u') AS updated FROM iso_e; }

session v
step vac	{ VACUUM iso_e; }

# the DELETE is older than the statement, the VACUUM runs in the middle of it
permutation wd mlock rseq vac munlock mcount
permutation wd mlock rbmp vac munlock mcount
permutation wd mlock ridx vac munlock mcount
