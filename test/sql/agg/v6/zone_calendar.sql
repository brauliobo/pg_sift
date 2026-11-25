-- date_trunc and EXTRACT of a timestamptz in the zone of the session (src/agg/vexpr/cal.c, src/core/zoneoff.c), held to PostgreSQL's own functions to the microsecond: in zones
-- with daylight saving time and across its changes, in zones of a half hour or a quarter of an hour, with a change at midnight, a negative one, and the years before and after the
-- rules the tables of the zones hold. A day or more is cut to the local midnight, whose offset is that of the midnight and not of the row; a second, a minute and an hour keep the row's.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET DateStyle = 'ISO, MDY';

-- the instants, as the microseconds of a timestamp column the engine reads as instants: a coarse grid over two years, the weeks around the changes of 2026 and 2040 of the
-- northern zones, the changes of Sao Paulo (a midnight that does not exist), and the first and last years of the tables
CREATE TABLE zi AS
SELECT row_number() OVER (ORDER BY t) AS n, t FROM (
	SELECT generate_series('2025-12-28'::timestamp, '2027-01-05', '53 minutes') AS t
	UNION SELECT generate_series('2026-03-01'::timestamp, '2026-04-05', '5 minutes')
	UNION SELECT generate_series('2026-10-20'::timestamp, '2026-11-10', '5 minutes')
	UNION SELECT generate_series('2040-03-01'::timestamp, '2040-04-05', '1 hour')
	UNION SELECT generate_series('2018-10-30'::timestamp, '2018-11-08', '7 minutes')
	UNION SELECT generate_series('2019-02-12'::timestamp, '2019-02-22', '7 minutes')
	UNION SELECT generate_series('1883-11-15'::timestamp, '1883-11-20', '1 hour')
	UNION SELECT generate_series('2099-12-30'::timestamp, '2100-01-03', '1 hour')
	UNION VALUES ('1890-01-01'::timestamp), ('1900-06-01'), ('1969-12-31 23:59:59'), ('2037-03-29'), ('4000-01-01'), ('0001-06-01 BC'), ('9999-12-31 23:59:59.999999')
) AS g;
CREATE TABLE zc AS
SELECT (n - 1) / 2048 AS chunk, array_agg(t ORDER BY n) AS t, count(*)::int AS len FROM zi GROUP BY 1;
ALTER TABLE zc ADD i int8[], ADD j int8[], ADD k int4[], ADD x float8[], ADD m bool[], ADD d jsonb[];
UPDATE zc SET i = array_fill(0::int8, ARRAY[len]), j = array_fill(0::int8, ARRAY[len]), k = array_fill(0, ARRAY[len]), x = array_fill(0::float8, ARRAY[len]),
              m = array_fill(true, ARRAY[len]), d = array_fill('{}'::jsonb, ARRAY[len]);
SELECT (SELECT count(*) FROM zi) > 30000 AS instants, count(*) AS chunks FROM zc;

-- the rows, from every chunk, where the engine and PostgreSQL differ over the expression of PostgreSQL (t is the instant, in the session's zone)
CREATE VIEW zrows AS SELECT z.chunk, u.t, u.ord FROM zc z, unnest(z.t) WITH ORDINALITY AS u(t, ord);

CREATE FUNCTION zdiff(prog text, pgx text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format($q$
		SELECT count(*) FROM (
			((SELECT c.chunk, e.ord, e.res FROM zc c, sift._test_vexpr(%1$L, c.i, c.j, c.k, c.x, c.x, c.m, c.d, NULL, NULL, NULL, c.t, NULL) AS e)
			 EXCEPT ALL
			 (SELECT chunk, ord, (%2$s)::text FROM zrows))
			UNION ALL
			((SELECT chunk, ord, (%2$s)::text FROM zrows)
			 EXCEPT ALL
			 (SELECT c.chunk, e.ord, e.res FROM zc c, sift._test_vexpr(%1$L, c.i, c.j, c.k, c.x, c.x, c.m, c.d, NULL, NULL, NULL, c.t, NULL) AS e))) AS d
	$q$, prog, pgx) INTO n;
	RETURN n;
END $$;

CREATE FUNCTION zone_wrong(tz text, kind text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE u text; n bigint := 0;
BEGIN
	PERFORM set_config('TimeZone', tz, false);
	IF kind = 'trunc' THEN
		FOREACH u IN ARRAY ARRAY['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'] LOOP
			n := n + zdiff(format('(trunctz %s t)', u), format('date_trunc(%L, t AT TIME ZONE ''UTC'')', u));
		END LOOP;
	ELSE
		FOREACH u IN ARRAY ARRAY['quarter', 'month', 'day', 'dow', 'isodow', 'doy', 'hour', 'minute'] LOOP
			n := n + zdiff(format('(extracttz %s t)', u), format('extract(%s from t AT TIME ZONE ''UTC'')', u));
		END LOOP;
	END IF;
	RETURN n;
END $$;

-- the wrong rows of 8 units of date_trunc and of 8 fields of EXTRACT, over every instant, in each zone
SELECT tz, zone_wrong(tz, 'trunc') AS trunc_wrong, zone_wrong(tz, 'extract') AS extract_wrong
FROM unnest(ARRAY['UTC', 'America/New_York', 'America/Sao_Paulo', 'Europe/Berlin', 'Asia/Kolkata', 'Asia/Kathmandu', 'Pacific/Chatham', 'Australia/Lord_Howe',
                  'America/Havana', 'Europe/Dublin', 'Asia/Tokyo', 'Etc/GMT+3']) AS tz ORDER BY tz;

-- the infinities are kept by date_trunc in a zone as in none
CREATE TABLE zs AS SELECT 0 AS chunk, ARRAY['infinity', '-infinity', '2026-03-08 12:00', 'infinity']::timestamp[] AS t, 4 AS len,
       array_fill(0::int8, ARRAY[4]) AS i, array_fill(0::int8, ARRAY[4]) AS j, array_fill(0, ARRAY[4]) AS k, array_fill(0::float8, ARRAY[4]) AS x,
       array_fill(true, ARRAY[4]) AS m, array_fill('{}'::jsonb, ARRAY[4]) AS d;
SET TimeZone = 'America/New_York';
SELECT e.ord, e.res FROM zs c, sift._test_vexpr('(trunctz day t)', c.i, c.j, c.k, c.x, c.x, c.m, c.d, NULL, NULL, NULL, c.t, NULL) AS e ORDER BY e.ord;

-- the cases that were wrong when every row took the offset of its own instant for the midnight it was cut to: a month that starts in standard time and a row in summer time,
-- the day of a spring forward (23 hours) and of a fall back (25 hours), a year, a week, a half hour zone, and the midnight that Sao Paulo skipped
CREATE FUNCTION trunc_of(tz text, unit text, instant timestamp, OUT engine text, OUT postgres text) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('TimeZone', tz, false);
	SELECT e.res INTO engine FROM sift._test_vexpr(format('(trunctz %s t)', unit), '{0}', '{0}', '{0}', '{0}', '{0}', '{t}', '{"{}"}', NULL, NULL, NULL, ARRAY[instant], NULL) AS e;
	postgres := date_trunc(unit, instant AT TIME ZONE 'UTC')::text;
END $$;
SELECT tz, unit, instant, (r).engine, (r).postgres, (r).engine = (r).postgres AS same
FROM (SELECT v.*, trunc_of(tz, unit, instant) AS r FROM (VALUES
  ('America/New_York', 'month', '2026-03-15 16:00'::timestamp), ('America/New_York', 'year', '2026-07-15 16:00'), ('America/New_York', 'day', '2026-03-08 20:00'),
  ('America/New_York', 'day', '2026-11-01 12:00'), ('America/New_York', 'week', '2026-03-08 20:00'), ('America/New_York', 'hour', '2026-11-01 06:30'),
  ('Asia/Kolkata', 'day', '2026-06-15 20:00'), ('Asia/Kolkata', 'hour', '2026-06-15 20:00'), ('America/Sao_Paulo', 'day', '2018-11-04 12:00'),
  ('Europe/Berlin', 'quarter', '2026-10-30 12:00')) AS v(tz, unit, instant)) AS q
ORDER BY tz, unit, instant;

DROP FUNCTION trunc_of(text, text, timestamp);
DROP FUNCTION zone_wrong(text, text);
DROP FUNCTION zdiff(text, text);
DROP VIEW zrows;
DROP TABLE zs, zc, zi;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
