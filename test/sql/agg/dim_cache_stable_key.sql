-- S119-16: a star join whose dimension key is computed by a stable function (timestamptz::date follows TimeZone) is never served from the dimension another statement kept:
-- after SET TimeZone the answer is the heap's (sift.enable_agg = off).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_hashagg = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET client_min_messages = warning;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET TimeZone = 'UTC';

CREATE TABLE dk_dim (k int PRIMARY KEY, ts timestamptz NOT NULL, a int) WITH (autovacuum_enabled = false);
INSERT INTO dk_dim SELECT i, timestamptz '2026-01-01 00:00+00' + i * interval '5 hours', i % 7 FROM generate_series(0, 199) i;
CREATE TABLE dk_ev (id int PRIMARY KEY, k int, day date, v int) WITH (autovacuum_enabled = false);
INSERT INTO dk_ev SELECT i, i % 200, date '2026-01-01' + i % 45, i % 11 FROM generate_series(1, 3000) i;
CREATE INDEX dk_dim_col ON dk_dim USING sift_col (k, ts, a);
CREATE INDEX dk_ev_col ON dk_ev USING sift_col (id, k, day, v) WITH (connect = 'k -> dk_dim(k)');
VACUUM (FREEZE, ANALYZE) dk_dim, dk_ev;

CREATE FUNCTION engaged(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l LIKE '%SiftAggV6%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
\set q 'SELECT dk_dim.a, count(*), sum(dk_ev.v) FROM dk_ev JOIN dk_dim ON dk_dim.ts::date = dk_ev.day GROUP BY 1 ORDER BY 1'
SELECT engaged(:'q') AS engine;
SELECT answer(:'q', true) = answer(:'q', false) AS utc_same;
SET TimeZone = 'Pacific/Kiritimati';
SELECT answer(:'q', true) = answer(:'q', false) AS kiritimati_same;
SELECT answer(:'q', true) = answer(:'q', false) AS kiritimati_again;
RESET TimeZone;

DROP FUNCTION engaged(text), answer(text, bool);
DROP TABLE dk_ev, dk_dim;
DROP EXTENSION pg_sift;
