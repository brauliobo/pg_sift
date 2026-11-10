-- A scan that an enclosing query starts over for each of its rows (a LATERAL top-1 per entity, a correlated sub-select) finds the entries of its group in the order array by searching the group table
-- of the segment. The first probe searches it where it is; a segment probed again holds the table in memory, and the dead rows and all-visible pages of its units it checked the entries against, so the 200 probes
-- of one statement pin two buffers each, not one for every step of a search over 250 groups and the pages of the checks. The count is the extra over the same statement on an index of 2 groups
-- (a build with CATCACHE_FORCE_RELEASE reads the catalog again at every lookup: some 6 buffers a probe more than a release build, for 2 groups as for 250). The answers are those of the heap (a scan without the engine) through the same changes between probes as agg_topn_rescan: deleted and updated rows, rows in the write
-- buffer, VACUUM, a promotion in the middle of the statement, a cursor read both ways, a generic plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE rk AS SELECT i AS id, 1 + (i * 7919) % 250 AS g, timestamp '2024-01-01' + i * interval '37 seconds' AS ts, repeat('x', 100) AS pad FROM generate_series(1, 20000) i;
CREATE INDEX rk_i ON rk USING sift_col (id, g, ts) WITH (order_array_columns = 'ts', order_array_group = 'g');
CREATE TABLE rk_o AS SELECT n AS i, CASE WHEN n % 47 = 0 THEN NULL WHEN n % 41 = 0 THEN 5000 + n ELSE 1 + (n * 31) % 250 END AS g FROM generate_series(1, 200) n;
VACUUM (FREEZE, ANALYZE) rk, rk_o;
CREATE TABLE rk2 AS SELECT i AS id, 1 + (i * 7919) % 2 AS g, timestamp '2024-01-01' + i * interval '37 seconds' AS ts, repeat('x', 100) AS pad FROM generate_series(1, 20000) i;
CREATE INDEX rk2_i ON rk2 USING sift_col (id, g, ts) WITH (order_array_columns = 'ts', order_array_group = 'g');
CREATE TABLE rk2_o AS SELECT n AS i, CASE WHEN n % 47 = 0 THEN NULL WHEN n % 41 = 0 THEN 5000 + n ELSE 1 + (n * 31) % 2 END AS g FROM generate_series(1, 200) n;
VACUUM (FREEZE, ANALYZE) rk2, rk2_o;

-- the buffers the SiftAgg node of a statement pins for each of its probes (shared hit and read of the node over its loops), and whether its last probe read the global order (a NULL group goes to the units)
CREATE FUNCTION probe_buffers(q text, OUT per_probe numeric, OUT probes int, OUT global_order boolean) LANGUAGE plpgsql AS $$
DECLARE l text; seen boolean := false; blocks bigint := 0;
BEGIN
	global_order := false; probes := 0;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN seen := true; probes := substring(l FROM 'loops=([0-9]+)')::int; END IF;
		IF seen AND l ~ 'Global Order: true' THEN global_order := true; END IF;
		IF seen AND l ~ '^\s+Buffers: shared' THEN
			blocks := coalesce(substring(l FROM 'hit=([0-9]+)')::bigint, 0) + coalesce(substring(l FROM 'read=([0-9]+)')::bigint, 0);
			seen := false;
		END IF;
	END LOOP;
	per_probe := round(blocks::numeric / greatest(probes, 1), 1);
END $$;

-- the buffers a probe pins beyond those of the same statement over 2 groups (@ is rk or rk2, @_o its outer table)
CREATE FUNCTION probe_extra(q text, OUT probes int, OUT global_order boolean, OUT extra numeric) LANGUAGE sql AS $$
	SELECT a.probes, a.global_order, a.per_probe - b.per_probe FROM probe_buffers(replace(q, '@', 'rk')) a, probe_buffers(replace(q, '@', 'rk2')) b $$;

SET sift.agg_force = on;
SET enable_seqscan = off;
SET enable_sort = off;
-- warm: the first statement reads the segment
SELECT count(*) FROM rk_o v LEFT JOIN LATERAL (SELECT ts FROM rk WHERE g = v.g ORDER BY ts DESC LIMIT 1) x ON true;
SELECT count(*) FROM rk2_o v LEFT JOIN LATERAL (SELECT ts FROM rk2 WHERE g = v.g ORDER BY ts DESC LIMIT 1) x ON true;
SELECT p.probes, p.global_order, p.extra <= 1 AS few_buffers FROM probe_extra('SELECT v.i, x.ts FROM @_o v LEFT JOIN LATERAL (SELECT ts FROM @ WHERE g = v.g ORDER BY ts DESC LIMIT 1) x ON true') p;
SELECT p.probes, p.global_order, p.extra <= 1 AS few_buffers FROM probe_extra('SELECT v.i, (SELECT id FROM @ WHERE g = v.g ORDER BY ts DESC LIMIT 1) FROM @_o v') p;
SELECT p.probes, p.global_order, p.extra <= 1 AS few_buffers FROM probe_extra('SELECT v.i, x.ts FROM @_o v, LATERAL (SELECT ts FROM @ WHERE g = v.g ORDER BY ts LIMIT 3) x') p;
RESET enable_sort; RESET enable_seqscan; RESET sift.agg_force;

-- the rows are the heap's: the same statements with the engine off, before and after each change between probes
CREATE FUNCTION rk_rows(on_ bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', on_::text, true);
	PERFORM set_config('sift.agg_force', on_::text, true);
	SELECT string_agg(z::text, ',' ORDER BY z.i, z.ts) INTO r FROM (
		SELECT v.i, x.ts FROM rk_o v LEFT JOIN LATERAL (SELECT ts FROM rk WHERE g = v.g ORDER BY ts DESC LIMIT 2) x ON true
		UNION ALL SELECT v.i, (SELECT ts FROM rk WHERE g = v.g AND id % 3 = 0 ORDER BY ts LIMIT 1) FROM rk_o v
		UNION ALL SELECT v.i, (SELECT max(ts) FROM rk WHERE g = v.g) FROM rk_o v) z;
	RETURN r;
END $$;
CREATE TABLE rk_step (n int, what text, same boolean);
CREATE FUNCTION rk_check(label text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO rk_step SELECT coalesce(max(n), 0) + 1, label, rk_rows(true) = rk_rows(false) FROM rk_step;
END $$;
SELECT rk_check('as built');
DELETE FROM rk WHERE id % 13 = 0;
UPDATE rk SET ts = ts + interval '3 days' WHERE id % 17 = 0;
INSERT INTO rk SELECT 20000 + n, 1 + (n * 7) % 250, timestamp '2025-01-01' + n * interval '41 seconds', 'x' FROM generate_series(1, 500) n;
SELECT rk_check('writes in the write buffer, deleted and updated rows');
VACUUM rk;
SELECT rk_check('after VACUUM');
SELECT sift.col_promote('rk_i'::regclass) >= 0 AS promoted;
SELECT rk_check('after a promotion');
SELECT n, what, same FROM rk_step ORDER BY n;

-- a promotion and writes in the middle of the statement: the outer rows call a function that does them between two probes; the statement keeps its snapshot
CREATE FUNCTION rk_mut(i int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	IF i = 100 AND current_setting('rk.mut', true) = 'on' THEN
		DELETE FROM rk WHERE id % 11 = 0;
		INSERT INTO rk SELECT 40000 + n, 1 + (n * 3) % 250, timestamp '2026-01-01' + n * interval '43 seconds', 'x' FROM generate_series(1, 300) n;
		PERFORM sift.col_promote('rk_i'::regclass);
	END IF;
	RETURN i;
END $$;
DO $$
DECLARE before text; during text; q text := 'SELECT string_agg((v.i, x.ts)::text, '','' ORDER BY v.i, x.ts) FROM (SELECT i, g, rk_mut(i) AS m FROM rk_o OFFSET 0) v, LATERAL (SELECT ts FROM rk WHERE g = v.g ORDER BY ts DESC LIMIT 1) x';
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE q INTO before;
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	PERFORM set_config('rk.mut', 'on', true);
	EXECUTE q INTO during;
	RAISE WARNING 'writes in the statement: %', CASE WHEN during = before THEN 'the rows of the snapshot' ELSE 'DIFFERS' END;
END $$;

-- a cursor read both ways: the rows forward, then backward, are the heap's
DO $$
DECLARE r record; fwd text := ''; bwd text := ''; ref text; rk_c refcursor;
BEGIN
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	OPEN rk_c SCROLL FOR SELECT v.i, x.ts FROM rk_o v LEFT JOIN LATERAL (SELECT ts FROM rk WHERE g = v.g ORDER BY ts DESC LIMIT 1) x ON true ORDER BY v.i;
	LOOP FETCH NEXT FROM rk_c INTO r; EXIT WHEN NOT FOUND; fwd := fwd || r::text; END LOOP;
	LOOP FETCH PRIOR FROM rk_c INTO r; EXIT WHEN NOT FOUND; bwd := r::text || bwd; END LOOP;
	CLOSE rk_c;
	SET LOCAL sift.enable_agg = off;
	SELECT string_agg(z::text, '' ORDER BY z.i) INTO ref FROM (SELECT v.i, x.ts FROM rk_o v LEFT JOIN LATERAL (SELECT ts FROM rk WHERE g = v.g ORDER BY ts DESC LIMIT 1) x ON true) z;
	RAISE WARNING 'cursor: forward % the heap, backward % forward', CASE WHEN fwd = ref THEN 'is' ELSE 'DIFFERS from' END, CASE WHEN bwd = fwd THEN 'is' ELSE 'DIFFERS from' END;
END $$;

-- the same probe over a table partitioned by time, as an ordered Append of one top-1 for each partition, the newest first (what PostgreSQL's own plan does with b-trees): every arm is a probe of its
-- own, the arms of the older partitions are not executed once the newest answers, and the rows are those of the heap
CREATE TABLE rkp (id int, g int, ts timestamp, pad text) PARTITION BY RANGE (ts);
CREATE TABLE rkp_a PARTITION OF rkp FOR VALUES FROM ('2024-01-01') TO ('2024-02-01');
CREATE TABLE rkp_b PARTITION OF rkp FOR VALUES FROM ('2024-02-01') TO ('2024-03-01');
CREATE TABLE rkp_c PARTITION OF rkp FOR VALUES FROM ('2024-03-01') TO ('2024-04-01');
INSERT INTO rkp SELECT i, 1 + i % 50, timestamp '2024-01-01' + i * interval '6 minutes', repeat('x', 100) FROM generate_series(1, 20000) i;
INSERT INTO rkp SELECT 30000 + n, 100 + n, timestamp '2024-01-02' + n * interval '1 hour', 'x' FROM generate_series(1, 5) n;
CREATE INDEX rkp_i ON rkp USING sift_col (id, g, ts) WITH (order_array_columns = 'ts', order_array_group = 'g');
VACUUM (FREEZE, ANALYZE) rkp;
CREATE TABLE rkp_o AS SELECT n AS i, CASE WHEN n % 13 = 0 THEN NULL WHEN n % 11 = 0 THEN 100 + n % 7 ELSE 1 + n % 50 END AS g FROM generate_series(1, 120) n;
ANALYZE rkp_o;
CREATE FUNCTION rkp_q(par bool) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN par THEN 'SELECT v.i, x.ts FROM rkp_o v LEFT JOIN LATERAL (SELECT ts FROM rkp WHERE g = v.g ORDER BY ts DESC LIMIT 1) x ON true'
	ELSE 'SELECT v.i, x.ts FROM rkp_o v LEFT JOIN LATERAL (SELECT ts FROM (' || (SELECT string_agg('(SELECT ts FROM ' || t || ' WHERE g = v.g ORDER BY ts DESC LIMIT 1)', ' UNION ALL ' ORDER BY t DESC) FROM unnest(ARRAY['rkp_a', 'rkp_b', 'rkp_c']) t) || ') u LIMIT 1) x ON true' END $$;
CREATE FUNCTION rkp_rows(on_ bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', on_::text, true);
	PERFORM set_config('sift.agg_force', on_::text, true);
	PERFORM set_config('enable_seqscan', (NOT on_)::text, true);
	PERFORM set_config('enable_sort', (NOT on_)::text, true);
	EXECUTE 'SELECT string_agg(z::text, '','' ORDER BY z.i) FROM (' || rkp_q(NOT on_) || ') z' INTO r;
	RETURN r;
END $$;
-- arms: the top-1 scans of the plan; walked: those that read the global order; skipped: those the Append never executed (the outer rows that name a group the partitions have, all in the newest)
CREATE FUNCTION rkp_plan(OUT arms int, OUT walked int, OUT skipped int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	arms := 0; walked := 0; skipped := 0;
	PERFORM set_config('sift.agg_force', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || rkp_q(false) || ' WHERE v.g <= 50' LOOP
		arms := arms + (l ~ 'Custom Scan \(SiftAgg\)')::int;
		walked := walked + (l ~ 'Global Order: true')::int;
		skipped := skipped + (l ~ 'Custom Scan \(SiftAgg\) \(never executed\)')::int;
	END LOOP;
END $$;
SELECT * FROM rkp_plan();
SELECT rkp_rows(true) = rkp_rows(false) AS same_rows;
DELETE FROM rkp WHERE id % 7 = 0;
INSERT INTO rkp SELECT 40000 + n, 1 + n % 50, timestamp '2024-03-20' + n * interval '1 minute', 'x' FROM generate_series(1, 200) n;
SELECT rkp_rows(true) = rkp_rows(false) AS same_rows_after_writes;

DROP FUNCTION probe_buffers(text), probe_extra(text), rk_rows(bool), rk_check(text), rk_mut(int), rkp_q(bool), rkp_rows(bool), rkp_plan();
DROP TABLE rk, rk_o, rk2, rk2_o, rk_step, rkp, rkp_o;
DROP EXTENSION pg_sift;
