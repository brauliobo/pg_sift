-- date_trunc of a date (src/agg/vexpr_cal.c vcal_date_to_ts, src/agg/vexpr.c op_calendar), held to PostgreSQL's own function to the microsecond. date_trunc has no form for a date: PostgreSQL reads the
-- date as a timestamptz, at its midnight in the zone of the session (a timestamp, in none), so a zone that skipped a midnight gives the instant after it. Every unit, in zones with and without daylight
-- saving time and with a change at midnight, over the days around the first and the last date a timestamp holds, the infinities, and the range that has a change of every zone of the list.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
CREATE EXTENSION pg_sift_test;
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
