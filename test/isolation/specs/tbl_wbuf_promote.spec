# A scan of a sift_table that a VACUUM interrupts while the rows are still in the write buffer: the VACUUM promotes the buffer into a column group
# under the scan, and the scan must go on with every row it has not returned yet. The row 1500 waits for the advisory lock 1 with built-in functions only.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_w (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_w_id ON iso_w (id);
	INSERT INTO iso_w SELECT g, 'row ' || md5(g::text) FROM generate_series(1, 3000) g;
	SELECT busy FROM sift.table_maintain('iso_w');
}

teardown
{
	DROP TABLE iso_w;
}

session r
step rseq	{ SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
		  UPDATE iso_w SET v = CASE WHEN id <> 1500 THEN 'u' WHEN pg_advisory_lock(1)::text <> '' OR NOT pg_advisory_unlock(1) THEN 'x' ELSE 'u' END WHERE id BETWEEN 1 AND 3000; }
step rbmp	{ SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = on; SET LOCAL enable_indexscan = off;
		  UPDATE iso_w SET v = CASE WHEN id <> 1500 THEN 'u' WHEN pg_advisory_lock(1)::text <> '' OR NOT pg_advisory_unlock(1) THEN 'x' ELSE 'u' END WHERE id BETWEEN 1 AND 3000; }
step ridx	{ SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = on;
		  UPDATE iso_w SET v = CASE WHEN id <> 1500 THEN 'u' WHEN pg_advisory_lock(1)::text <> '' OR NOT pg_advisory_unlock(1) THEN 'x' ELSE 'u' END WHERE id BETWEEN 1 AND 3000; }
step sbmp	{ SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = on; SET LOCAL enable_indexscan = off;
		  SELECT count(*) AS seen FROM iso_w WHERE id BETWEEN 1 AND 3000 AND (id <> 1500 OR (pg_advisory_lock(1)::text = '' AND pg_advisory_unlock(1))); }

session m
step mlock	{ SELECT pg_advisory_lock(1); }
step munlock	{ SELECT pg_advisory_unlock(1); }
step mcount	{ SELECT count(*) AS live, count(*) FILTER (WHERE v = 'u') AS updated FROM iso_w; }

session v
step vac	{ VACUUM iso_w; }

permutation mlock rseq vac munlock mcount
permutation mlock rbmp vac munlock mcount
permutation mlock ridx vac munlock mcount
permutation mlock sbmp vac munlock
