# SiftAgg answering the stored units of a sift_table from their pre-aggregates while writers and VACUUM move rows around it: snapshots older
# than a committed insert or delete, uncommitted and rolled back inserts and deletes, promotion by VACUUM. The step prints the rows the
# snapshot must see (plain plan), whether the pushed-down result with and without pre-aggregates equals it, and how many units answered.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_pa (id int, g int, v numeric(10,2)) USING sift_table;
	INSERT INTO iso_pa SELECT i, i % 7, i % 100 FROM generate_series(1, 120000) i;
	CREATE FUNCTION iso_pa_rows(q text, pre bool, push bool) RETURNS text LANGUAGE plpgsql AS $f$
	DECLARE r text;
	BEGIN
		PERFORM set_config('sift.agg_preagg', pre::text, true);
		PERFORM set_config('sift.enable_agg', push::text, true);
		PERFORM set_config('cpu_tuple_cost', '0.5', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
		RETURN r;
	END $f$;
	CREATE FUNCTION iso_pa_units(q text) RETURNS text LANGUAGE plpgsql AS $f$
	DECLARE l text; units int; pre int;
	BEGIN
		PERFORM set_config('sift.agg_preagg', 'on', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		PERFORM set_config('cpu_tuple_cost', '0.5', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
			pre := coalesce(substring(l from '(\d+) from pre-aggregates')::int, pre);
		END LOOP;
		RETURN CASE WHEN units IS NULL THEN 'no SiftAgg' WHEN pre IS NULL THEN 'scan' WHEN pre = 0 THEN 'none' WHEN pre = units THEN 'all' ELSE 'some' END;
	END $f$;
	CREATE FUNCTION iso_pa_check(OUT n bigint, OUT same bool, OUT pre text) LANGUAGE plpgsql AS $f$
	DECLARE q text := 'SELECT g, count(*), sum(v) FROM iso_pa GROUP BY g';
	BEGIN
		n := trim(both '()' from iso_pa_rows('SELECT count(*) FROM iso_pa', false, false))::bigint;
		same := iso_pa_rows(q, true, true) = iso_pa_rows(q, false, false) AND iso_pa_rows(q, false, true) = iso_pa_rows(q, false, false);
		pre := iso_pa_units(q);
	END $f$;
}

setup
{
	VACUUM iso_pa;
}

teardown
{
	DROP TABLE iso_pa;
	DROP FUNCTION iso_pa_rows(text, bool, bool), iso_pa_units(text), iso_pa_check();
}

session s1
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pa; }
step s1q	{ SELECT * FROM iso_pa_check(); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2del	{ DELETE FROM iso_pa WHERE id % 50 = 0; }
step s2ins	{ INSERT INTO iso_pa SELECT 200000 + i, i % 7, i FROM generate_series(1, 3000) i; }
step s2upd	{ UPDATE iso_pa SET v = v + 1, g = 3 WHERE id % 1000 = 0; }
step s2c	{ COMMIT; }
step s2a	{ ROLLBACK; }

session s3
step s3v	{ VACUUM iso_pa; }

# the snapshot predates a committed delete, insert or update, and VACUUM runs in between
permutation s1rr s2del s3v s1q s1c s1q
permutation s1rr s2ins s3v s1q s1c s1q
permutation s1rr s2upd s3v s1q s1c s1q

# uncommitted changes of another session, with VACUUM running while they are open
permutation s2b s2del s3v s1q s2c s1q
permutation s2b s2ins s3v s1q s2c s1q
permutation s2b s2upd s3v s1q s2a s3v s1q

# rolled back changes
permutation s2b s2del s2a s3v s1q
permutation s2b s2ins s2a s3v s1q
