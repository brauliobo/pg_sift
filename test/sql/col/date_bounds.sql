-- A date column compared with a timestamp or timestamptz (d < x + interval '1 day', d >= now() - interval '30 days'), and a timestamp or timestamptz column compared with
-- a date, are index quals of a plain sift_col index: the bound is the column's own (a date is its midnight, in the session's zone against a timestamptz), the rows are those
-- of the heap. Each comparison runs on an indexed table and on a copy with no index, in zones with a midnight that does not exist (America/Sao_Paulo, 2018-11-04).
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET TimeZone = 'America/Sao_Paulo';

CREATE TABLE xb_d AS SELECT g, d FROM (SELECT g, ('2018-10-25'::date + g) d FROM generate_series(0, 21) g UNION ALL VALUES (100, 'infinity'::date), (101, '-infinity'::date), (102, NULL::date)) s;
CREATE TABLE xb_ts AS SELECT g, t FROM (SELECT g, '2018-10-25'::timestamp + g * interval '1 hour' t FROM generate_series(0, 527) g UNION ALL VALUES (1000, 'infinity'::timestamp), (1001, '-infinity'::timestamp), (1002, NULL::timestamp)) s;
CREATE TABLE xb_tz AS SELECT g, t::timestamptz FROM xb_ts;
CREATE TABLE xr_d AS SELECT * FROM xb_d;
CREATE TABLE xr_ts AS SELECT * FROM xb_ts;
CREATE TABLE xr_tz AS SELECT * FROM xb_tz;
CREATE INDEX xb_d_i ON xb_d USING sift_col (d);
CREATE INDEX xb_ts_i ON xb_ts USING sift_col (t);
CREATE INDEX xb_tz_i ON xb_tz USING sift_col (t);
VACUUM (ANALYZE) xb_d, xb_ts, xb_tz, xr_d, xr_ts, xr_tz;

CREATE FUNCTION xb_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text = '';
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP r = r || l; END LOOP;
	RETURN r;
END $$;

-- every comparison of a table with the bound of the other type, in a zone: the rows of the indexed table are those of its copy, and the index is the plan
CREATE FUNCTION xb_check(zone text) RETURNS TABLE (compared int, wrong int, by_index int) LANGUAGE plpgsql SET enable_seqscan = off SET sift.enable_agg = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE
	c record;
	b text;
	o text;
	q text;
	a bigint;
	r bigint;
BEGIN
	compared := 0; wrong := 0; by_index := 0;
	EXECUTE format('SET LOCAL TimeZone = %L', zone);
	FOR c IN SELECT * FROM (VALUES ('xb_d', 'xr_d', 'd', 'timestamp'), ('xb_d', 'xr_d', 'd', 'timestamptz'), ('xb_ts', 'xr_ts', 't', 'date'), ('xb_tz', 'xr_tz', 't', 'date')) v(ix, ref, col, arg) LOOP
		FOREACH b IN ARRAY ARRAY['2018-11-03', '2018-11-04', '2018-11-04 00:00', '2018-11-04 00:00:01', '2018-11-04 00:30', '2018-11-04 01:00', '2018-11-03 23:59:59.999999',
								 '2018-11-04 12:00', '2018-10-25', '2018-10-24 23:00', '2018-11-15 23:59', '2019-01-01', '-infinity', 'infinity'] LOOP
			CONTINUE WHEN c.arg = 'date' AND b ~ ':';
			FOREACH o IN ARRAY ARRAY['<', '<=', '=', '>=', '>'] LOOP
				q := format('SELECT g FROM %s WHERE %s %s %L::%s', c.ix, c.col, o, b, c.arg);
				EXECUTE q;
				GET DIAGNOSTICS a = ROW_COUNT;
				EXECUTE replace(q, c.ix, c.ref);
				GET DIAGNOSTICS r = ROW_COUNT;
				compared := compared + 1;
				wrong := wrong + (a IS DISTINCT FROM r)::int;
				by_index := by_index + (xb_plan(q) ~ 'xb_[a-z]+_i')::int;
			END LOOP;
		END LOOP;
	END LOOP;
	RETURN NEXT;
END $$;

SELECT * FROM xb_check('America/Sao_Paulo');
SELECT * FROM xb_check('Europe/London');
SELECT * FROM xb_check('UTC');

-- the bound is an expression of the statement: a date plus an interval, a stable now() - interval
SET enable_seqscan = off;
SET sift.enable_agg = off;
SELECT (SELECT count(*) FROM xb_d WHERE d < '2018-11-04'::date + interval '1 day') = (SELECT count(*) FROM xr_d WHERE d < '2018-11-04'::date + interval '1 day') AS interval_bound,
       xb_plan('SELECT * FROM xb_d WHERE d < ''2018-11-04''::date + interval ''1 day''') ~ 'xb_d_i' AS interval_by_index,
       xb_plan('SELECT * FROM xb_d WHERE d >= now() - interval ''30 days''') ~ 'xb_d_i' AS now_by_index,
       xb_plan('SELECT * FROM xb_ts WHERE t >= current_date') ~ 'xb_ts_i' AS current_date_by_index;

-- a list of another type is the heap's to test
SELECT (SELECT count(*) FROM xb_d WHERE d = ANY ('{"2018-11-04 00:00","2018-11-05 12:00"}'::timestamp[])) =
       (SELECT count(*) FROM xr_d WHERE d = ANY ('{"2018-11-04 00:00","2018-11-05 12:00"}'::timestamp[])) AS list_of_timestamps;
DROP TABLE xb_d, xb_ts, xb_tz, xr_d, xr_ts, xr_tz;
DROP FUNCTION xb_check(text), xb_plan(text);
DROP EXTENSION pg_sift;
