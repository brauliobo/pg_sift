-- A timestamptz column against a timestamp constant: PostgreSQL reads the timestamp in the session's zone, and so do the predicates the scan tests.
-- Each query must return the rows of a heap copy, with and without the aggregate scan. The rows sit around midnight UTC and midnight of the zone, three hours apart.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SET timezone = 'America/Bahia';

CREATE TABLE tvt (id int, at timestamptz NOT NULL, v int NOT NULL) USING sift_table;
INSERT INTO tvt SELECT i, timestamptz '2025-11-01 00:00:00+00' + i * interval '7 minutes', i FROM generate_series(1, 20000) i;
CREATE TABLE tvt_heap AS SELECT * FROM tvt;
VACUUM (FREEZE, ANALYZE) tvt, tvt_heap;

CREATE FUNCTION tvt_rows(q text, on_ bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', on_::text, true);
	EXECUTE q INTO r;
	RETURN r;
END $$;

CREATE TABLE tvt_q (q text);
INSERT INTO tvt_q VALUES
	('SELECT count(*)::text || '' '' || sum(v) FROM tvt WHERE at >= timestamp ''2025-12-01'''),
	('SELECT count(*)::text || '' '' || sum(v) FROM tvt WHERE at < timestamp ''2025-12-01 01:30'''),
	('SELECT count(*)::text || '' '' || sum(v) FROM tvt WHERE at >= date ''2026-03-01'' - interval ''90 days'' AND at < date ''2025-12-02''');

CREATE FUNCTION tvt_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; r bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP r := r OR l ~ 'Custom Scan \(SiftAgg'; END LOOP;
	RETURN r;
END $$;

SELECT q, tvt_pushed(q) AS pushed, tvt_rows(q, true) = tvt_rows(replace(q, 'FROM tvt ', 'FROM tvt_heap '), false) AS same_on,
	tvt_rows(q, false) = tvt_rows(replace(q, 'FROM tvt ', 'FROM tvt_heap '), false) AS same_off FROM tvt_q ORDER BY q COLLATE "C";

DROP FUNCTION tvt_rows(text, bool), tvt_pushed(text);
DROP TABLE tvt, tvt_heap, tvt_q;
DROP EXTENSION pg_sift;
