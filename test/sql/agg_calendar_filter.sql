-- A comparison of a calendar step of a timestamp leaf with a constant (x::date, date_trunc(unit, x), extract(year FROM x)) is a range of the leaf: the rows are those of the
-- normal plan, in the zones whose clocks jump (a gap at midnight, a day skipped, a repeated hour, a half hour of daylight time), for every operator and at the edges of the steps,
-- the infinities, NULL and the constants that stay with the normal plan (outside the years 100 to 9900, NULL, a cross type comparison).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE cf (id int, ts timestamp, tz timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO cf
SELECT row_number() OVER (), t.i AT TIME ZONE 'UTC', t.i
FROM (SELECT DISTINCT (x.t + k * interval '7 minutes') AS i
      FROM (VALUES ('2024-03-10 07:00+00'::timestamptz), ('2024-11-03 06:00+00'), ('2018-11-04 03:00+00'), ('2019-02-17 02:00+00'), ('2024-04-06 15:00+00'),
                   ('2024-10-05 15:30+00'), ('2011-12-30 10:00+00'), ('2024-03-31 01:00+00'), ('2024-10-27 01:00+00'), ('2023-11-05 05:00+00'), ('2024-03-10 05:00+00'),
                   ('2024-11-03 04:30+00'), ('2024-04-14 02:00+00')) x(t), generate_series(-150, 150) k) t;
INSERT INTO cf VALUES (100000, 'infinity', 'infinity'), (100001, '-infinity', '-infinity'), (100002, NULL, NULL), (100003, '0100-01-01', '0100-01-01 00:00+00'),
	(100004, '9899-12-31 23:59:59.999999', '9899-12-31 23:59:59.999999+00'), (100005, '9900-01-01', '9900-01-01 00:00+00'), (100006, '2024-01-01', '2024-01-01 00:00+00'),
	(100007, '2023-12-31 23:59:59.999999', '2023-12-31 23:59:59.999999+00');
CREATE INDEX cf_col ON cf USING sift_col (id, ts, tz);
VACUUM (FREEZE, ANALYZE) cf;

CREATE TABLE cf_zone (zone text, edge timestamptz);
INSERT INTO cf_zone VALUES ('America/New_York', '2024-03-10 07:00+00'), ('America/New_York', '2024-11-03 06:00+00'), ('America/Sao_Paulo', '2018-11-04 03:00+00'),
	('America/Sao_Paulo', '2019-02-17 02:00+00'), ('Australia/Lord_Howe', '2024-04-06 15:00+00'), ('Australia/Lord_Howe', '2024-10-05 15:30+00'),
	('Pacific/Apia', '2011-12-30 10:00+00'), ('Europe/London', '2024-03-31 01:00+00'), ('Europe/London', '2024-10-27 01:00+00'), ('America/Havana', '2023-11-05 05:00+00'), ('America/Havana', '2024-03-10 05:00+00'),
	('America/St_Johns', '2024-03-10 05:30+00'), ('America/St_Johns', '2024-11-03 04:30+00'), ('Africa/Casablanca', '2024-03-10 02:00+00'), ('Africa/Casablanca', '2024-04-14 02:00+00'),
	('Antarctica/Troll', '2024-03-31 01:00+00'), ('Asia/Kolkata', '2024-03-10 07:00+00'),
	('UTC', '2024-03-10 07:00+00');

CREATE FUNCTION count_of(q text, scan bool) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.count_agg_pushdown_set(q, scan) $$;

CREATE FUNCTION uses_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; s bool := false;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		s := s OR l LIKE '%SiftAgg%';
	END LOOP;
	RETURN s;
END $$;

-- every function of a calendar step against constants at the edges of the zone's jumps, for the six operators and both column types
CREATE TABLE cf_result (zone text, statements int, same int, scans int);
DO $$
DECLARE
	z record; q text; f text; o text; c text; fn text[]; ct text; col text;
	n int; s int; sc int;
BEGIN
	FOR z IN SELECT zone FROM cf_zone GROUP BY zone ORDER BY zone LOOP
		n := 0; s := 0; sc := 0;
		EXECUTE format('SET LOCAL TimeZone = %L', z.zone);
		FOREACH col IN ARRAY ARRAY['tz', 'ts'] LOOP
			FOREACH f IN ARRAY ARRAY['%1$s::date', 'date_trunc(''hour'', %1$s)', 'date_trunc(''day'', %1$s)', 'date_trunc(''week'', %1$s)', 'date_trunc(''month'', %1$s)',
				'date_trunc(''quarter'', %1$s)', 'date_trunc(''year'', %1$s)', 'date_trunc(''decade'', %1$s)', 'date_trunc(''minute'', %1$s)', 'extract(year FROM %1$s)', 'date_part(''year'', %1$s)'] LOOP
				FOREACH o IN ARRAY ARRAY['=', '<>', '<', '<=', '>', '>='] LOOP
					FOR c IN
						SELECT DISTINCT format(CASE WHEN f LIKE '%::date' THEN '%L::date' WHEN f LIKE 'extract%' OR f LIKE 'date_part%' THEN '%s' ELSE '%L::' || CASE col WHEN 'tz' THEN 'timestamptz' ELSE 'timestamp' END END,
						       CASE WHEN f LIKE 'extract%' OR f LIKE 'date_part%' THEN extract(year FROM (e.edge AT TIME ZONE z.zone) + d)::text
						            ELSE ((e.edge AT TIME ZONE z.zone) + d)::text END)
						FROM cf_zone e, unnest(ARRAY[interval '0', interval '-1 microsecond', interval '1 microsecond', interval '-1 day', interval '1 day', interval '-1 hour', interval '3 hours',
							interval '-30 days', interval '400 days']) d
						WHERE e.zone = z.zone AND (col = 'tz' OR e.zone = 'UTC')
					LOOP
						q := format('SELECT count(*) FROM cf WHERE ' || f || ' %s %s', col, o, c);
						n := n + 1;
						IF count_of(q, true) = count_of(q, false) THEN s := s + 1; END IF;
						IF uses_scan(q) THEN sc := sc + 1; END IF;
					END LOOP;
				END LOOP;
			END LOOP;
		END LOOP;
		INSERT INTO cf_result VALUES (z.zone, n, s, sc);
	END LOOP;
END $$;
SELECT zone, statements > 0 AS ran, statements = same AS all_same, scans = statements AS all_scans FROM cf_result ORDER BY zone;

-- the edges, written out: constants outside the window, infinities, NULL, a cross type comparison and a parameter keep the normal plan or give its rows
SET TimeZone = 'America/Sao_Paulo';
SELECT q, count_of(q, true) = count_of(q, false) AS same FROM (VALUES
	('SELECT count(*) FROM cf WHERE tz::date = ''infinity''::date'),
	('SELECT count(*) FROM cf WHERE tz::date < ''-infinity''::date'),
	('SELECT count(*) FROM cf WHERE tz::date >= ''9999-12-31''::date'),
	('SELECT count(*) FROM cf WHERE tz::date < ''0001-01-01''::date'),
	('SELECT count(*) FROM cf WHERE date_trunc(''day'', ts) = ''infinity''::timestamp'),
	('SELECT count(*) FROM cf WHERE date_trunc(''year'', ts) >= ''-infinity''::timestamp'),
	('SELECT count(*) FROM cf WHERE ts::date = NULL::date'),
	('SELECT count(*) FROM cf WHERE ts::date = ''2024-03-10''::timestamp'),
	('SELECT count(*) FROM cf WHERE extract(year FROM ts) = 2024.5'),
	('SELECT count(*) FROM cf WHERE extract(year FROM ts) = ''Infinity''::numeric'),
	('SELECT count(*) FROM cf WHERE extract(year FROM tz) >= 9900'),
	('SELECT count(*) FROM cf WHERE ''2024-03-10''::date = ts::date'),
	('SELECT count(*) FROM cf WHERE ts::date BETWEEN ''2024-03-10'' AND ''2024-03-11'''),
	('SELECT count(*) FROM cf WHERE date_trunc(''day'', ts) = ''2024-03-10 00:00:01''::timestamp'),
	('SELECT count(*) FROM cf WHERE (ts::date = ''2024-03-10'' OR tz::date = ''2024-11-03'') AND id > 5'),
	('SELECT count(*) FROM cf WHERE NOT (ts::date = ''2024-03-10'')')
) v(q);

-- the bounds follow the zone of the session that runs the scan
PREPARE by_day AS SELECT count(*) FROM cf WHERE tz::date = '2024-03-10'::date;
SET plan_cache_mode = force_generic_plan;
SET TimeZone = 'UTC';
SELECT count(*) AS utc_day FROM cf WHERE tz::date = '2024-03-10'::date;
EXECUTE by_day;
SET TimeZone = 'Pacific/Apia';
EXECUTE by_day;
SET sift.enable_agg = off;
EXECUTE by_day;
RESET sift.enable_agg;
RESET plan_cache_mode;

SELECT uses_scan('SELECT count(*) FROM cf WHERE ts::date = ''2024-03-10''') AS date_eq,
       uses_scan('SELECT count(*) FROM cf WHERE date_trunc(''month'', tz) >= ''2024-03-01''::timestamptz') AS trunc_ge,
       uses_scan('SELECT count(*) FROM cf WHERE extract(year FROM ts) = 2024') AS year_eq;

DROP FUNCTION count_of(text, bool);
DROP FUNCTION uses_scan(text);
DROP TABLE cf_result, cf_zone, cf;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
