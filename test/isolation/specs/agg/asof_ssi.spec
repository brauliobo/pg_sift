# SERIALIZABLE and the ASOF join of SiftAggV6Asof over the sift_col index of the dim table (Format 2.0): two transactions that each join the facts to the latest dim row before them
# and then insert, the one a fact and the other a dim row, are a write skew, and the second one to commit fails with 40001: the dim side, read from the index, locks the dim heap.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_ad (k int, ts bigint, px int) WITH (autovacuum_enabled = false);
	INSERT INTO iso_ad SELECT g % 50, g::bigint * 13, g FROM generate_series(1, 20000) g;
	CREATE INDEX iso_ad_idx ON iso_ad USING sift_col (k, ts, px) WITH (key_order = 'k, ts');
	CREATE TABLE iso_af (k int, ts bigint, id int) WITH (autovacuum_enabled = false);
	INSERT INTO iso_af SELECT g % 50, g::bigint * 10 + g % 7, g FROM generate_series(1, 20000) g;
	CREATE INDEX iso_af_idx ON iso_af USING sift_col (k, ts, id) WITH (key_order = 'k, ts');
	CREATE FUNCTION iso_ad_asof() RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE l text; found bool := false;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF) SELECT f.id, q.px FROM iso_af f LEFT JOIN LATERAL (SELECT px FROM iso_ad d WHERE d.k = f.k AND d.ts <= f.ts ORDER BY d.ts DESC LIMIT 1) q ON true WHERE f.k = 3 LOOP
			found := found OR l LIKE '%SiftAggV6Asof%';
		END LOOP;
		RETURN found;
	END $$;
}

setup
{
	VACUUM (FREEZE, ANALYZE) iso_af, iso_ad;
}

teardown
{
	DROP TABLE iso_af, iso_ad;
	DROP FUNCTION iso_ad_asof();
}

session s1
setup		{ SET enable_nestloop = off; SET max_parallel_workers_per_gather = 0; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1plan	{ SELECT iso_ad_asof() AS asof; }
step s1r	{ SELECT count(q.px) FROM iso_af f LEFT JOIN LATERAL (SELECT px FROM iso_ad d WHERE d.k = f.k AND d.ts <= f.ts ORDER BY d.ts DESC LIMIT 1) q ON true WHERE f.k = 3; }
step s1i	{ INSERT INTO iso_af VALUES (3, 1, -1); }
step s1c	{ COMMIT; }

session s2
setup		{ SET enable_nestloop = off; SET max_parallel_workers_per_gather = 0; }
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2r	{ SELECT count(q.px) FROM iso_af f LEFT JOIN LATERAL (SELECT px FROM iso_ad d WHERE d.k = f.k AND d.ts <= f.ts ORDER BY d.ts DESC LIMIT 1) q ON true WHERE f.k = 3; }
step s2i	{ INSERT INTO iso_ad VALUES (3, 2, -2); }
step s2c	{ COMMIT; }

permutation s1b s2b s1plan s1r s2r s1i s1c s2i s2c
