-- The partitions of a SiftAggV6Join that spilled are joined at the first level when their build files fit the budget: four times the rows write four times the bytes, not more.
-- The keys are 64 apart, so that no star join holds the dimension (agg/star/dense.h).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET work_mem = '1MB';

CREATE FUNCTION gs_tables(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE gs_f%s WITH (autovacuum_enabled = off) AS SELECT g::bigint AS id, g * 64::bigint AS k, ((g %% 29500) / 100.0)::numeric(10,2) AS amount '
				   'FROM generate_series(1, %s) g', n, n);
	EXECUTE format('CREATE TABLE gs_d%s WITH (autovacuum_enabled = off) AS SELECT g * 64::bigint AS id, g %% 7 AS cls FROM generate_series(1, %s) g', n, n);
	EXECUTE format('CREATE INDEX ON gs_f%s USING sift_col (id, k, amount)', n);
	EXECUTE format('ANALYZE gs_f%s, gs_d%s', n, n);
END $$;
SELECT gs_tables(32768), gs_tables(131072);

-- the bytes the join of gs_f<n> and gs_d<n> spilled
CREATE FUNCTION gs_spilled(n int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE p json;
BEGIN
	SET LOCAL sift.agg_force = on;
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, FORMAT JSON) SELECT d.cls, count(*), sum(f.amount) FROM gs_f%s f JOIN gs_d%s d ON d.id = f.k GROUP BY d.cls', n, n) INTO p;
	RETURN (SELECT (m[1])::bigint FROM regexp_matches(p::text, '"Spilled Bytes": (\d+)') m);
END $$;

SELECT s1 > 0 AS spilled, s4 <= s1 * 4.5 AS linear FROM (SELECT gs_spilled(32768) s1, gs_spilled(131072) s4) x;

SET sift.agg_force = on;
SELECT d.cls, count(*), sum(f.amount) FROM gs_f131072 f JOIN gs_d131072 d ON d.id = f.k GROUP BY d.cls ORDER BY 1;
SET sift.enable_agg = off;
SELECT d.cls, count(*), sum(f.amount) FROM gs_f131072 f JOIN gs_d131072 d ON d.id = f.k GROUP BY d.cls ORDER BY 1;
RESET sift.enable_agg;
RESET sift.agg_force;

DROP TABLE gs_f32768, gs_d32768, gs_f131072, gs_d131072;
DROP FUNCTION gs_tables(int), gs_spilled(int);
DROP EXTENSION pg_sift;
