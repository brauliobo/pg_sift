-- A joined table's uuid, interval, numeric of many digits, bytea and inet columns are read from the leaves of its index as classes of the values they take
-- (agg_lookup_dense), not from the heap row by row: a group by, a filter and a distinct count over them give the normal plan's answers, NULLs and numerics of other scales
-- included. A column with more values than a table of classes holds is read from the heap as before.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE wd (id int PRIMARY KEY, u uuid, iv interval, big numeric(30, 4), nn numeric, b bytea, ip inet, many uuid) WITH (autovacuum_enabled = false);
INSERT INTO wd SELECT g, CASE WHEN g % 11 = 0 THEN NULL ELSE md5((g % 7)::text)::uuid END, CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 5 || ' hours')::interval END,
       CASE WHEN g % 17 = 0 THEN NULL ELSE (g % 4) * 12345678901234567890.1234 END, CASE g % 3 WHEN 0 THEN 1.0 WHEN 1 THEN 1.00 ELSE 2.5 END,
       CASE WHEN g % 19 = 0 THEN NULL ELSE decode(md5((g % 3)::text), 'hex') END, CASE WHEN g % 23 = 0 THEN NULL ELSE ('10.0.0.' || g % 6)::inet END, md5(g::text)::uuid
FROM generate_series(1, 20000) g;
CREATE TABLE wf (id int, d int, v int) WITH (autovacuum_enabled = false);
INSERT INTO wf SELECT g, 1 + (g * 7) % 20000, g % 100 FROM generate_series(1, 60000) g;
SET client_min_messages = warning;
CREATE INDEX wd_col ON wd USING sift_col (id, u, iv, big, nn, b, ip, many);
CREATE INDEX wf_col ON wf USING sift_col (id, d, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) wd, wf;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT d.u, count(*), sum(f.v) FROM wf f JOIN wd d ON d.id = f.d GROUP BY 1$q$),
 (2, $q$SELECT d.iv, count(*), sum(f.v) FROM wf f JOIN wd d ON d.id = f.d GROUP BY 1$q$),
 (3, $q$SELECT d.big, count(*), sum(f.v) FROM wf f JOIN wd d ON d.id = f.d GROUP BY 1$q$),
 (4, $q$SELECT d.nn, count(*), sum(f.v) FROM wf f JOIN wd d ON d.id = f.d GROUP BY 1$q$),
 (5, $q$SELECT d.b, d.ip, count(*), sum(f.v) FROM wf f JOIN wd d ON d.id = f.d GROUP BY 1, 2$q$),
 (6, $q$SELECT d.iv, d.u, d.big, count(*) FROM wf f LEFT JOIN wd d ON d.id = f.d GROUP BY 1, 2, 3$q$),
 (7, $q$SELECT count(*), sum(f.v) FROM wf f JOIN wd d ON d.id = f.d WHERE d.iv > interval '2 hours' AND d.ip <> inet '10.0.0.1'$q$),
 (8, $q$SELECT count(DISTINCT d.u), count(DISTINCT d.iv), count(DISTINCT d.big) FROM wf f JOIN wd d ON d.id = f.d$q$),
 (9, $q$SELECT d.many, count(*) FROM wf f JOIN wd d ON d.id = f.d GROUP BY 1$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- how the scan got the lookup table: 'classed' when it made the classes, 'heap' when the rows came from the heap, else 'loaded'
CREATE FUNCTION got(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'loaded'; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '[1-9]\d* rows classed' THEN r := 'classed'; ELSIF l ~ 'Lookup Heap Reason' THEN r := 'heap'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT n, got(sql), answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY n;

DROP FUNCTION answer(text, bool), got(text);
DROP TABLE shapes, wf, wd;
DROP EXTENSION pg_sift;
