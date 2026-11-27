# SERIALIZABLE write skew through SiftAggV6 over the sift_col index of a heap table: s2 reads kb and writes ka (an insert or an update), s1, whose snapshot is older, reads ka
# by max, percentile_disc or sift.approx_percentile (its exact query) and then inserts into kb. Each read misses the other's write, so the second to commit fails with 40001. The
# inserted row reaches the index only when s2 flushes its stage, at commit: that flush finds the SIREAD lock the aggregate scan left on the index.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE ka (id int, v float8) WITH (autovacuum_enabled = off);
	INSERT INTO ka SELECT i, i FROM generate_series(1, 20000) i;
	CREATE INDEX ka_col ON ka USING sift_col (id, v);
	CREATE TABLE kb (id int);
	CREATE FUNCTION ka_v6() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE l text; found bool := false;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT max(v), percentile_disc(0.5) WITHIN GROUP (ORDER BY v) FROM ka LOOP
			found := found OR l LIKE '%SiftAggV6%';
		END LOOP;
		RETURN found;
	END $$;
}

setup
{
	VACUUM (FREEZE, ANALYZE) ka;
}

teardown
{
	DROP TABLE ka, kb;
	DROP FUNCTION ka_v6();
}

session s1
setup		{ SET max_parallel_workers_per_gather = 0; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT ka_v6() AS v6; }
step s1max	{ SELECT max(v) AS m FROM ka; }
step s1pct	{ SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY v) AS p FROM ka; }
step s1apx	{ SELECT sift.approx_percentile('ka', 'v', 1) AS p; }
step s1w	{ INSERT INTO kb VALUES (1); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) AS n FROM kb; }
step s2i	{ INSERT INTO ka VALUES (20001, 30000); }
step s2u	{ UPDATE ka SET v = 30000 WHERE id = 5; }
step s2c	{ COMMIT; }

permutation s1b s2b s2i s1max s1w s2c s1c
permutation s1b s2b s2i s1max s1w s1c s2c
permutation s1b s2b s2u s1max s1w s2c s1c
permutation s1b s2b s2i s1pct s1w s2c s1c
permutation s1b s2b s2i s1apx s1w s2c s1c
