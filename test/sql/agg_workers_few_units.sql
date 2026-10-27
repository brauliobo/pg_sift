-- Workers of a scan whose joins PostgreSQL counts at a handful of rows. The lookups are a CTE of ids it has no statistics for, so it expects
-- the rows of the join few and the price of the scan with them: a few milliseconds, and the workers (3 to 4 ms to start) never pay. The path with
-- workers is priced for the rows the scan reads (the price of the scan without workers stays what the plan above is priced with), and a scan
-- of fewer rows than sift.agg_par_min_rows keeps one process when it starts, whatever the plan asked for. sift.plan_scale plans the table at
-- a size where the scan is long enough for the workers to pay.
CREATE EXTENSION pg_sift;

CREATE TABLE props (id int PRIMARY KEY, grp int, kind text);
INSERT INTO props SELECT i, i % 7, 'k' || i % 5 FROM generate_series(1, 300) i;
CREATE TABLE ev (id int PRIMARY KEY, prop_id int, day date, items jsonb) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + i % 300, '2026-01-01'::date + i % 90,
       (SELECT jsonb_agg(jsonb_build_object('kind', 'c' || ((i + k) % 11), 'n', (i * k) % 9)) FROM generate_series(1, 1 + i % 6) k)
FROM generate_series(1, 12000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, prop_id, day, items) WITH (paths = 'items[]{kind text, n int4}');
VACUUM ANALYZE ev;
ANALYZE props;

SET max_parallel_workers_per_gather = 2;
SET sift.agg_preagg = off;

CREATE TABLE w_q (q text);
INSERT INTO w_q VALUES ($q$
WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM props WHERE kind = ANY (string_to_array('k1,k2,k3,k4,k0', ',')))
SELECT p.grp, i.kind, count(*) AS n, sum(i.n) AS s, count(DISTINCT e.id) AS d
FROM ev e JOIN ids ON ids.id = e.prop_id JOIN props p ON p.id = ids.id
CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int)
WHERE e.day >= '2026-01-10' GROUP BY 1, 2
$q$);

-- the workers the plan asks for, and the most that a run starts (a launch can fail when other sessions hold the workers: tried again)
CREATE FUNCTION w_planned() RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || (SELECT q FROM w_q) LOOP
		IF l ~ 'Workers Planned: [0-9]' THEN n := substring(l FROM 'Workers Planned: ([0-9]+)')::int; END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION w_launched() RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR i IN 1..20 LOOP
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || (SELECT q FROM w_q) LOOP
			IF l ~ 'Workers Launched: [0-9]' THEN n := greatest(n, substring(l FROM 'Workers Launched: ([0-9]+)')::int); END IF;
		END LOOP;
		EXIT WHEN n > 0 OR (SELECT current_setting('sift.agg_par_min_rows')::int) > 0;
		PERFORM pg_sleep(0.1);
	END LOOP;
	RETURN n;
END $$;

-- the size at which the plan asks for workers priced for the rows the scan reads (the scale is a diagnostic: the table is planned that many times larger)
CREATE FUNCTION w_priced_for_rows() RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR s IN 1..60 LOOP
		PERFORM set_config('sift.plan_scale', s::text, true);
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || (SELECT q FROM w_q) LOOP
			IF l ~ 'Workers Priced For' THEN RETURN s; END IF;
		END LOOP;
	END LOOP;
	RETURN 0;
END $$;

SELECT w_planned() AS planned_as_is;
SELECT w_priced_for_rows() > 1 AS some_scale_prices_workers_for_rows;
SELECT set_config('sift.plan_scale', w_priced_for_rows()::text, false) IS NOT NULL AS scale_set;
SELECT w_planned() AS planned_at_scale;

-- 12,000 rows: the leader scans alone; with the threshold off the workers start
SELECT w_launched() AS launched_default;
SET sift.agg_par_min_rows = 0;
SELECT w_launched() AS launched_row_floor_off;

-- the rows are those of the normal plan
RESET sift.plan_scale;
CREATE FUNCTION w_rows(push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', (SELECT q FROM w_q)) INTO r;
	RETURN r;
END $$;
SELECT w_rows(true) = w_rows(false) AS same_rows;

-- Units cut by a key: an index built by workers has a segment for each, over the same superblocks, so the units its statistics count (the superblocks
-- they span) are a fraction of the units it holds, and the average unit they give is several times the real one. The rows of the kept units decide,
-- not their number times that average: here 4,500 rows against a floor of 6,000, where the kept units times the average unit are 9,000 or more.
CREATE TABLE ev_k WITH (fillfactor = 10, autovacuum_enabled = false) AS SELECT * FROM ev ORDER BY prop_id, id;
ALTER TABLE ev_k SET (parallel_workers = 4);
SET max_parallel_maintenance_workers = 4;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
CREATE INDEX ev_k_col ON ev_k USING sift_col (id, prop_id, day, items) WITH (paths = 'items[]{kind text, n int4}', key_cut = prop_id);
RESET max_parallel_maintenance_workers;
RESET min_parallel_table_scan_size;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
VACUUM ANALYZE ev_k;
SELECT segments > 1 AS built_by_workers FROM sift.col_index_info('ev_k_col');
SET sift.agg_par_min_rows = 6000;

CREATE TABLE k_q (name text, q text);
INSERT INTO k_q SELECT name, format($q$
WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM props WHERE kind = ANY (string_to_array('k1,k2,k3,k4,k0', ',')) AND id %% %s = 0)
SELECT p.grp, i.kind, count(*) AS n, sum(i.n) AS s
FROM ev_k e JOIN ids ON ids.id = e.prop_id JOIN props p ON p.id = ids.id
CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int)
WHERE e.day >= '2026-01-10' GROUP BY 1, 2
$q$, m) FROM (VALUES ('small', 50), ('large', 1)) t(name, m);

CREATE FUNCTION k_launched(qname text, retry bool) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR i IN 1..20 LOOP
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || (SELECT q FROM k_q WHERE name = qname) LOOP
			IF l ~ 'Workers Launched: [0-9]' THEN n := greatest(n, substring(l FROM 'Workers Launched: ([0-9]+)')::int); END IF;
		END LOOP;
		EXIT WHEN n > 0 OR NOT retry;
		PERFORM pg_sleep(0.1);
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION k_priced(qname text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || (SELECT q FROM k_q WHERE name = qname) LOOP
		IF l ~ 'Workers Priced For' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- the scale at which the plan prices the workers for the rows the scan reads
CREATE FUNCTION k_scale() RETURNS int LANGUAGE plpgsql AS $$
DECLARE s int;
BEGIN
	FOREACH s IN ARRAY ARRAY[1, 10, 30, 60, 100, 200, 400, 1000] LOOP
		PERFORM set_config('sift.plan_scale', s::text, true);
		IF k_priced('small') AND k_priced('large') THEN RETURN s; END IF;
	END LOOP;
	RETURN 0;
END $$;
SELECT set_config('sift.plan_scale', k_scale()::text, false) <> '0' AS scale_set;
SELECT k_priced('small') AS small_priced_for_rows, k_priced('large') AS large_priced_for_rows;
-- the small scan keeps one process, the large one (about 12,000 rows) starts its workers
SELECT k_launched('small', false) AS launched_small;
SELECT k_launched('large', true) > 0 AS launched_large;
-- a floor under its rows lets the small one start them
SET sift.agg_par_min_rows = 3000;
SELECT k_launched('small', true) > 0 AS launched_small_floor_under_rows;

-- the rows are those of the normal plan
CREATE FUNCTION k_rows(qname text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', (SELECT q FROM k_q WHERE name = qname)) INTO r;
	RETURN r;
END $$;
SELECT k_rows('small', true) = k_rows('small', false) AS same_rows_small, k_rows('large', true) = k_rows('large', false) AS same_rows_large;
DROP FUNCTION k_launched(text, bool), k_priced(text), k_scale(), k_rows(text, bool);
DROP TABLE k_q, ev_k;

DROP FUNCTION w_planned(), w_launched(), w_priced_for_rows(), w_rows(bool);
DROP TABLE w_q, ev, props;
DROP EXTENSION pg_sift;
