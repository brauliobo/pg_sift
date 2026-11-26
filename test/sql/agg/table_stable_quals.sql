-- A comparison of a sift_table column with a stable expression (now(), CURRENT_DATE, current_setting()) or a parameter is a key of the scan of SiftAggV6 and SiftAggV6Push whose value is read when the scan starts, and again when it is rescanned:
-- the rows are those of the heap twin for every type of the column. Every check prints the engine that answered and whether the rows are the twin's.
-- requires: build col
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE sq (id int NOT NULL, ts timestamptz NOT NULL, d date NOT NULL, k text NOT NULL, n numeric(12,2) NOT NULL, u uuid NOT NULL, f float8 NOT NULL, v int NOT NULL, e int NOT NULL)
	USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE sqh (LIKE sq) WITH (autovacuum_enabled = false);
CREATE TABLE sd (e int NOT NULL, grp int NOT NULL, PRIMARY KEY (e, grp));
INSERT INTO sq SELECT g, now() - (g % 400) * interval '1 day', current_date - g % 400, 'k' || g % 7, (g % 50)::numeric,
-- 60000 rows are 8 granules of 8192 that every rescan reads again with the new value of the stable key
	('00000000-0000-0000-0000-' || lpad((g % 20)::text, 12, '0'))::uuid, (g % 90)::float8, g % 100, 1 + g % 300 FROM generate_series(1, 60000) g;
INSERT INTO sqh SELECT * FROM sq;
INSERT INTO sd SELECT g, 1 + g % 7 FROM generate_series(1, 300) g;
SELECT sift.table_maintain('sq') IS NOT NULL AS maintained;
ANALYZE sq; ANALYZE sqh; ANALYZE sd;
SET app.k = 'k3'; SET app.n = '25'; SET app.u = '00000000-0000-0000-0000-000000000003'; SET app.i = '55'; SET app.f = '44.5'; SET app.t = '2020-01-01';

-- engine and parity of a statement over sq, against the same statement over its twin sqh and sd's twin sdh
CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text; engine text := 'native';
BEGIN
	FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF plan LIKE '%SiftAggV6Push%' THEN engine := 'push'; ELSIF plan LIKE '%SiftAggV6%' AND engine = 'native' THEN engine := 'v6'; END IF;
	END LOOP;
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO a;
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || regexp_replace(regexp_replace(q, '\msq\M', 'sqh', 'g'), '\msd\M', 'sdh', 'g') || ') x' INTO b;
	RETURN engine || ' ' || CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END;
END $$;
CREATE TABLE sdh (LIKE sd INCLUDING ALL);
INSERT INTO sdh SELECT * FROM sd;
ANALYZE sdh;

SELECT same('SELECT count(*), sum(v) FROM sq WHERE ts >= now() - interval ''30 days''') AS timestamptz_now;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE ts < now() - interval ''300 days''') AS timestamptz_now_lt;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE d >= current_date - 7') AS date_current_date;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE d = current_date - 3') AS date_equal;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE ts >= current_setting(''app.t'')::timestamptz') AS timestamptz_setting;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE v >= current_setting(''app.i'')::int') AS int_setting;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE f >= current_setting(''app.f'')::float8') AS float8_setting;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE k = current_setting(''app.k'')') AS text_setting;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE n >= current_setting(''app.n'')::numeric') AS numeric_setting;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE u = current_setting(''app.u'')::uuid') AS uuid_setting;
SELECT same('SELECT d, count(*) FROM sq WHERE d >= current_date - 2 GROUP BY d') AS date_grouped;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE ts >= now() - interval ''30 days'' AND v >= current_setting(''app.i'')::int') AS two_runtime_keys;
SELECT same('SELECT count(*) FROM sq WHERE v > current_setting(''app.null'', true)::int') AS null_operand;

-- parameters of a prepared statement, generic and custom
PREPARE sp_ts(timestamptz) AS SELECT count(*), sum(v) FROM sq WHERE ts >= $1;
PREPARE sp_ts_h(timestamptz) AS SELECT count(*), sum(v) FROM sqh WHERE ts >= $1;
PREPARE sp_any(int[]) AS SELECT count(*), sum(v) FROM sq WHERE id = ANY ($1);
PREPARE sp_any_h(int[]) AS SELECT count(*), sum(v) FROM sqh WHERE id = ANY ($1);
SET plan_cache_mode = force_generic_plan;
EXECUTE sp_ts(now() - interval '30 days');
EXECUTE sp_ts_h(now() - interval '30 days');
EXECUTE sp_ts(now() - interval '200 days');
EXECUTE sp_ts_h(now() - interval '200 days');
EXECUTE sp_any(ARRAY[1, 2, 3, 70]);
EXECUTE sp_any_h(ARRAY[1, 2, 3, 70]);
RESET plan_cache_mode;

-- SiftAggV6Push: the values of the small side as the key of the scan, with a stable qual and a parameter on the fact
SELECT same('SELECT count(*), sum(v) FROM sq WHERE e IN (SELECT e FROM sd WHERE grp IN (3, 4)) AND ts >= now() - interval ''30 days''') AS push_stable;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE e IN (SELECT e FROM sd WHERE grp IN (3, 4)) AND d >= current_date - 7') AS push_date;
SELECT same('SELECT v % 5, count(*), sum(v) FROM sq WHERE e IN (SELECT e FROM sd WHERE grp IN (3, 4)) AND ts >= now() - interval ''100 days'' GROUP BY 1') AS push_grouped;
SELECT same('SELECT count(*), sum(v) FROM sq JOIN sd USING (e) WHERE sd.grp = 3 AND sq.ts >= now() - interval ''30 days''') AS join_stable;
PREPARE sp_push(timestamptz) AS SELECT count(*), sum(v) FROM sq WHERE e IN (SELECT e FROM sd WHERE grp IN (3, 4)) AND ts >= $1;
PREPARE sp_push_h(timestamptz) AS SELECT count(*), sum(v) FROM sqh WHERE e IN (SELECT e FROM sdh WHERE grp IN (3, 4)) AND ts >= $1;
EXECUTE sp_push(now() - interval '30 days');
EXECUTE sp_push_h(now() - interval '30 days');
EXECUTE sp_push(now() - interval '250 days');
EXECUTE sp_push_h(now() - interval '250 days');
SET plan_cache_mode = force_generic_plan;
EXECUTE sp_push(now() - interval '30 days');
EXECUTE sp_push_h(now() - interval '30 days');
EXECUTE sp_push(now() - interval '250 days');
EXECUTE sp_push_h(now() - interval '250 days');
RESET plan_cache_mode;

-- a rescan: the inner side of a nested loop reads the key again for each outer row
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;
SELECT same('SELECT o.d, x.c FROM (SELECT DISTINCT d FROM sq WHERE d >= current_date - 5) o, LATERAL (SELECT count(*) c FROM sq WHERE sq.d = o.d AND sq.ts >= now() - interval ''399 days'') x') AS rescan_lateral;
SELECT same('SELECT o.k, x.c FROM (SELECT DISTINCT k FROM sq) o, LATERAL (SELECT count(*) c FROM sq WHERE sq.k = o.k AND sq.d >= current_date - 30) x') AS rescan_text_param;
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT same('SELECT o.d, x.c FROM (SELECT DISTINCT d FROM sq WHERE d >= current_date - 5) o, LATERAL (SELECT count(*) c FROM sq WHERE sq.d = o.d AND sq.ts >= now() - interval ''399 days'') x') AS rescan_lateral_parallel;
SELECT same('SELECT count(*), sum(v) FROM sq WHERE ts >= now() - interval ''30 days'' AND d >= current_date - 20') AS parallel_stable;
RESET max_parallel_workers_per_gather; RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_material;
DROP TABLE sq, sqh, sd, sdh CASCADE;
DROP FUNCTION same(text);
DROP EXTENSION pg_sift;
