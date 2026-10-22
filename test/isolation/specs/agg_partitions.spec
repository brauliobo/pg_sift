# A partitioned table is one source of Custom Scan (SiftAgg), each partition read through its own index. A partition attached (it gets the table's index), detached (also
# concurrently) or rows moved between partitions while a scan runs, in a snapshot older than the change or not, give the rows of the normal plan, and a prepared
# statement made before a partition changed is planned again with the partitions it has now.
# requires: build col

setup
{
	CREATE TABLE iso_pp (id int, g int, d date NOT NULL, v int) PARTITION BY RANGE (d);
	CREATE TABLE iso_pp_a PARTITION OF iso_pp FOR VALUES FROM ('2024-01-01') TO ('2024-04-01') WITH (autovacuum_enabled = off);
	CREATE TABLE iso_pp_b PARTITION OF iso_pp FOR VALUES FROM ('2024-04-01') TO ('2024-07-01') WITH (autovacuum_enabled = off);
	INSERT INTO iso_pp SELECT i, i % 5, DATE '2024-01-01' + i % 180, i FROM generate_series(1, 3000) i;
	CREATE INDEX iso_pp_col ON iso_pp USING sift_col (id, g, d, v);
	CREATE TABLE iso_pp_n (LIKE iso_pp INCLUDING DEFAULTS) WITH (autovacuum_enabled = off);
	INSERT INTO iso_pp_n SELECT 10000 + i, i % 5, DATE '2024-07-01' + i % 90, i FROM generate_series(1, 500) i;
	-- split by hash: the remainders of the values of a statement decide the partitions its scan reads
	CREATE TABLE iso_ph (id int, k int, v int) PARTITION BY HASH (k);
	CREATE TABLE iso_ph_0 PARTITION OF iso_ph FOR VALUES WITH (MODULUS 4, REMAINDER 0) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_ph_1 PARTITION OF iso_ph FOR VALUES WITH (MODULUS 4, REMAINDER 1) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_ph_2 PARTITION OF iso_ph FOR VALUES WITH (MODULUS 4, REMAINDER 2) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_ph_3 PARTITION OF iso_ph FOR VALUES WITH (MODULUS 4, REMAINDER 3) WITH (autovacuum_enabled = off);
	INSERT INTO iso_ph SELECT i, CASE WHEN i % 50 = 0 THEN NULL ELSE i % 60 END, i FROM generate_series(1, 3000) i;
	CREATE INDEX iso_ph_col ON iso_ph USING sift_col (id, k, v);
}

setup		{ VACUUM (FREEZE, ANALYZE) iso_pp; }
setup		{ VACUUM (FREEZE, ANALYZE) iso_pp_n; }
setup		{ VACUUM (FREEZE, ANALYZE) iso_ph; }

setup
{
	-- the plan of a statement (SiftAgg nodes, the partitions it read), and whether its rows are those of the normal plan in the snapshot of the statement
	CREATE FUNCTION iso_pp_run(stmt text, ref text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		nodes int := 0;
		listed int := 0;
		removed int := 0;
		differences bigint;
	BEGIN
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || stmt LOOP
			IF l LIKE '%Custom Scan (SiftAgg)%' THEN nodes := nodes + 1; END IF;
			IF l LIKE '%Partitions:%' THEN listed := array_length(string_to_array(l, ','), 1); END IF;
			IF l LIKE '%Partitions Removed:%' THEN removed := trim(split_part(l, ':', 2))::int; END IF;
		END LOOP;
		EXECUTE 'CREATE TEMP TABLE iso_pp_on AS ' || stmt;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'CREATE TEMP TABLE iso_pp_off AS ' || ref;
		PERFORM set_config('sift.enable_agg', 'on', true);
		differences := (SELECT count(*) FROM ((TABLE iso_pp_on EXCEPT TABLE iso_pp_off) UNION ALL (TABLE iso_pp_off EXCEPT TABLE iso_pp_on)) d);
		DROP TABLE iso_pp_on, iso_pp_off;
		RETURN format('%s SiftAgg nodes, %s partitions read, %s differences from the normal plan', nodes, listed - removed, differences);
	END $$;
}

teardown
{
	DROP FUNCTION iso_pp_run(text, text);
	DROP TABLE iso_pp, iso_pp_n, iso_ph;
	DROP TABLE IF EXISTS iso_ph_3;
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 1000; SET random_page_cost = 1000; }
step s1run	{ SELECT iso_pp_run('SELECT g, count(*) AS n, sum(v) AS s FROM iso_pp GROUP BY g', 'SELECT g, count(*) AS n, sum(v) AS s FROM iso_pp GROUP BY g'); }
step s1win	{ SELECT iso_pp_run('SELECT g, count(*) AS n, sum(v) AS s FROM iso_pp WHERE d >= DATE ''2024-05-01'' + (current_date - current_date) GROUP BY g',
                           'SELECT g, count(*) AS n, sum(v) AS s FROM iso_pp WHERE d >= DATE ''2024-05-01'' GROUP BY g'); }
step s1prep	{ SET plan_cache_mode = force_generic_plan;
		  PREPARE iso_pp_p (date) AS SELECT g, count(*) AS n, sum(v) AS s FROM iso_pp WHERE d >= $1 GROUP BY g; }
step s1gen	{ SELECT iso_pp_run('EXECUTE iso_pp_p(DATE ''2024-05-01'')', 'SELECT g, count(*) AS n, sum(v) AS s FROM iso_pp WHERE d >= DATE ''2024-05-01'' GROUP BY g'); }
step s1hprep	{ SET plan_cache_mode = force_generic_plan;
		  PREPARE iso_ph_p (int[]) AS SELECT count(*) AS n, sum(v) AS s FROM iso_ph WHERE k = ANY ($1); }
step s1hgen	{ SELECT iso_pp_run('EXECUTE iso_ph_p(ARRAY[7, 12])', 'SELECT count(*) AS n, sum(v) AS s FROM iso_ph WHERE k = ANY (ARRAY[7, 12])'); }
step s1hrr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_ph; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_pp; }
step s1end	{ COMMIT; }

session s2
step s2attach	{ ALTER TABLE iso_pp ATTACH PARTITION iso_pp_n FOR VALUES FROM ('2024-07-01') TO ('2024-10-01'); }
step s2detach	{ ALTER TABLE iso_pp DETACH PARTITION iso_pp_n; }
step s2detachc	{ ALTER TABLE iso_pp DETACH PARTITION iso_pp_n CONCURRENTLY; }
step s2hdetach	{ ALTER TABLE iso_ph DETACH PARTITION iso_ph_3; }
step s2hdetachc	{ ALTER TABLE iso_ph DETACH PARTITION iso_ph_3 CONCURRENTLY; }
step s2hattach	{ ALTER TABLE iso_ph ATTACH PARTITION iso_ph_3 FOR VALUES WITH (MODULUS 4, REMAINDER 3); }
step s2hmove	{ UPDATE iso_ph SET k = k + 1 WHERE id % 7 = 0; }
step s2begin	{ BEGIN; }
step s2move	{ UPDATE iso_pp SET d = CASE WHEN d >= DATE '2024-04-01' THEN d - 91 ELSE d + 91 END WHERE id % 7 = 0; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }

# a partition attached between two scans, and one attached while a snapshot older than the attach scans
permutation s1run s2attach s1run s1win
permutation s1rr s2attach s1run s1end s1run
# detached: waits for the scan's transaction (the lock on the table), then the scan reads the partitions left
permutation s2attach s1rr s2detach s1run s1end s1run
# detached concurrently: waits for the transactions that can still see the partition
permutation s2attach s1rr s2detachc s1run s1end s1run s1win
# rows moved to another partition: in flight, committed, rolled back
permutation s2begin s2move s1run s1win s2commit s1run s1win
permutation s2begin s2move s1run s2abort s1run
permutation s1rr s2move s1run s1end s1run
# a prepared statement planned before the partition changed
permutation s1prep s1gen s2attach s1gen s2detach s1gen

# split by hash: the partitions the remainders of the values leave, with a partition detached (also concurrently) and attached again;
# a prepared statement made before the partition went is planned again
permutation s1hprep s1hgen s2hdetach s1hgen s2hattach s1hgen
permutation s1hrr s2hdetach s1hgen s1end s1hgen s2hattach s1hgen
permutation s1hrr s2hdetachc s1hgen s1end s1hgen s2hattach s1hgen
# rows moved to other remainders: in flight, committed, rolled back, and under an older snapshot
permutation s2begin s2hmove s1hgen s2commit s1hgen
permutation s2begin s2hmove s1hgen s2abort s1hgen
permutation s1hrr s2hmove s1hgen s1end s1hgen
