-- A group key that is an expression of the key of an unnested jsonb map (split_part(key, '_', 1)), grouped by grouping sets below a left join, reaches the
-- planner as a placeholder: PostgreSQL 18 marks the grouping step with a group RTE, before 18 the grouping sets expression is wrapped in a placeholder that
-- carries no nulling of its own. The scan reads the expression in both (the arm single is used) and the rows equal those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE gs_events (id int PRIMARY KEY, dim_a_id int NOT NULL, d date NOT NULL, metrics_a jsonb NOT NULL);
INSERT INTO gs_events
SELECT g, 1 + g % 40, date '2025-01-01' + g % 60, jsonb_build_object('total', g % 10, 'grade_a', g % 7, 'grade', g % 5, 'size_b', g % 3)
FROM generate_series(1, 20000) g;
CREATE INDEX gs_events_col ON gs_events USING sift_col (id, dim_a_id, d, metrics_a) WITH (paths = 'metrics_a.* float8');
VACUUM (FREEZE, ANALYZE) gs_events;

CREATE FUNCTION gs_query(entity int) RETURNS text LANGUAGE sql AS $f$
	SELECT format($q$WITH days AS (SELECT DISTINCT d AS date FROM gs_events WHERE d >= DATE '2025-01-01'),
	parsed AS (SELECT e.id, e.dim_a_id, e.d, s.category, s.value_num, (s.value #>> '{}')::float AS score FROM gs_events e
	           INNER JOIN LATERAL (SELECT split_part(key, '_', 1) AS category, nullif(split_part(key, '_', 2), '') AS value_num, value FROM jsonb_each(e.metrics_a)) s ON true
	           WHERE e.dim_a_id = %s)
SELECT coalesce(category, 'total') AS category, value_num, date, avg(score) AS value, count(DISTINCT id) AS n
FROM (SELECT p.*, days.date AS date FROM days LEFT JOIN parsed p ON p.d = days.date) x
GROUP BY GROUPING SETS ((category, value_num, date), (category, value_num))$q$, entity) $f$;

CREATE FUNCTION gs_rows(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN scan THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT string_agg(t::text, E''\n'' ORDER BY t::text) FROM (%s) t', q) INTO r;
	RETURN r;
END $$;

SELECT bool_or(used) AS single_used FROM sift.explain_why(gs_query(7)) WHERE arm = 'single';
SELECT gs_rows(gs_query(7), true) = gs_rows(gs_query(7), false) AS same_rows, length(gs_rows(gs_query(7), false)) > 0 AS has_rows;
DROP FUNCTION gs_rows, gs_query;
DROP TABLE gs_events;
DROP EXTENSION pg_sift;
