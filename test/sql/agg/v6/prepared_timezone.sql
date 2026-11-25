-- A statement planned once (a generic plan, cached by PREPARE or by a PL/pgSQL function) over a sift_table and over a sift_col index reads the time zone of the session when it runs, not the one it was planned in:
-- the same plans are run under several zones across the DST transitions of America/New_York, America/Sao_Paulo, Asia/Kathmandu (+05:45) and Australia/Lord_Howe (a 30 minute shift), and every answer is the heap twin's.
-- requires: build col
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE SCHEMA tzs;
SET search_path = tzs, public;
CREATE TABLE tzh (id int PRIMARY KEY, ts timestamptz NOT NULL, v int NOT NULL);
INSERT INTO tzh SELECT i * 100 + m, ts, (i + m) % 17 FROM generate_series(timestamptz '2025-03-07 00:00+00', timestamptz '2025-03-11 23:00+00', interval '5 minutes') WITH ORDINALITY g(ts, i), generate_series(1, 25) m;
INSERT INTO tzh SELECT 1000000 + i * 100 + m, ts, (i + m) % 17 FROM generate_series(timestamptz '2025-10-31 00:00+00', timestamptz '2025-11-04 23:00+00', interval '5 minutes') WITH ORDINALITY g(ts, i), generate_series(1, 25) m;
CREATE TABLE tzt (LIKE tzh INCLUDING ALL) USING sift_table;
INSERT INTO tzt SELECT * FROM tzh;
CREATE TABLE tzc (LIKE tzh INCLUDING ALL);
INSERT INTO tzc SELECT * FROM tzh;
CREATE INDEX tzc_col ON tzc USING sift_col (ts, v);
ANALYZE tzh; ANALYZE tzt; ANALYZE tzc;

-- the statements: %T is the table, arg the parameter (a zone name, or a timestamp of the zone of the session)
CREATE TABLE stmts (name text PRIMARY KEY, body text NOT NULL, needs_ts boolean NOT NULL);
INSERT INTO stmts VALUES
	('by_day',     'SELECT date_trunc(''day'', ts)::text AS k, count(*) AS c, sum(v) AS s FROM %T GROUP BY 1', false),
	('by_hour',    'SELECT date_trunc(''hour'', ts)::text AS k, count(*) AS c, sum(v) AS s FROM %T WHERE ts >= arg::timestamptz AND ts < arg::timestamptz + interval ''2 days'' GROUP BY 1', true),
	('by_bin',     'SELECT date_bin(''1 hour'', ts, arg::timestamptz)::text AS k, count(*) AS c, sum(v) AS s FROM %T WHERE ts < arg::timestamptz + interval ''3 days'' GROUP BY 1', true),
	('one_day',    'SELECT arg AS k, count(*) AS c, sum(v) AS s FROM %T WHERE date_trunc(''day'', ts) = arg::timestamptz', true),
	('by_date',    'SELECT ts::date::text AS k, count(*) AS c, sum(v) AS s FROM %T GROUP BY 1', false),
	('by_extract', 'SELECT extract(hour FROM ts)::int || '':'' || extract(dow FROM ts)::int AS k, count(*) AS c, sum(v) AS s FROM %T GROUP BY 1', false),
	('by_local',   'SELECT (ts AT TIME ZONE ''Pacific/Auckland'')::date::text AS k, count(*) AS c, sum(v) AS s FROM %T GROUP BY 1', false),
	('by_week',    'SELECT date_trunc(''week'', ts)::text AS k, count(*) AS c, sum(v) AS s FROM %T GROUP BY 1', false);

-- one PL/pgSQL function per statement and table: its statement is planned on the first call and its plan is kept for the session
SELECT format('CREATE FUNCTION run_%s_%s(arg text) RETURNS TABLE (k text, c bigint, s bigint) LANGUAGE plpgsql AS $f$ BEGIN RETURN QUERY %s; END $f$',
              t.tag, s.name, replace(s.body, '%T', t.tbl)) AS ddl
FROM stmts s, (VALUES ('h', 'tzh'), ('t', 'tzt'), ('c', 'tzc')) t(tag, tbl) \gexec

CREATE FUNCTION check_zone(zone text, ts text) RETURNS TABLE (name text, h text, t text, c text) LANGUAGE plpgsql AS $$
DECLARE s record; ah text; at text; ac text;
BEGIN
	PERFORM set_config('TimeZone', zone, false);
	FOR s IN SELECT * FROM stmts ORDER BY name LOOP
		EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM run_h_%s(%L) x', s.name, ts) INTO ah;
		EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM run_t_%s(%L) x', s.name, ts) INTO at;
		EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM run_c_%s(%L) x', s.name, ts) INTO ac;
		name := s.name;
		h := md5(ah);
		t := CASE WHEN at = ah THEN 'same' ELSE 'DIFFERENT' END;
		c := CASE WHEN ac = ah THEN 'same' ELSE 'DIFFERENT' END;
		RETURN NEXT;
	END LOOP;
END $$;

SET plan_cache_mode = force_generic_plan;
-- the hash of the heap answer tells the zones apart: the same statement answers differently in each
SELECT * FROM check_zone('UTC', '2025-03-08 00:00+00');
SELECT * FROM check_zone('America/Sao_Paulo', '2025-03-08 00:00+00');
SELECT * FROM check_zone('America/New_York', '2025-03-09 00:00-05');
SELECT * FROM check_zone('UTC', '2025-11-02 00:00+00');
SELECT * FROM check_zone('America/New_York', '2025-11-02 00:00-04');
SELECT * FROM check_zone('Asia/Kathmandu', '2025-03-09 00:00+05:45');
SELECT * FROM check_zone('Australia/Lord_Howe', '2025-10-31 00:00+11');
SELECT * FROM check_zone('America/Sao_Paulo', '2025-11-01 00:00-03');
SELECT * FROM check_zone('America/New_York', '2025-03-09 00:00-05');

-- the zone changes inside a transaction, between two runs of the same plan
BEGIN;
SET LOCAL TimeZone = 'UTC';
SELECT count(*) FROM run_t_by_day('') x WHERE x.c > 0;
SET LOCAL TimeZone = 'America/New_York';
SELECT (SELECT string_agg(x::text, E'\n' ORDER BY x::text) FROM run_t_by_day('') x) = (SELECT string_agg(x::text, E'\n' ORDER BY x::text) FROM run_h_by_day('') x) AS new_york_by_day;
SET LOCAL TimeZone = 'America/Sao_Paulo';
SELECT (SELECT string_agg(x::text, E'\n' ORDER BY x::text) FROM run_t_by_day('') x) = (SELECT string_agg(x::text, E'\n' ORDER BY x::text) FROM run_h_by_day('') x) AS sao_paulo_by_day;
COMMIT;

-- the engines that answered: the table and the index
RESET plan_cache_mode;
CREATE FUNCTION engine(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE plan text;
BEGIN
	FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF plan LIKE '%SiftAggV6%' THEN RETURN 'v6'; END IF;
	END LOOP;
	RETURN 'native';
END $$;
SELECT engine('SELECT date_trunc(''day'', ts), count(*), sum(v) FROM tzt GROUP BY 1') AS table_day,
       engine('SELECT ts::date, count(*), sum(v) FROM tzt GROUP BY 1') AS table_date,
       engine('SELECT date_trunc(''day'', ts), count(*), sum(v) FROM tzc GROUP BY 1') AS index_day;
RESET search_path;
DROP SCHEMA tzs CASCADE;
DROP EXTENSION pg_sift;
