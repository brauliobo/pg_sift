# The workers of a parallel scan of a sift_table read the rows of the micro-slabs that the leader listed: the leader lists the units, and writers append to the slab before any participant
# opens its unit (they wait on the gate sift.test_phase_gate + 21 of a slab read again); the parallel answer agrees with the serial one. Two inserts that find the lock of the partition
# taken (a VACUUM that flushes the slab holds it on the gate sift.test_phase_gate + 10 of a commit of the manifest) make it hot: its slab becomes a directory of shards, read shard by shard and newest page first, which an append to one shard reorders.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_psv (id int, v int, pad text) USING sift_table WITH (autovacuum_enabled = false);
	ALTER TABLE iso_psv ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO iso_psv SELECT g, g % 100, repeat('x', 100) FROM generate_series(1, 20000) g;
	SELECT busy FROM sift.table_maintain('iso_psv');
	INSERT INTO iso_psv SELECT g, 1000 + g % 7, 'slab' FROM generate_series(20001, 20050) g;

	CREATE FUNCTION iso_psv_hot(regclass) RETURNS int4 LANGUAGE C STRICT AS 'pg_sift', 'sift_test_f2_l0_hot_dirs';
	CREATE FUNCTION iso_psv_q(gate int, OUT agree bool, OUT parallel bool) LANGUAGE plpgsql AS $$
	DECLARE
		q text := 'SELECT count(*), sum(v), count(DISTINCT id) FROM iso_psv';
		a text;
		b text;
		l text;
	BEGIN
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('max_parallel_workers_per_gather', '2', true);
		PERFORM set_config('parallel_setup_cost', '0', true);
		PERFORM set_config('parallel_tuple_cost', '0', true);
		PERFORM set_config('min_parallel_table_scan_size', '0', true);
		parallel := false;
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
			parallel := parallel OR l ~ 'Parallel Seq Scan';
		END LOOP;
		PERFORM set_config('sift.test_phase_gate', gate::text, true);
		EXECUTE q INTO a;
		PERFORM set_config('sift.test_phase_gate', '0', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE q INTO b;
		agree := a = b;
		IF NOT agree THEN
			RAISE WARNING 'parallel % serial %', a, b;
		END IF;
	END $$;
}

teardown
{
	DROP TABLE iso_psv;
	DROP FUNCTION iso_psv_q(int);
	DROP FUNCTION iso_psv_hot(regclass);
}

session s1
setup		{ SET sift.test_create_format1 = off; }
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_psv; }
step s1q	{ SELECT agree, parallel FROM iso_psv_q(7100); }
step s1c	{ COMMIT; }
step s1hot	{ SELECT iso_psv_hot('iso_psv') AS hot; }

session s2
step s2l	{ SELECT pg_advisory_lock(7121); }
step s2u	{ SELECT pg_advisory_unlock(7121); }

session s3
setup		{ SET sift.test_create_format1 = off; }
step s3m	{ VACUUM (FREEZE) iso_psv; }
step s3i	{ INSERT INTO iso_psv SELECT g, 2000, 'new' FROM generate_series(30001, 30060) g; }
step s3one	{ INSERT INTO iso_psv VALUES (40001, 3000, 'one'); }
step s3two	{ INSERT INTO iso_psv VALUES (40002, 3000, 'one'); }

session s4
setup		{ SET sift.test_create_format1 = off; SET sift.test_phase_gate = 7100; }
step s4v	{ VACUUM (FREEZE) iso_psv; }

session s7
step s7l	{ SELECT pg_advisory_lock(7110); }
step s7u	{ SELECT pg_advisory_unlock(7110); }

session s5
setup		{ SET sift.test_create_format1 = off; }
step s5one	{ INSERT INTO iso_psv VALUES (50001, 4000, 'five'); }
step s5two	{ INSERT INTO iso_psv VALUES (50002, 4000, 'five'); }

session s6
setup		{ SET sift.test_create_format1 = off; }
step s6one	{ INSERT INTO iso_psv VALUES (60001, 5000, 'six'); }
step s6two	{ INSERT INTO iso_psv VALUES (60002, 5000, 'six'); }

# the slab the leader listed is flushed and a new one is written while the participants wait to open their units
permutation s1b s2l s1q s3m s3i s2u s1c
# rows are added to the slab the leader listed
permutation s1b s2l s1q s3i s2u s1c
# the partition is made hot, writers append to the shards of its directory, the leader lists it and the writers append again before the participants open their units
permutation s3one s7l s4v s3two s7u s7l s4v s6one s7u s1hot s5one s5two s6two s1b s2l s1q s3one s5two s6two s2u s1c
