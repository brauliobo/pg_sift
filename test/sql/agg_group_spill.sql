-- The group table of a scan that outgrows hash memory goes to partitions on disk as partial states, and each partition is merged and returned by
-- itself: the answer is the normal plan's, for every kind of key, with the float sums within 1e-9 of it. work_mem 64kB with hash_mem_multiplier 1 puts
-- the limit at the smallest the settings allow, so that 100,000 rows fill the table several times and a partition is partitioned again.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '60s';
SET client_min_messages = warning;

CREATE TABLE events (id int, k1 int, k2 text, k3 float8, val int, w float8, n numeric(12,2), ts date);
INSERT INTO events
SELECT i, CASE WHEN i % 20 = 0 THEN NULL ELSE (i * 7919) % 50000 END, 'k' || (i * 31) % 3000, ((i * 13) % 700) * 0.5,
       CASE WHEN i % 11 = 0 THEN NULL ELSE i % 1000 END, (i % 977) * 1.25, (i % 313) / 4.0, date '2024-01-01' + i % 400
FROM generate_series(1, 100000) i;
CREATE INDEX events_col ON events USING sift_col (id, k1, k2, k3, val, w, n, ts);
VACUUM (FREEZE, ANALYZE) events;

-- the rows of two tables, the keys (columns k*) equal and the measures (columns m*) equal or within 1e-9 of each other
CREATE FUNCTION same_rows(a regclass, b regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE ok boolean;
BEGIN
	EXECUTE format($q$
		WITH ka AS (SELECT j, (SELECT coalesce(jsonb_object_agg(key, value), '{}') FROM jsonb_each(j) WHERE key LIKE 'k%%') k FROM (SELECT to_jsonb(x) j FROM %s x) s),
		     kb AS (SELECT j, (SELECT coalesce(jsonb_object_agg(key, value), '{}') FROM jsonb_each(j) WHERE key LIKE 'k%%') k FROM (SELECT to_jsonb(y) j FROM %s y) s)
		SELECT NOT EXISTS (SELECT 1 FROM ka FULL JOIN kb USING (k)
			WHERE ka.j IS NULL OR kb.j IS NULL OR EXISTS (
				SELECT 1 FROM jsonb_each(ka.j) e LEFT JOIN jsonb_each(kb.j) f USING (key)
				WHERE e.key LIKE 'm%%' AND NOT (e.value = f.value OR (jsonb_typeof(e.value) = 'number' AND jsonb_typeof(f.value) = 'number'
					AND abs((e.value)::text::numeric - (f.value)::text::numeric) <= 1e-9 * greatest(1, abs((e.value)::text::numeric), abs((f.value)::text::numeric))))))
		$q$, a, b) INTO ok;
	RETURN ok;
END $$;

-- one query forced to the scan (the sort and the hash of the normal plan off) with the memory given, then as the normal plan: does it spill, are the rows the same
CREATE FUNCTION spill_case(kcols text, keys text, mem text DEFAULT '64kB') RETURNS TABLE (shape text, spilled boolean, same boolean) LANGUAGE plpgsql AS $$
DECLARE q text; l text; sp boolean := false;
BEGIN
	q := format('SELECT %s, count(*) AS m1, count(val) AS m2, sum(val) AS m3, avg(val) AS m4, min(val) AS m5, max(val) AS m6, sum(w) AS m7, avg(w) AS m8, '
				'min(w) AS m9, max(w) AS m10, sum(n) AS m11, avg(n) AS m12 FROM events GROUP BY %s', kcols, keys);
	DROP TABLE IF EXISTS got;
	DROP TABLE IF EXISTS want;
	PERFORM set_config('work_mem', mem, false), set_config('hash_mem_multiplier', '1', false), set_config('enable_hashagg', 'off', false), set_config('enable_sort', 'off', false);
	EXECUTE 'CREATE TABLE got AS ' || q;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		sp := sp OR l ~ 'Group Table Spills: [1-9]';
	END LOOP;
	RESET work_mem; RESET hash_mem_multiplier; RESET enable_hashagg; RESET enable_sort;
	SET sift.enable_agg = off;
	EXECUTE 'CREATE TABLE want AS ' || q;
	RESET sift.enable_agg;
	RETURN QUERY SELECT keys, sp, same_rows('got', 'want');
END $$;

SELECT * FROM spill_case('k1 AS k1', 'k1');
SELECT * FROM spill_case('k2 AS k2', 'k2');
SELECT * FROM spill_case('k1 AS k1, k2 AS k2', 'k1, k2');
SELECT * FROM spill_case('k3 AS k3', 'k3');
SELECT * FROM spill_case('k1 % 7 AS k1, ts AS k2', 'k1 % 7, ts');
-- more memory, fewer spills; the answer is the same
SELECT * FROM spill_case('k1 AS k1', 'k1', '1MB');

-- an aggregate that cannot spill keeps the scan until its table is far past hash memory (agg_group_spill_limits); one that can is planned without workers, whose tables would each be a table of the process
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT arm, used, slug FROM sift.explain_why('SELECT k1, count(DISTINCT val) FROM events GROUP BY k1');
CREATE FUNCTION plan_has(q text, pat text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	RETURN plan ~ pat;
END $$;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET enable_hashagg = off;
SET enable_sort = off;
SELECT plan_has('SELECT k2, count(*), sum(val) FROM events GROUP BY k2', 'SiftAgg') AS scan,
       plan_has('SELECT k2, count(*), sum(val) FROM events GROUP BY k2', 'Workers Planned') AS workers_without_room;
SET work_mem = '64MB';
SELECT plan_has('SELECT k2, count(*), sum(val) FROM events GROUP BY k2', 'Workers Planned') AS workers_with_room;
RESET ALL;
SET client_min_messages = warning;

-- the node is scanned again, whole (the inner side of a nested loop) and after a few of its groups (a LIMIT above it): the groups of the partitions come back
CREATE FUNCTION rescan_case(forced boolean) RETURNS TABLE (g int, n bigint, total numeric, groups_seen bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('work_mem', '64kB', true), set_config('hash_mem_multiplier', '1', true), set_config('enable_material', 'off', true),
			set_config('enable_hashjoin', 'off', true), set_config('enable_mergejoin', 'off', true);
	IF forced THEN
		PERFORM set_config('enable_hashagg', 'off', true), set_config('enable_sort', 'off', true);
	ELSE
		PERFORM set_config('sift.enable_agg', 'off', true);
	END IF;
	RETURN QUERY SELECT x, count(*), round(sum(s.m)::numeric, 2),
		(SELECT count(*) FROM (SELECT k1, sum(w) FROM events GROUP BY k1 LIMIT 7) l)
	FROM generate_series(1, 3) x LEFT JOIN (SELECT k1, sum(w) AS m FROM events GROUP BY k1) s ON true GROUP BY x ORDER BY x;
END $$;
SELECT * FROM rescan_case(true);
SELECT * FROM rescan_case(false);
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET enable_hashagg = off;
SET enable_sort = off;
SET enable_material = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT plan_has('SELECT x, count(*) FROM generate_series(1, 3) x LEFT JOIN (SELECT k1, sum(w) AS m FROM events GROUP BY k1) s ON true GROUP BY x',
                'Nested Loop Left Join(.|\n)*SiftAgg') AS scan_is_the_inner;
RESET ALL;
SET client_min_messages = warning;

-- the comparison fails on a wrong row, a missing row and a measure off by more than 1e-9
CREATE TABLE wrong AS SELECT * FROM want;
SELECT same_rows('got', 'wrong') AS control_equal;
UPDATE wrong SET m7 = m7 * 1.000001 WHERE ctid = (SELECT min(ctid) FROM wrong);
SELECT same_rows('got', 'wrong') AS control_measure;
DELETE FROM wrong WHERE ctid = (SELECT min(ctid) FROM wrong);
SELECT same_rows('got', 'wrong') AS control_missing;

DROP TABLE wrong, got, want;
DROP FUNCTION spill_case(text, text, text), same_rows(regclass, regclass), plan_has(text, text), rescan_case(boolean);
DROP TABLE events;
DROP EXTENSION pg_sift;
