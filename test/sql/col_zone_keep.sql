-- A filter that is an OR of ranges is a zone tree: the units it keeps are found once for the scan, as for the other filters (the begin of every morsel of two units
-- tested its units again, so its cost grew with the units of the index and not with the units it kept). The rows are those of the normal plan, and the units read and
-- pruned are the same as before the tree joined the cache. A page holds one row, so unit n holds the rows 256 * n .. 256 * n + 255 (k = n, except the stray rows, which
-- keep their unit alive for an OR arm on their k); sm_k is cut by the key (a unit holds a stretch of k from the whole table).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE sm (id int, k int, f float8, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO sm SELECT i, CASE WHEN i % 50 = 7 THEN ((i / 50) * 37) % 100 ELSE i / 256 END, i % 13, repeat('x', 600) FROM generate_series(0, 100 * 256 - 1) i;
CREATE TABLE sm_o (LIKE sm) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO sm_o SELECT id, id / 256, f, pad FROM sm;
CREATE TABLE sm_k (LIKE sm) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO sm_k SELECT * FROM sm ORDER BY (id * 7919) % 25600;
CREATE INDEX sm_idx ON sm USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops, f sift.col_float8_ops);
CREATE INDEX sm_o_idx ON sm_o USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops, f sift.col_float8_ops);
CREATE INDEX sm_k_idx ON sm_k USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops, f sift.col_float8_ops) WITH (key_cut = 'k', key_cut_rows = 256);
VACUUM (FREEZE, ANALYZE) sm, sm_o, sm_k;

CREATE FUNCTION units_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; read text; pruned text := '0';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l ~ 'Units: (\d+)' THEN read := substring(l from 'Units: (\d+)'); END IF;
		IF l ~ '(\d+) pruned' THEN pruned := substring(l from '(\d+) pruned'); END IF;
	END LOOP;
	RETURN CASE WHEN r = 'normal' THEN r ELSE format('%s read %s pruned %s', r, read, pruned) END;
END $$;

CREATE FUNCTION same(tbl text, agg text, cond text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT k %% 3 AS g, count(*) AS n, %s AS a FROM %s WHERE %s GROUP BY 1', agg, tbl, cond);
BEGIN
	plan := units_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((SELECT * FROM r_on EXCEPT ALL SELECT * FROM r_off) UNION ALL (SELECT * FROM r_off EXCEPT ALL SELECT * FROM r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE sm_q (n serial, cond text);
INSERT INTO sm_q (cond) VALUES ('k = 40 OR k = 41'), ('k BETWEEN 10 AND 12 OR k >= 98'), ('id < 300 OR id BETWEEN 20000 AND 20100'), ('(k = 40 OR k = 70) AND id > 10000'),
	('k < 0 OR k > 500'), ('k = 40 OR f = 3'), ('k BETWEEN 0 AND 99 OR id < 1'), ('(k = 3 OR k = 4) AND (id < 2000 OR id > 24000)'), ('k = 99 OR (k = 50 AND f = 7)'),
	('k = 40'), ('k = 99 AND id > 25000'), ('k BETWEEN 98 AND 99 AND f = 3');

-- sum(id) is exact in any order (items of two superblocks), sum of a float keeps its order (items of one); the table in order (sm_o), with stray rows (sm: a unit
-- the OR keeps for one arm only), and cut by the key (sm_k). The last three conditions keep a few units of a hundred (or none): the morsels without a kept
-- unit are skipped, where the stray rows of sm spread the key over units that are not in its order

SELECT t, a, n, cond, s.* FROM (VALUES ('sm_o'), ('sm'), ('sm_k')) tt(t), (VALUES ('sum(id)'), ('avg(f)')) aa(a), sm_q, LATERAL same(t, a, cond) s ORDER BY t DESC, a, n;

DROP FUNCTION same(text, text, text), units_of(text);
DROP TABLE sm_q, sm, sm_o, sm_k;
DROP EXTENSION pg_sift;
