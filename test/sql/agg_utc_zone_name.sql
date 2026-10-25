-- A bare UTC or GMT in date_trunc(unit, timestamptz, 'UTC') and in timestamptz AT TIME ZONE 'UTC' is the abbreviation of the session's time zone when that zone
-- has one of the name (the POSIX zone UTC+3 is -03 with the abbreviation UTC): the value is not the UTC clock's there. The scan must give the rows of the normal plan in
-- every session, with the calendar cells of the units and without them, and from a plan that was made in another zone. The names with a slash (Etc/UTC) name the database zone.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;

CREATE TABLE uz (id int, ts timestamptz, v int) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO uz SELECT i, timestamptz '2024-01-01 00:00+00' + i * interval '97 minutes 13 seconds', i % 13 FROM generate_series(1, 6000) i;
INSERT INTO uz VALUES (7001, 'infinity', 1), (7002, '-infinity', 2), (7003, NULL, 3), (7004, '2024-05-01 02:30+00', 4), (7005, '2024-05-01 21:30+00', 5);
CREATE INDEX uz_col ON uz USING sift_col (id, ts, v);
VACUUM ANALYZE uz;

CREATE TABLE zones (z text);
INSERT INTO zones VALUES ('UTC'), ('America/New_York'), ('Asia/Kolkata'), ('UTC+3'), ('UTC-5'), ('GMT+3'), ('GMT-11'), ('utc+1');
CREATE TABLE exprs (n int, e text);
INSERT INTO exprs VALUES
	(1, 'date_trunc(''day'', ts, ''UTC'')'), (2, 'date_trunc(''hour'', ts, ''GMT'')'), (3, 'date_trunc(''month'', ts AT TIME ZONE ''UTC'')'), (4, 'date_trunc(''day'', ts AT TIME ZONE ''GMT'')'),
	(5, '(ts AT TIME ZONE ''UTC'')::date'), (6, 'extract(hour from ts AT TIME ZONE ''UTC'')'), (7, 'extract(dow from ts AT TIME ZONE ''utc'')'),
	(8, 'date_trunc(''day'', ts, ''Etc/UTC'')'), (9, 'date_trunc(''day'', ts AT TIME ZONE ''Etc/UTC'')'), (10, 'date_trunc(''day'', ts AT TIME ZONE ''Asia/Tokyo'')'),
	-- keys of the session zone's clock next to the name (the zone's steps must not leak into a shadowed name's)
	(11, 'date_trunc(''day'', ts)'), (12, 'date_trunc(''hour'', ts)'), (13, 'extract(hour from ts)'), (14, 'date_trunc(''day'', date_trunc(''hour'', ts) AT TIME ZONE ''UTC'')'),
	(15, 'date_trunc(''hour'', date_trunc(''day'', ts, ''GMT''))'),
	(16, 'date_trunc(''day'', date_trunc(''day'', ts) AT TIME ZONE ''UTC'')'), (17, 'date_trunc(''month'', date_trunc(''day'', ts) AT TIME ZONE ''GMT'')');

-- the checksum of the groups of an expression in zone z: the scan (cells on or off), and the normal plan
CREATE FUNCTION run(e text, z text, scan bool, cells bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('TimeZone', z, true);
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('sift.agg_force', scan::text, true);
	PERFORM set_config('sift.agg_preagg', cells::text, true);
	EXECUTE format('SELECT md5(string_agg(row(k, n, s)::text, E''\n'' ORDER BY k)) FROM (SELECT %s AS k, count(*) AS n, sum(v) AS s FROM uz GROUP BY 1) q', e) INTO r;
	RETURN r;
END $$;

SELECT n, e, (SELECT count(*) FROM zones WHERE run(x.e, z, true, true) IS DISTINCT FROM run(x.e, z, false, true)) AS cells_differ,
       (SELECT count(*) FROM zones WHERE run(x.e, z, true, false) IS DISTINCT FROM run(x.e, z, false, false)) AS scan_differ
FROM exprs x ORDER BY n;

-- a key of the session zone's clock and a key that names UTC in one grouping
CREATE FUNCTION run2(z text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('TimeZone', z, true);
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('sift.agg_force', scan::text, true);
	SELECT md5(string_agg(row(a, b, n)::text, E'\n' ORDER BY a, b)) INTO r FROM
		(SELECT date_trunc('day', ts) AS a, date_trunc('day', ts, 'UTC') AS b, count(*) AS n FROM uz GROUP BY 1, 2) q;
	RETURN r;
END $$;
SELECT count(*) AS two_keys_differ FROM zones WHERE run2(z, true) IS DISTINCT FROM run2(z, false);

-- the scan still takes the UTC clock's days where the name is the UTC (the zone UTC, a zone whose GMT is the UTC, a database zone) and not where the session zone redefines it
CREATE FUNCTION per_day(z text, e text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text; r boolean := false;
BEGIN
	PERFORM set_config('TimeZone', z, true);
	PERFORM set_config('sift.agg_force', 'true', true);
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT %s AS k, count(*) FROM uz GROUP BY 1', e) LOOP
		r := r OR l LIKE '%(per day)%';
	END LOOP;
	RETURN r;
END $$;
-- (from PostgreSQL 18 a bare name is read as the session zone's abbreviation first; before it the abbreviations table alone, the name is the UTC in every zone)
SELECT z, per_day(z, 'date_trunc(''day'', ts AT TIME ZONE ''UTC'')') = (z <> 'UTC+3' OR current_setting('server_version_num')::int < 180000) AS utc_name,
       per_day(z, 'date_trunc(''day'', ts, ''GMT'')') = (z <> 'GMT-11' OR current_setting('server_version_num')::int < 180000) AS gmt_name,
       per_day(z, 'date_trunc(''day'', ts AT TIME ZONE ''Etc/UTC'')') AS slash_name
FROM unnest(ARRAY['UTC', 'Europe/London', 'America/New_York', 'UTC+3', 'GMT-11']) WITH ORDINALITY u(z, i) ORDER BY i;

-- a plan made in the zone UTC runs in a zone that names UTC
SET plan_cache_mode = force_generic_plan;
CREATE FUNCTION by_day_scan(z text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_force', 'true', true);
	PERFORM set_config('TimeZone', z, true);
	SELECT md5(string_agg(row(k, n)::text, ',' ORDER BY k)) INTO r FROM (SELECT date_trunc('day', ts AT TIME ZONE 'UTC') AS k, count(*) AS n FROM uz GROUP BY 1) q;
	RETURN r;
END $$;
CREATE FUNCTION by_day_native(z text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'false', true);
	PERFORM set_config('TimeZone', z, true);
	SELECT md5(string_agg(row(k, n)::text, ',' ORDER BY k)) INTO r FROM (SELECT date_trunc('day', ts AT TIME ZONE 'UTC') AS k, count(*) AS n FROM uz GROUP BY 1) q;
	RETURN r;
END $$;
SELECT z, by_day_scan(z) = by_day_native(z) AS same FROM unnest(ARRAY['UTC', 'UTC+3', 'America/New_York', 'GMT-11', 'UTC']) WITH ORDINALITY u(z, i) ORDER BY i;
-- the same over a call of the session zone's clock: its steps are the zone's, the name's are not (the reference is planned afresh by EXECUTE, the scan is the cached plan)
CREATE FUNCTION by_zone_day(z text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_force', 'true', true);
	PERFORM set_config('TimeZone', z, true);
	IF scan THEN
		SELECT md5(string_agg(row(k, n)::text, ',' ORDER BY k)) INTO r FROM (SELECT date_trunc('day', date_trunc('day', ts) AT TIME ZONE 'UTC') AS k, count(*) AS n FROM uz GROUP BY 1) q;
	ELSE
		PERFORM set_config('sift.enable_agg', 'false', true);
		EXECUTE 'SELECT md5(string_agg(row(k, n)::text, '','' ORDER BY k)) FROM (SELECT date_trunc(''day'', date_trunc(''day'', ts) AT TIME ZONE ''UTC'') AS k, count(*) AS n FROM uz GROUP BY 1) q' INTO r;
	END IF;
	RETURN r;
END $$;
SELECT z, by_zone_day(z, true) = by_zone_day(z, false) AS same FROM unnest(ARRAY['America/New_York', 'UTC+3', 'GMT-11', 'UTC']) WITH ORDINALITY u(z, i) ORDER BY i;
RESET plan_cache_mode;

DROP FUNCTION run(text, text, bool, bool), run2(text, bool), per_day(text, text), by_day_scan(text), by_day_native(text), by_zone_day(text, bool);
DROP TABLE uz, zones, exprs;
DROP EXTENSION pg_sift;
