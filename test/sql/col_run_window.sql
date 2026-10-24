-- A range on a leaf with a sorted run (order_columns) in a unit is answered by two binary searches on the run, and the chunk is decoded at the rows of the window,
-- not tested at every row (EXPLAIN: Units Filtered By Sorted Run). The answers are those of the heap and of the scan without the pushdown, for every case the search can
-- get wrong: a window at the start, the middle and the end of the run (the entries are read from the window, not from the first), keys of 64 bits, a run that is the
-- identity or its reverse, NULL rows, ties, negative keys, an empty range, three bounds on one leaf; and the cases where the run is not used and the plain test
-- runs: rows that overflowed (a numeric NaN), a window of more than an eighth of the unit (or a range of more than a quarter of the zone, which is not tried), one bound, a unit of fewer than 1,024 rows; and after deletes,
-- updates and inserts (dead rows of the units, the write buffer).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;

-- ts: shuffled over two years; tsa ascends with id and tsd descends (the identity run and its reverse); kn: ties and NULLs; n: a numeric with NaN in every unit
-- (stored as an overflow); neg: negative keys; d: a date
CREATE TABLE rw (id int, ts timestamp, tsa timestamp, tsd timestamp, kn int, n numeric(12, 2), neg int, d date, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE rw ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO rw
SELECT g, timestamp '2024-01-01' + ((hashint4(g) & 2147483647) % 1051200) * interval '1 minute',
       timestamp '2024-01-01' + g * interval '10 minutes', timestamp '2030-01-01' - g * interval '10 minutes',
       CASE WHEN g % 7 = 0 THEN NULL ELSE (hashint4(g + 1) & 2147483647) % 400 END,
       CASE WHEN g % 97 = 0 THEN 'NaN'::numeric ELSE ((hashint4(g + 2) & 2147483647) % 100000) / 100.0 END,
       (hashint4(g + 3) & 2147483647) % 100000 - 50000, date '2020-01-01' + (hashint4(g + 4) & 2147483647) % 2000, repeat('x', 200)
FROM generate_series(1, 40000) g;
CREATE INDEX rw_col ON rw USING sift_col (id, ts, tsa, tsd, kn, n, neg, d)
	WITH (order_columns = 'ts, tsa, tsd, kn, n, neg, d', cube_minmax = false, superunit = 0, calendar_columns = 'off');
-- a table of one unit of 600 rows
CREATE TABLE rs AS SELECT id, ts, kn FROM rw WHERE id <= 600;
CREATE INDEX rs_col ON rs USING sift_col (id, ts, kn) WITH (order_columns = 'ts, kn', cube_minmax = false, superunit = 0, calendar_columns = 'off');
VACUUM (ANALYZE, FREEZE) rw;
VACUUM (ANALYZE, FREEZE) rs;
SET sift.enable_col_scan = on;

-- the units that took their rows from a run
CREATE FUNCTION rw_windows(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || q LOOP
		IF l ~ 'Units Filtered By Sorted Run' THEN RETURN regexp_replace(l, '\D', '', 'g')::int; END IF;
	END LOOP;
	RETURN 0;
END $$;
-- the answer of the query as md5 under the pushdown, under the index scan alone and under neither (the heap)
CREATE FUNCTION rw_answers(q text, agg bool, col bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text;
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, false);
	PERFORM set_config('sift.enable_col_scan', col::text, false);
	PERFORM set_config('enable_indexonlyscan', col::text, false);
	PERFORM set_config('enable_indexscan', col::text, false);
	PERFORM set_config('enable_bitmapscan', col::text, false);
	EXECUTE format('SELECT md5(coalesce(string_agg(x::text, ''|'' ORDER BY x::text), '''')) FROM (%s) x', q) INTO a;
	PERFORM set_config('sift.enable_agg', 'on', false);
	PERFORM set_config('sift.enable_col_scan', 'on', false);
	PERFORM set_config('enable_indexonlyscan', 'on', false);
	PERFORM set_config('enable_indexscan', 'on', false);
	PERFORM set_config('enable_bitmapscan', 'on', false);
	RETURN a;
END $$;
CREATE FUNCTION rw_same(w text, t text DEFAULT 'rw') RETURNS bool LANGUAGE plpgsql AS $$
DECLARE r text := rw_answers('SELECT id FROM ' || t || ' WHERE ' || w, false, false);
BEGIN
	RETURN r = rw_answers('SELECT id FROM ' || t || ' WHERE ' || w, false, true)
		AND r = rw_answers('SELECT id FROM ' || t || ' WHERE ' || w, true, true)
		AND rw_answers('SELECT count(*), sum(id) FROM ' || t || ' WHERE ' || w, false, false) = rw_answers('SELECT count(*), sum(id) FROM ' || t || ' WHERE ' || w, true, true);
END $$;

CREATE TABLE rw_case (n serial, name text, w text, t text DEFAULT 'rw');
INSERT INTO rw_case (name, w) VALUES
	('ts start',            'ts >= timestamp ''2024-01-01'' AND ts < timestamp ''2024-01-06'''),
	('ts middle',           'ts >= timestamp ''2024-06-01'' AND ts < timestamp ''2024-06-11'''),
	('ts end',              'ts >= timestamp ''2025-12-20'' AND ts < timestamp ''2026-01-01'''),
	('ts between',          'ts BETWEEN timestamp ''2024-09-01'' AND timestamp ''2024-09-04'''),
	('ts one second',       'ts >= timestamp ''2024-06-01'' AND ts < timestamp ''2024-06-01 00:00:01'''),
	('identity run',        'tsa >= timestamp ''2024-03-01'' AND tsa < timestamp ''2024-03-05'''),
	('reverse run',         'tsd >= timestamp ''2029-06-01'' AND tsd < timestamp ''2029-06-04'''),
	('NULLs and ties',      'kn >= 100 AND kn <= 102'),
	('equality on ties',    'kn = 5'),
	('negative keys',       'neg >= -100 AND neg < 100'),
	('date',                'd >= date ''2022-02-01'' AND d < date ''2022-02-10'''),
	('three bounds',        'ts >= timestamp ''2024-06-01'' AND ts > timestamp ''2024-06-02'' AND ts < timestamp ''2024-06-11'''),
	('window and equality', 'ts >= timestamp ''2024-06-01'' AND ts < timestamp ''2024-06-11'' AND kn < 200'),
	('empty range',         'ts >= timestamp ''2024-06-11'' AND ts < timestamp ''2024-06-01'''),
	('beyond the data',     'ts >= timestamp ''2031-01-01'' AND ts < timestamp ''2031-02-01'''),
	('below an eighth',     'ts >= timestamp ''2024-01-01'' AND ts < timestamp ''2024-03-15'''),
	('above an eighth',     'ts >= timestamp ''2024-01-01'' AND ts < timestamp ''2024-04-20'''),
	('over a quarter',      'ts >= timestamp ''2024-01-01'' AND ts < timestamp ''2024-09-01'''),
	('wide date',           'd >= date ''2020-01-01'' AND d < date ''2021-01-01'''),
	('overflow rows',       'n >= 10 AND n < 10.5'),
	('one bound',           'ts < timestamp ''2024-01-06'''),
	('one unit of 600',     'ts >= timestamp ''2024-06-01'' AND ts < timestamp ''2024-09-01''');
UPDATE rw_case SET t = 'rs' WHERE name = 'one unit of 600';

CREATE FUNCTION rw_count(w text, t text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', false);
	PERFORM set_config('sift.enable_col_scan', 'off', false);
	EXECUTE 'SELECT count(*) FROM ' || t || ' WHERE ' || w INTO n;
	PERFORM set_config('sift.enable_agg', 'on', false);
	PERFORM set_config('sift.enable_col_scan', 'on', false);
	RETURN n;
END $$;

-- from_run: some units took their rows from the run; same: the answers of the pushdown, of the index scan and of the heap agree
SELECT n, name, rw_count(w, t) AS rows, rw_windows('SELECT count(*) FROM ' || t || ' WHERE ' || w) > 0 AS from_run, rw_same(w, t) AS same FROM rw_case ORDER BY n;

-- the same after deletes (dead rows in the window), updates (rows that leave and enter it) and inserts (the write buffer)
DELETE FROM rw WHERE id % 50 = 0;
UPDATE rw SET ts = ts + interval '30 days' WHERE id % 61 = 0;
UPDATE rw SET ts = timestamp '2024-06-05 12:00' WHERE id % 997 = 3;
INSERT INTO rw SELECT g + 100000, timestamp '2024-06-02' + g * interval '1 hour', timestamp '2024-03-02' + g * interval '1 hour', timestamp '2029-06-02' + g * interval '1 hour',
       g % 400, g, g - 150, date '2022-02-02', repeat('x', 200) FROM generate_series(1, 150) g;
SELECT n, name, rw_count(w, t) AS rows, rw_windows('SELECT count(*) FROM ' || t || ' WHERE ' || w) > 0 AS from_run, rw_same(w, t) AS same FROM rw_case ORDER BY n;
-- the units merged again
SELECT sift.col_merge('rw_col', true) IS NOT NULL AS merged;
SELECT n, name, rw_count(w, t) AS rows, rw_windows('SELECT count(*) FROM ' || t || ' WHERE ' || w) > 0 AS from_run, rw_same(w, t) AS same FROM rw_case ORDER BY n;

DROP FUNCTION rw_windows(text), rw_answers(text, bool, bool), rw_same(text, text), rw_count(text, text);
DROP TABLE rw_case, rs, rw;
DROP EXTENSION pg_sift;
