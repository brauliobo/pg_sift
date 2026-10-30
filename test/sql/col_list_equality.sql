-- An IN list of the keys of a scope and an equality on one of them (the planner keeps both) are one test of the constants both hold: the scan tests a unit and a
-- buffered row against that one constant, and prices it so. The answers are the heap's, with the equality in the list and out of it.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
CREATE TABLE ev (id int PRIMARY KEY, k int NOT NULL, ts date NOT NULL, note text, pad text DEFAULT repeat('x', 100));
INSERT INTO ev (id, k, ts, note) SELECT g, 1 + (g * 7919) % 2000, date '2025-01-01' + g % 300, CASE WHEN g % 5 = 0 THEN NULL ELSE 'n' || g % 50 END FROM generate_series(1, 100000) g;
CREATE INDEX ev_i ON ev USING sift_col (id, k, ts, note) WITH (key_cut = 'k');
VACUUM (FREEZE, ANALYZE) ev;
-- rows that are only in the write buffer
INSERT INTO ev (id, k, ts, note) SELECT g, 1 + (g * 7919) % 2000, date '2025-01-01' + g % 300, 'n' || g % 50 FROM generate_series(100001, 110000) g;

CREATE FUNCTION scope_list(n int) RETURNS text LANGUAGE sql AS $$ SELECT string_agg((1 + g * 6)::text, ',') FROM generate_series(0, n - 1) g $$;

-- the rows of one shape by the column index's bitmap scan and by the heap
CREATE FUNCTION rows_of(w text, scan boolean) RETURNS bigint[] LANGUAGE plpgsql AS $$
DECLARE r bigint[];
BEGIN
	IF scan THEN
		SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
	ELSE
		SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	END IF;
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM ev WHERE %s', w) INTO r;
	RETURN r;
END $$;

-- the blocks of the bitmap index scan, and the estimated cost of the plan
CREATE FUNCTION index_blocks(w text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int;
BEGIN
	SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM ev WHERE %s', w) LOOP
		IF l ~ 'Bitmap Index Scan on ev_i' THEN n := 0; END IF;
		IF n IS NOT NULL AND l ~ '^ +Buffers: shared' THEN n := coalesce(substring(l from 'hit=(\d+)')::int, 0) + coalesce(substring(l from 'read=(\d+)')::int, 0); EXIT; END IF;
	END LOOP;
	RETURN n;
END $$;
CREATE FUNCTION plan_cost(w text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE l text; c float8;
BEGIN
	SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
	FOR l IN EXECUTE format('EXPLAIN SELECT id FROM ev WHERE %s', w) LOOP
		IF l ~ 'Bitmap Index Scan on ev_i' THEN c := substring(l from '\.\.([0-9.]+) rows')::float8; END IF;
	END LOOP;
	RETURN c;
END $$;

CREATE TABLE ev_q (n serial, w text);
SELECT format($f$INSERT INTO ev_q (w) VALUES ('k = ANY (''{%1$s}'') AND k = 7'), ('k = ANY (''{%1$s}'') AND k = 8'), ('k = ANY (''{%1$s}'') AND k = 601 AND ts > ''2025-06-01'''),
 ('k = ANY (''{%1$s}'') AND k IN (7, 13, 8, 19, 1000)'), ('k = ANY (''{%1$s}'') AND note IS NOT NULL AND k = 1201'), ('k = 13 AND k = ANY (''{%1$s}'')'),
 ('k = ANY (''{%1$s}'') AND k = ANY (''{%2$s}'')')$f$, scope_list(300), scope_list(1000)) \gexec
SELECT count(*) AS shapes FROM ev_q;
SELECT n, w FROM ev_q WHERE rows_of(w, true) <> rows_of(w, false) ORDER BY n;
SELECT n, cardinality(rows_of(w, false)) > 0 AS has_rows FROM ev_q ORDER BY n;

-- the equality is out of the list: nothing can match, and the scan reads no unit
SELECT index_blocks(format('k = ANY (''{%s}'') AND k = 8', scope_list(300))) <= 8 AS unsatisfiable_reads_nothing;
-- the list costs no more than its equality
SELECT plan_cost(format('k = ANY (''{%s}'') AND k = 7', scope_list(300))) <= 1.05 * plan_cost('k = 7') AS list_adds_no_cost;

DROP TABLE ev, ev_q CASCADE;
DROP FUNCTION scope_list(int), rows_of(text, boolean), index_blocks(text), plan_cost(text);
DROP EXTENSION pg_sift;
