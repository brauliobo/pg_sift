-- The rows a unit keeps for the ranges of an equality of a periodic calendar step (extract(month|doy|hour FROM x), to_char) are not tested by the expression again. The rows are those of the
-- normal plan over rows the source does not hold as a unit: a write buffer, deleted and updated rows, a NULL, and then an infinite timestamp in the write buffer.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE pd AS SELECT g AS id, timestamp '2024-01-01' + g * interval '13 minutes' AS ts FROM generate_series(1, 150000) g;
CREATE INDEX pd_col ON pd USING sift_col (id, ts);
VACUUM (FREEZE, ANALYZE) pd;
INSERT INTO pd SELECT 100000 + g, timestamp '2024-03-01' + g * interval '1 hour' FROM generate_series(1, 2000) g;
INSERT INTO pd VALUES (200000, NULL);
DELETE FROM pd WHERE id % 7 = 0;
UPDATE pd SET ts = ts + interval '40 days' WHERE id % 11 = 0;
CREATE FUNCTION count_of(q text, scan bool) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SET LOCAL sift.enable_agg = %s', CASE WHEN scan THEN 'on' ELSE 'off' END);
	EXECUTE q INTO n;
	RETURN n;
END $$;
CREATE FUNCTION narrowed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; s bool := false;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		s := s OR l LIKE '%Filters In Source: %';
	END LOOP;
	RETURN s;
END $$;
CREATE TABLE pd_q (w text);
INSERT INTO pd_q VALUES ('extract(month FROM ts) = 3'), ('extract(month FROM ts) = 4'), ('extract(month FROM ts) = 13'), ('extract(doy FROM ts) = 100'), ('extract(hour FROM ts) = 5'),
	('to_char(ts, ''YYYY-MM'') = ''2024-03'''), ('to_char(ts, ''YYYY-MM-DD'') = ''2024-03-05'''), ('to_char(ts, ''YYYY-MM-DD HH24'') = ''2024-03-05 01''');
SELECT w, count_of('SELECT count(*) FROM pd WHERE ' || w, true) = count_of('SELECT count(*) FROM pd WHERE ' || w, false) AS same, narrowed('SELECT count(*) FROM pd WHERE ' || w) AS narrowed FROM pd_q ORDER BY w;
INSERT INTO pd VALUES (200001, 'infinity');
SELECT w, count_of('SELECT count(*) FROM pd WHERE ' || w, true) = count_of('SELECT count(*) FROM pd WHERE ' || w, false) AS same, narrowed('SELECT count(*) FROM pd WHERE ' || w) AS narrowed FROM pd_q ORDER BY w;
DROP FUNCTION count_of(text, bool);
DROP FUNCTION narrowed(text);
DROP TABLE pd_q, pd;
DROP EXTENSION pg_sift;
