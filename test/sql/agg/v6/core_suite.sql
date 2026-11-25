-- The 6.0 aggregate engine. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;


-- >>> agg_v6_explain
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- the compact lines of EXPLAIN (ANALYZE) of the Format 2.0 engine: four scans in text, the pruning one in JSON, and the zero counters of VERBOSE
SELECT sift._test_explain_v6(s, 'text') FROM generate_series(1, 4) s;
SELECT sift._test_explain_v6(4, 'json');
SELECT sift._test_explain_v6(2, 'text', true);
SELECT s, array_length(string_to_array(trim(trailing E'\n' from sift._test_explain_v6(s, 'text')), E'\n'), 1) <= 6 AS at_most_six_lines FROM generate_series(1, 4) s;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_explain

-- >>> agg_v6_group
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- Group table beyond 16 byte keys (src/agg/ht_test.c): the growth threshold at a load of 7/8 (written out in the test, so a
-- changed constant fails it), wide keys and string views with state blocks of sums, minima, variance and a distinct set, and the
-- distinct set alone, of 64-bit values and of the bytes of wide ones (hashes forced to collide, merged, shipped as records). Each is the number of failed cases.

SELECT sift._test_ht_grow() AS grow_failures;
SELECT sift._test_ht_ext(3, 25) AS ext_failures;
SELECT sift._test_dist_set(9, 60) AS dist_failures;
SELECT sift._test_dist_set(1234, 60) AS dist_failures;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_group

-- >>> agg_v6_morsel
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
-- Guided work queue of the parallel scans (src/core/par/morsel.c): the leader and 0, 1, 2, 4 and 8 workers take every granule of
-- parts whose sizes are as skewed as a Zipf law (half of the granules in one part, a quarter in the next, some parts empty)
-- from one cursor. Every granule is visited exactly once, every claim lies in one part and has the guided length (the part is
-- shared by all the participants, not given to one), and the claim that ends the list is one granule.
SET max_parallel_workers = 16;
SET max_parallel_workers_per_gather = 16;

SELECT w AS workers, p AS parts, r.launched = w AS launched_all, r.total, r.wrong AS granules_not_once, r.bad_claims,
       r.max_run, r.tail_run, r.claims > 0 AS claimed
FROM (VALUES (0, 7), (1, 7), (2, 7), (4, 7), (8, 7), (4, 1), (8, 16), (8, 3)) AS v(w, p),
     LATERAL sift._test_morsel_par(5, w, p, 6000) AS r
ORDER BY w, p;

-- more participants than granules, and a list that ends inside the first claim
SELECT w AS workers, g AS granules, r.launched = w AS launched_all, r.total, r.wrong AS granules_not_once, r.bad_claims, r.tail_run
FROM (VALUES (0, 1), (2, 1), (8, 5), (8, 33), (4, 64)) AS v(w, g),
     LATERAL sift._test_morsel_par(9, w, 2, g) AS r
ORDER BY w, g;

-- the claim shrinks as the list is consumed: one participant takes the peak run first and one granule last
SELECT r.max_run, r.tail_run, r.claims > r.total / 8 AS shrank
FROM sift._test_morsel_par(3, 0, 4, 20000) AS r;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_morsel

-- >>> agg_v6_parallel_2stage_plan
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
-- The plans of the parallel SiftAggV6 (Spec 44): a Gather over a SiftAggV6 that names its strategy, the strategy the shape of each query asks for, and the settings that keep a query serial.
-- Needs the extension (the first query is the proof that the sift path is taken); expected output is not generated here: run it in the integration tree, REGEN there.
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
CREATE TABLE pe AS
SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE (i * 7919) % 50021 END AS g, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 31) % 1000 END AS v,
       (CASE WHEN i % 11 = 0 THEN NULL ELSE ((i * 104729) % 100000)::numeric / 8 END)::numeric(12,3) AS n, (i * 40503) % 977 AS d,
       CASE WHEN i % 19 = 0 THEN NULL ELSE 'k' || (i * 7 % 311) END AS t, ((i * 40503) % 1009)::float8 / 16 AS f
FROM generate_series(1::int8, 60000::int8) AS i;
CREATE INDEX pe_col ON pe USING sift_col (g, v, n, d, t, f);
ANALYZE pe;
CREATE FUNCTION plan_of(q text, settings text DEFAULT '') RETURNS jsonb LANGUAGE plpgsql AS $f$
DECLARE j jsonb;
BEGIN
	EXECUTE settings;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
	RETURN j;
END $f$;
CREATE FUNCTION undo(n text, v text) RETURNS boolean LANGUAGE plpgsql AS $f$ BEGIN PERFORM set_config(n, v, false); RETURN true; END $f$;
-- every shape of agg_v6_parallel_2stage.sql: a Gather above the node, the strategy the node names, and that the workers stole morsels
SELECT shape, plan::text LIKE '%"Node Type": "Gather"%' AS gathers, plan::text LIKE '%"Custom Plan Provider": "SiftAggV6"%' AS sift_node,
       substring(plan::text from '"Parallel Strategy": "([^"]*)"') AS strategy, plan::text LIKE '%"Morsels Stolen"%' AS morsels
FROM (VALUES ('q4', 'SELECT g, count(*) AS c, sum(v) AS s, min(v) AS mn, max(n) AS mx, avg(n) AS a FROM pe GROUP BY g'),
             ('q5', 'SELECT count(DISTINCT d) AS dd, count(DISTINCT t) AS dt, count(DISTINCT g) AS dg FROM pe'),
             ('low', 'SELECT t, count(*) AS c, sum(n) AS s, min(d) AS mn, max(d) AS mx, avg(v) AS a FROM pe GROUP BY t'),
             ('grouped_distinct', 'SELECT t, count(DISTINCT g) AS dg, count(DISTINCT d) AS dd, count(*) AS c FROM pe GROUP BY t'),
             ('ungrouped', 'SELECT count(*) AS c, count(v) AS cv, sum(n) AS s, avg(v) AS a, min(n) AS mn, max(n) AS mx FROM pe'),
             ('sets', 'SELECT t, v, GROUPING(t, v) AS gr, count(*) AS c, sum(d) AS s FROM pe GROUP BY GROUPING SETS ((t), (v), (t, v), ())'),
             ('having', 'SELECT g, count(*) AS c FROM pe GROUP BY g HAVING count(*) > 1'),
             ('filter', 'SELECT t, count(*) FILTER (WHERE d < 100) AS lo, sum(v) FILTER (WHERE d >= 100) AS hi FROM pe GROUP BY t'),
             ('float', 'SELECT t, sum(f) AS s, avg(f) AS a FROM pe GROUP BY t')) AS v(shape, q), LATERAL plan_of(q) AS plan;
-- what keeps a query serial: the session, a float sum under exact_float, no worker allowed; the baseline shows the same query gathers
SELECT s AS setting, plan_of('SELECT t, sum(f) FROM pe GROUP BY t', format('SET %s = %s', n, v))::text LIKE '%"Node Type": "Gather"%' AS gathers, undo(n, u)
FROM (VALUES ('the baseline', 'max_parallel_workers_per_gather', '4', '4'), ('sift.parallel_scan = off', 'sift.parallel_scan', 'off', 'on'),
             ('max_parallel_workers_per_gather = 0', 'max_parallel_workers_per_gather', '0', '4'), ('sift.agg_exact_float = on', 'sift.agg_exact_float', 'on', 'off')) AS v(s, n, v, u);
DROP FUNCTION undo(text, text);
DROP FUNCTION plan_of(text, text);
DROP TABLE pe;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_parallel_2stage_plan

-- >>> agg_v6_text_intake
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
-- SiftAggV6 takes the hits of a BM25 `sift` index as a TID bridge: the aggregate of a text qual and structured filters is read from the sift_col index in chunks,
-- and the groups equal those of the standard plan (sift.enable_agg = off), also after deletes, updates and a transaction's own changes
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET sift.enable_scan = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ti (id int PRIMARY KEY, tenant int NOT NULL, region int NOT NULL, amount int NOT NULL, body text NOT NULL);
INSERT INTO ti SELECT i, i % 10000, i % 7, i % 100,
	concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, CASE WHEN i % 20 = 0 THEN 'mid' END, CASE WHEN i % 20000 = 0 THEN 'rareterm' END, 'tok' || i % 13, 'base')
FROM generate_series(1, 200000) i;
CREATE INDEX ti_bm ON ti USING sift (body);
CREATE INDEX ti_col ON ti USING sift_col (tenant, region, amount);
VACUUM (ANALYZE) ti;

CREATE FUNCTION ti_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'SiftAggV6|Search' THEN r := r || btrim(l) || '; '; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE TABLE ti_q (name text PRIMARY KEY, q text);
INSERT INTO ti_q VALUES
	('group',	'SELECT region, count(*) AS n, sum(amount) AS s FROM ti WHERE body @@> ''mid'' AND amount >= 0 GROUP BY region'),
	('filter',	'SELECT region, count(*) AS n, min(tenant) AS lo, max(amount) AS hi FROM ti WHERE amount >= 20 AND tenant < 5000 AND body @@> ''mid'' GROUP BY region'),
	('count',	'SELECT count(*) AS n FROM ti WHERE region < 5 AND body @@> ''mid tok3'''),
	('rare',	'SELECT region, count(*) AS n FROM ti WHERE body @@> ''rareterm'' AND amount >= 0 GROUP BY region'),
	('none',	'SELECT region, count(*) AS n FROM ti WHERE body @@> ''absent'' AND amount >= 0 GROUP BY region');

CREATE FUNCTION ti_check(q text, OUT v6 bigint, OUT heap_only bigint, OUT v6_only bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE ti_a ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE ti_b ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO v6 FROM ti_a;
	SELECT count(*) INTO heap_only FROM (SELECT * FROM ti_b EXCEPT SELECT * FROM ti_a) d;
	SELECT count(*) INTO v6_only FROM (SELECT * FROM ti_a EXCEPT SELECT * FROM ti_b) d;
	DROP TABLE ti_a, ti_b;
END $$;

-- the plan reads the text index through the bridge
SELECT name, ti_plan(q) FROM ti_q WHERE name IN ('group', 'filter', 'count') ORDER BY name;
SELECT name, (ti_check(q)).* FROM ti_q ORDER BY name;

-- rows deleted, updated into and out of the text, and a transaction's own changes
DELETE FROM ti WHERE id % 7 = 0;
UPDATE ti SET body = body || ' mid' WHERE id % 13 = 0;
UPDATE ti SET body = 'base' WHERE id % 11 = 0;
SELECT name, (ti_check(q)).* FROM ti_q ORDER BY name;
BEGIN;
INSERT INTO ti SELECT i, 3, 4, 15, 'mid own' FROM generate_series(300001, 300050) i;
DELETE FROM ti WHERE tenant < 1000;
SELECT name, (ti_check(q)).* FROM ti_q WHERE name IN ('group', 'filter', 'count') ORDER BY name;
ROLLBACK;

-- a small work_mem cuts the hits into several chunks of the same result
SET work_mem = '64kB';
SELECT name, (ti_check(q)).* FROM ti_q WHERE name IN ('group', 'filter') ORDER BY name;
RESET work_mem;

DROP TABLE ti_q, ti;
DROP FUNCTION ti_plan(text), ti_check(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_text_intake

-- >>> agg_v6_units
CREATE SCHEMA s7;
GRANT USAGE ON SCHEMA s7 TO PUBLIC;
SET search_path = s7, public;
-- 2048-row engine units (src/agg/v6/core_test.c): the salted hash table with its growth and its radix scatter and merge,
-- and the five-input cost of a SiftAgg path at the default planner costs.

SELECT sift._test_salted_ht(11, 60) AS ht_failures;

SET seq_page_cost = 1;
SET cpu_operator_cost = 0.0025;
SET cpu_tuple_cost = 0.01;
SET parallel_setup_cost = 1000;
SET parallel_tuple_cost = 0.1;

-- (g_eff, b_dec, n_groups, f_join, selectivity, is_preagg, is_streaming, max_workers): serial, parallel, answered from the
-- granule pre-aggregates, hash table past the last-level cache, sorted streaming, and too few granules to launch a worker
SELECT workers, round(startup::numeric, 4) AS startup, round(total::numeric, 4) AS total
FROM (VALUES (1000, 8e7, 100, 1, 1.0, false, false, 0),
             (1000, 8e7, 100, 1, 1.0, false, false, 4),
             (1000, 8e7, 100, 1, 1.0, true, false, 4),
             (1000, 8e7, 1e6, 1, 0.5, false, false, 0),
             (1000, 8e7, 100, 1, 1.0, false, true, 0),
             (4, 8e5, 100, 1, 1.0, false, false, 4)) AS v(g, b, n, f, s, p, st, w),
     LATERAL sift._test_agg_cost(g, b, n, f, s, p, st, w);

-- the setting of the bit-exact float8 sum: off, and settable
SHOW sift.agg_exact_float;
SET sift.agg_exact_float = on;
SHOW sift.agg_exact_float;
RESET sift.agg_exact_float;
SHOW sift.agg_exact_float;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s7 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_units

-- >>> agg_v6_vacuum_reused_tid
CREATE SCHEMA s8;
GRANT USAGE ON SCHEMA s8 TO PUBLIC;
SET search_path = s8, public;
-- VACUUM frees the heap addresses of deleted rows and an INSERT reuses them: the index then holds a dead row and a live one at one address. The row a later VACUUM names dead is the live one,
-- so the count of the node, of the index-only scan and of the heap agree after INSERT, UPDATE and VACUUM, in every order of them.
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE rt (id int PRIMARY KEY, g int, v int) WITH (autovacuum_enabled = false);
INSERT INTO rt SELECT i, i % 5, i FROM generate_series(1, 30000) i;
CREATE INDEX rt_col ON rt USING sift_col (g, v, id);

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l; END LOOP;
	RETURN p;
END $$;

CREATE FUNCTION groups_md5() RETURNS text LANGUAGE sql AS $$ SELECT md5(string_agg(r::text, '|' ORDER BY r::text)) FROM (SELECT g, count(*), sum(v) FROM rt GROUP BY g) r $$;

CREATE FUNCTION three_counts() RETURNS TABLE (node bigint, ios bigint, heap bigint, node_plan bool, ios_plan bool, groups_same bool) LANGUAGE plpgsql AS $$
DECLARE n_md5 text; i_md5 text; h_md5 text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	node_plan := plan_of('SELECT count(*) FROM rt') LIKE '%SiftAggV6%';
	SELECT count(*) INTO node FROM rt;
	n_md5 := groups_md5();
	PERFORM set_config('sift.enable_agg', 'off', true);
	ios_plan := plan_of('SELECT count(*) FROM rt') LIKE '%Index Only Scan%';
	SELECT count(*) INTO ios FROM rt;
	i_md5 := groups_md5();
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	SELECT count(*) INTO heap FROM rt;
	h_md5 := groups_md5();
	groups_same := n_md5 = h_md5 AND i_md5 = h_md5;
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	RETURN NEXT;
END $$;

DELETE FROM rt WHERE id % 29 = 0;
VACUUM rt;
INSERT INTO rt SELECT i, i % 5, i FROM generate_series(50001, 50600) i;
SELECT * FROM three_counts();
UPDATE rt SET v = v + 1 WHERE id % 13 = 0;
SELECT * FROM three_counts();
VACUUM rt;
SELECT * FROM three_counts();

-- the same again: the addresses the second VACUUM freed are reused by the next INSERT, and rows are deleted that were inserted after the index was built
DELETE FROM rt WHERE id > 50000 AND id % 3 = 0;
UPDATE rt SET v = v + 2 WHERE id % 7 = 0;
VACUUM rt;
INSERT INTO rt SELECT i, i % 5, i FROM generate_series(60001, 61000) i;
UPDATE rt SET v = v + 1 WHERE id > 60000 AND id % 2 = 0;
VACUUM rt;
SELECT * FROM three_counts();

DROP FUNCTION three_counts();
DROP FUNCTION groups_md5();
DROP FUNCTION plan_of(text);
DROP TABLE rt;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s8 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_vacuum_reused_tid

-- >>> agg_v6_variant_stream
CREATE SCHEMA s9;
GRANT USAGE ON SCHEMA s9 TO PUBLIC;
SET search_path = s9, public;
-- The sorted streaming GROUP BY and Top-N (src/agg/stream_exec.c) against a loop over the rows. Each is the number of failed cases.

SELECT sift._test_stream_exec(6, 300) AS stream_failures;
SELECT sift._test_stream_exec(33, 300) AS stream_failures;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s9 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_variant_stream

-- >>> agg_v6_date_trunc_dates
CREATE SCHEMA s10;
GRANT USAGE ON SCHEMA s10 TO PUBLIC;
SET search_path = s10, public;
-- date_trunc of a date (src/agg/vexpr/cal.c vcal_date_to_ts, src/agg/vexpr/vexpr.c op_calendar), held to PostgreSQL's own function to the microsecond. date_trunc has no form for a date: PostgreSQL reads the
-- date as a timestamptz, at its midnight in the zone of the session (a timestamp, in none), so a zone that skipped a midnight gives the instant after it. Every unit, in zones with and without daylight
-- saving time and with a change at midnight, over the days around the first and the last date a timestamp holds, the infinities, and the range that has a change of every zone of the list.
SET sift.test_create_format1 = off;
SET DateStyle = 'ISO, MDY';

CREATE TABLE kd AS
SELECT (n - 1) / 2048 AS chunk, array_agg(k ORDER BY n) AS k, count(*)::int AS len
FROM (SELECT row_number() OVER (ORDER BY k) AS n, k FROM (
	SELECT generate_series(-3000, 12000) AS k
	UNION SELECT generate_series(-2451145, -2451100)
	UNION SELECT generate_series(106751930, 106751982)
	UNION SELECT generate_series(7000, 7400) * 37
	UNION VALUES (2147483647), (-2147483648)) AS s) AS q GROUP BY 1;
ALTER TABLE kd ADD i int8[], ADD j int8[], ADD x float8[], ADD m bool[], ADD d jsonb[], ADD t timestamp[];
UPDATE kd SET i = array_fill(0::int8, ARRAY[len]), j = array_fill(0::int8, ARRAY[len]), x = array_fill(0::float8, ARRAY[len]), m = array_fill(true, ARRAY[len]),
              d = array_fill('{}'::jsonb, ARRAY[len]), t = array_fill('2000-01-01'::timestamp, ARRAY[len]);
SELECT (SELECT sum(len) FROM kd) > 15000 AS days, count(*) AS chunks FROM kd;

CREATE VIEW kr AS
SELECT z.chunk, u.ord, CASE u.k WHEN 2147483647 THEN 'infinity'::date WHEN -2147483648 THEN '-infinity'::date ELSE DATE '2000-01-01' + u.k END AS dt
FROM kd z, unnest(z.k) WITH ORDINALITY AS u(k, ord);

CREATE FUNCTION kdiff(prog text, pgx text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format($q$
		SELECT count(*) FROM (
			((SELECT c.chunk, e.ord, e.res FROM kd c, sift._test_vexpr(%1$L, c.i, c.j, c.k, c.x, c.x, c.m, c.d, NULL, NULL, NULL, c.t, NULL) AS e)
			 EXCEPT ALL
			 (SELECT chunk, ord, (%2$s)::text FROM kr))
			UNION ALL
			((SELECT chunk, ord, (%2$s)::text FROM kr)
			 EXCEPT ALL
			 (SELECT c.chunk, e.ord, e.res FROM kd c, sift._test_vexpr(%1$L, c.i, c.j, c.k, c.x, c.x, c.m, c.d, NULL, NULL, NULL, c.t, NULL) AS e))) AS d
	$q$, prog, pgx) INTO n;
	RETURN n;
END $$;

CREATE FUNCTION date_wrong(tz text, kind text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE u text; n bigint := 0;
BEGIN
	PERFORM set_config('TimeZone', tz, false);
	FOREACH u IN ARRAY ARRAY['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'] LOOP
		n := n + kdiff(format('(%s %s k)', CASE kind WHEN 'tz' THEN 'trunctz' ELSE 'trunc' END, u),
					   format('date_trunc(%L, dt%s)', u, CASE kind WHEN 'tz' THEN '' ELSE '::timestamp' END));
	END LOOP;
	RETURN n;
END $$;

-- the wrong rows of 8 units over every date, in each zone (tz: a timestamptz in the zone of the session; plain: a timestamp)
SELECT tz, date_wrong(tz, 'tz') AS tz_wrong, date_wrong(tz, 'plain') AS plain_wrong
FROM unnest(ARRAY['UTC', 'America/New_York', 'America/Sao_Paulo', 'Europe/Berlin', 'Asia/Kolkata', 'Pacific/Chatham', 'Australia/Lord_Howe', 'America/Havana', 'Etc/GMT+3']) AS tz ORDER BY tz;

-- the day PostgreSQL gives for a midnight a zone skipped (Sao Paulo on 2018-11-04), and the error of a date past the last timestamp
SET TimeZone = 'America/Sao_Paulo';
SELECT e.res FROM sift._test_vexpr('(trunctz day k)', '{0}', '{0}', ARRAY[(DATE '2018-11-04' - DATE '2000-01-01')], '{0}', '{0}', '{t}', '{"{}"}', NULL, NULL, NULL, ARRAY['2000-01-01'::timestamp], NULL) AS e;
SELECT date_trunc('day', DATE '2018-11-04');
CREATE FUNCTION failure(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN others THEN
	RETURN SQLERRM;
END $$;
SELECT failure($q$SELECT e.res FROM sift._test_vexpr('(trunc day k)', '{0}', '{0}', ARRAY[106751983], '{0}', '{0}', '{t}', '{"{}"}', NULL, NULL, NULL, ARRAY['2000-01-01'::timestamp], NULL) AS e$q$) AS engine,
       failure($q$SELECT date_trunc('day', DATE '2000-01-01' + 106751983)$q$) AS postgres;
DROP FUNCTION failure(text);

DROP FUNCTION date_wrong(text, text);
DROP FUNCTION kdiff(text, text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s10 CASCADE;
RESET client_min_messages;
-- <<< agg_v6_date_trunc_dates

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
