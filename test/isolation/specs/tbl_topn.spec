# ORDER BY .. LIMIT on a sift_table (the top-N scan) while writers and VACUUM move rows around it: snapshots older than a committed
# insert, delete or update, uncommitted and rolled back changes, promotion by VACUUM. The new version of an updated row goes to the write
# buffer whatever its sort value, so the best rows are not in the newest superblocks. The step prints whether the top-N result equals the
# plain plan's and whether the plan was the top-N scan.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_tn (id int, g int, seq int, pad text DEFAULT repeat('x', 1500)) USING sift_table;
	INSERT INTO iso_tn (id, g, seq) SELECT i, i % 7, i FROM generate_series(1, 30000) i;
	CREATE FUNCTION iso_tn_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $f$
	DECLARE r text;
	BEGIN
		PERFORM set_config('sift.enable_agg', push::text, true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO r;
		RETURN r;
	END $f$;
	CREATE FUNCTION iso_tn_plan(q text) RETURNS text LANGUAGE plpgsql AS $f$
	DECLARE l text; r text := '';
	BEGIN
		PERFORM set_config('sift.enable_agg', 'on', true);
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP r := r || l; END LOOP;
		RETURN CASE WHEN r LIKE '%Top-N:%' THEN 'top-n' ELSE 'other' END;
	END $f$;
	CREATE FUNCTION iso_tn_check(OUT n bigint, OUT same bool, OUT plan text) LANGUAGE plpgsql AS $f$
	DECLARE
		qd text := 'SELECT id, seq FROM iso_tn WHERE g IN (1, 3) ORDER BY seq DESC, id LIMIT 12';
		qa text := 'SELECT id, seq FROM iso_tn WHERE g IN (1, 3) ORDER BY seq, id LIMIT 12';
	BEGIN
		n := trim(both '()' from iso_tn_rows('SELECT count(*) FROM iso_tn', false))::bigint;
		same := iso_tn_rows(qd, true) = iso_tn_rows(qd, false) AND iso_tn_rows(qa, true) = iso_tn_rows(qa, false);
		plan := iso_tn_plan(qd);
	END $f$;
}

setup
{
	VACUUM ANALYZE iso_tn;
}

teardown
{
	DROP TABLE iso_tn;
	DROP FUNCTION iso_tn_rows(text, bool), iso_tn_plan(text), iso_tn_check();
}

session s1
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_tn; }
step s1q	{ SELECT * FROM iso_tn_check(); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2del	{ DELETE FROM iso_tn WHERE id > 29000 OR id < 1000; }
step s2ins	{ INSERT INTO iso_tn (id, g, seq) SELECT 40000 + i, i % 7, 40000 + i FROM generate_series(1, 3000) i; }
step s2upd	{ UPDATE iso_tn SET seq = seq + 1000000 WHERE id % 1500 = 0; }
step s2low	{ UPDATE iso_tn SET seq = -seq WHERE id % 1500 = 1; }
step s2c	{ COMMIT; }
step s2a	{ ROLLBACK; }

session s3
step s3v	{ VACUUM iso_tn; }

# the snapshot predates a committed delete, insert or update, and VACUUM runs in between
permutation s1rr s2del s3v s1q s1c s1q
permutation s1rr s2ins s3v s1q s1c s1q
permutation s1rr s2upd s2low s3v s1q s1c s1q

# updated rows land in the write buffer with values far from the superblock's neighbours
permutation s2upd s2low s1q s3v s1q

# uncommitted changes of another session, with VACUUM running while they are open
permutation s2b s2del s3v s1q s2c s1q
permutation s2b s2upd s2low s3v s1q s2c s1q
permutation s2b s2ins s3v s1q s2c s1q

# rolled back changes
permutation s2b s2del s2a s3v s1q
permutation s2b s2upd s2a s3v s1q
