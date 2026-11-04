-- Each partition a scan reads is opened, its first unit decoded: 39 to 51 us measured, 17 units, for each (count(*) of 20,000 rows over 16 and 64 partitions 0.70 and 2.56 ms against
-- 1.79 and 2.0 ms for the normal plan). 64 partitions of 300 rows are a normal plan's (the Append of their sequential scans), 4 of 50,000 rows the scan's; the rows are the same either way.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE pp_few (id int NOT NULL, kind text NOT NULL, ts date NOT NULL, amount float8) PARTITION BY RANGE (ts);
CREATE TABLE pp_many (id int NOT NULL, kind text NOT NULL, ts date NOT NULL, amount float8) PARTITION BY RANGE (ts);
SELECT format('CREATE TABLE pp_few_%s PARTITION OF pp_few FOR VALUES FROM (%L) TO (%L)', i, date '2020-01-01' + (i - 1) * 100, date '2020-01-01' + i * 100) FROM generate_series(1, 4) i \gexec
SELECT format('CREATE TABLE pp_many_%s PARTITION OF pp_many FOR VALUES FROM (%L) TO (%L)', i, date '2020-01-01' + (i - 1) * 5, date '2020-01-01' + i * 5) FROM generate_series(1, 64) i \gexec
INSERT INTO pp_few SELECT i, (ARRAY['a', 'b', 'c'])[1 + i % 3], date '2020-01-01' + i % 400, i % 97 FROM generate_series(1, 200000) i;
INSERT INTO pp_many SELECT i, (ARRAY['a', 'b', 'c'])[1 + i % 3], date '2020-01-01' + i % 320, i % 97 FROM generate_series(1, 20000) i;
SELECT format('CREATE INDEX %s_col ON %s USING sift_col (id, kind, ts, amount)', c.relname, c.relname) FROM pg_class c WHERE c.relname ~ '^pp_(few|many)_[0-9]+$' ORDER BY 1 \gexec
VACUUM ANALYZE pp_few, pp_many;

CREATE FUNCTION pp_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'scan'; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION pp_same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (%s) x', q) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (%s) x', q) INTO b;
	SET LOCAL sift.enable_agg = on;
	RETURN a = b;
END $$;

SELECT t, pp_plan(q) AS plan, pp_same(q) AS rows_same FROM (VALUES
	('4 partitions of 50,000 rows: count', 'SELECT count(*) FROM pp_few'),
	('4 partitions of 50,000 rows: group', 'SELECT kind, count(*) FROM pp_few GROUP BY 1'),
	('64 partitions of 300 rows: count', 'SELECT count(*) FROM pp_many'),
	('64 partitions of 300 rows: group', 'SELECT kind, count(*) FROM pp_many GROUP BY 1')) v(t, q) ORDER BY 1;

DROP TABLE pp_few, pp_many;
DROP FUNCTION pp_plan(text), pp_same(text);
DROP EXTENSION pg_sift;
