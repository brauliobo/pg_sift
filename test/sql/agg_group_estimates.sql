-- The groups the planner expects of a calendar key on a date column come from the days the column takes (the index's range, no more than the days the
-- conditions on it leave), not from PostgreSQL's count of distinct dates: a month, a year or a week of a date is a few groups, not as many as the dates.
-- Each statement is planned as a SiftAgg scan; its estimated rows must be within a factor of two of the groups it returns.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ge_rows (id int PRIMARY KEY, entity_id int NOT NULL, day date NOT NULL, val int NOT NULL) WITH (autovacuum_enabled = false);
-- 700 days, a thousand rows a day on average
INSERT INTO ge_rows SELECT i, 1 + i % 50, date '2025-01-01' + (hashint4(i) & 1023) % 700, i % 10 FROM generate_series(1, 100000) i;
CREATE INDEX ge_rows_col ON ge_rows USING sift_col (id, entity_id, day, val);
VACUUM (FREEZE, ANALYZE) ge_rows;

CREATE FUNCTION est_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN substring(l FROM 'rows=(\d+)')::bigint; END IF;
	END LOOP;
	RETURN NULL;
END $$;

CREATE FUNCTION groups_of(q text) RETURNS TABLE (planned_rows_ok bool, groups bigint) LANGUAGE plpgsql AS $$
DECLARE e bigint;
BEGIN
	e := est_rows(q);
	EXECUTE format('SELECT count(*) FROM (%s) x', q) INTO groups;
	planned_rows_ok := e IS NOT NULL AND e BETWEEN groups / 2.0 AND groups * 2.0;
	RETURN NEXT;
END $$;

SELECT k AS key, (groups_of(format('SELECT %s, count(*) FROM ge_rows GROUP BY 1', k))).*
FROM (VALUES ('date_trunc(''year'', day)'), ('date_trunc(''quarter'', day)'), ('date_trunc(''month'', day)'), ('date_trunc(''week'', day)'),
	('extract(year FROM day)'), ('extract(month FROM day)'), ('extract(isodow FROM day)'), ('date_trunc(''month'', day)::date')) v(k);

-- the conditions on the date bound the days: a quarter holds three months and about thirteen weeks
SELECT k AS key, (groups_of(format('SELECT %s, count(*) FROM ge_rows WHERE day >= %L AND day < %L GROUP BY 1', k, '2025-04-01', '2025-07-01'))).*
FROM (VALUES ('date_trunc(''month'', day)'), ('date_trunc(''week'', day)'), ('extract(month FROM day)')) v(k);

-- a column of timestamps that was never analyzed has no histogram: the index's smallest and largest value give the span of the buckets
CREATE TABLE ge_ts (id int PRIMARY KEY, ts timestamptz NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ge_ts SELECT i, timestamptz '2025-01-01' + (hashint4(i) & 1023) % 700 * interval '1 day' + i % 86400 * interval '1 second' FROM generate_series(1, 100000) i;
CREATE INDEX ge_ts_col ON ge_ts USING sift_col (id, ts);
VACUUM (FREEZE) ge_ts;
SELECT k AS key, (groups_of(format('SELECT %s, count(*) FROM ge_ts GROUP BY 1', k))).*
FROM (VALUES ('date_trunc(''month'', ts AT TIME ZONE ''UTC'')'), ('date_trunc(''week'', ts AT TIME ZONE ''UTC'')')) v(k);

DROP FUNCTION groups_of(text), est_rows(text);
DROP TABLE ge_rows, ge_ts;
DROP EXTENSION pg_sift;
