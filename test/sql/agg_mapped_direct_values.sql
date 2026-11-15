-- min, max, sum and avg of a column of a joined table (the lookup of a join) take the value of each tuple as it comes, with no set of the distinct values of each group:
-- the rows are those of the normal plan for integer, date, timestamp and float8 columns, NULL values, children whose parent is gone or has no key, groups with no tuple, ties, and a NaN and
-- an infinity among the floats.
CREATE EXTENSION pg_sift;

SET client_min_messages = warning;
SET sift.agg_preagg = off;

CREATE TABLE mv_parent (id int PRIMARY KEY, grp int, i2 int2, i4 int4, i8 int8, d date, ts timestamp, tstz timestamptz, f8 float8, nn int4) WITH (autovacuum_enabled = off);
INSERT INTO mv_parent
SELECT g, g % 7, CASE WHEN g % 11 = 0 THEN NULL ELSE (g % 50 - 25)::int2 END, CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 97 - 40) * 1000 END,
       CASE WHEN g % 17 = 0 THEN NULL ELSE (g % 31)::int8 * 1000000000 - 5000000000 END, date '2020-01-01' + g % 400,
       timestamp '2020-01-01' + (g % 500) * interval '1 hour', timestamptz '2020-01-01 00:00:00+00' + (g % 300) * interval '1 day',
       CASE WHEN g = 7 THEN 'NaN'::float8 WHEN g = 9 THEN 'Infinity'::float8 WHEN g % 19 = 0 THEN NULL ELSE (g % 40)::float8 / 2 - 5 END, g
FROM generate_series(1, 2000) g;
CREATE TABLE mv_child (id int PRIMARY KEY, parent_id int, g int, at timestamp) WITH (autovacuum_enabled = off);
INSERT INTO mv_child
SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 53 = 0 THEN 3000 + i % 100 ELSE 1 + (i::bigint * 7919) % 2000 END, i % 5, timestamp '2024-01-01' + (i % 300) * interval '1 day'
FROM generate_series(1, 100000) i;
CREATE INDEX mv_parent_col ON mv_parent USING sift_col (id, grp, i2, i4, i8, d, ts, tstz, f8, nn);
CREATE INDEX mv_child_col ON mv_child USING sift_col (id, parent_id, g, at);
VACUUM (FREEZE, ANALYZE) mv_parent, mv_child;

CREATE TABLE mv_q (slug text, sql text);
INSERT INTO mv_q VALUES
 ('inner_join_min_max_integers', 'SELECT c.g, min(p.i2), max(p.i2), min(p.i4), max(p.i4), min(p.i8), max(p.i8) FROM mv_child c JOIN mv_parent p ON p.id = c.parent_id GROUP BY c.g'),
 ('inner_join_min_max_date_timestamps', 'SELECT c.g, min(p.d), max(p.d), min(p.ts), max(p.ts), min(p.tstz), max(p.tstz) FROM mv_child c JOIN mv_parent p ON p.id = c.parent_id GROUP BY c.g'),
 ('inner_join_min_max_float8', 'SELECT c.g, min(p.f8), max(p.f8) FROM mv_child c JOIN mv_parent p ON p.id = c.parent_id GROUP BY c.g'),
 ('inner_join_sum_avg_integers', 'SELECT c.g, sum(p.i2), sum(p.i4), sum(p.i8), avg(p.i2), avg(p.i4), avg(p.i8) FROM mv_child c JOIN mv_parent p ON p.id = c.parent_id GROUP BY c.g'),
 ('inner_join_sum_avg_float8', 'SELECT c.g, sum(p.f8), avg(p.f8), sum(p.f8) FILTER (WHERE p.grp = 3) FROM mv_child c JOIN mv_parent p ON p.id = c.parent_id GROUP BY c.g'),
 ('left_join_orphans_min_max_sum', 'SELECT c.g, min(p.i4), max(p.f8), sum(p.i4), sum(p.nn), count(p.i4), count(*) FROM mv_child c LEFT JOIN mv_parent p ON p.id = c.parent_id GROUP BY c.g'),
 ('left_join_filtered_children', 'SELECT c.g, min(p.d), sum(p.i2), max(p.i8), avg(p.i4) FROM mv_child c LEFT JOIN mv_parent p ON p.id = c.parent_id WHERE c.at >= timestamp ''2024-06-01'' AND c.id % 3 = 0 GROUP BY c.g'),
 ('group_by_parent_column_min_max', 'SELECT p.grp, min(p.i4), max(p.f8), sum(p.i2), count(*) FROM mv_child c JOIN mv_parent p ON p.id = c.parent_id GROUP BY p.grp'),
 ('no_child_kept_min_max_sum', 'SELECT min(p.i4), max(p.f8), sum(p.i2), avg(p.f8) FROM mv_child c JOIN mv_parent p ON p.id = c.parent_id WHERE c.g > 100');

CREATE FUNCTION mv_answer(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE 'SELECT coalesce(string_agg(t::text, '';'' ORDER BY t::text), '''') FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION mv_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; r bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r OR l ~ 'Custom Scan \(SiftAgg\)';
	END LOOP;
	RETURN r;
END $$;

SELECT slug, mv_scan(sql) AS scan, mv_answer(sql, true) = mv_answer(sql, false) AS same_answers FROM mv_q ORDER BY slug;

DROP FUNCTION mv_answer(text, bool), mv_scan(text);
DROP TABLE mv_q, mv_child, mv_parent;
DROP EXTENSION pg_sift;
