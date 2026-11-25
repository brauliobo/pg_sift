-- count(x) of a column the table declares NOT NULL is count(*) for the scan, whether or not the sift_col index holds the column: a primary key outside the index
-- no longer keeps the grouped count of a statement in the normal plan. A column that may be NULL outside the index, and a column on the nullable side of an outer join
-- (NULL for a row the join pads), are counted as before. Every statement gives the rows of the plan without the extension.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, kind text NOT NULL, amt int, flag int NOT NULL, note int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 50, (ARRAY['a', 'b', 'c'])[1 + i % 3], CASE WHEN i % 7 <> 0 THEN i % 100 END, i % 2, CASE WHEN i % 5 <> 0 THEN i END FROM generate_series(1, 30000) i;
CREATE TABLE dim_a (id int PRIMARY KEY);
INSERT INTO dim_a SELECT generate_series(1, 60);
CREATE INDEX events_col ON events USING sift_col (dim_a_id, kind, amt);
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) dim_a;

\ir test/agg_helpers.sql
-- in_scan: the plan has a Custom Scan (SiftAgg); same: the rows are the plan's without the extension
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, in_scan bool, same bool, rows bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT name, pushed(q), runq(q, true) = runq(q, false), (SELECT count(*) FROM jsonb_array_elements(runq(q, true)::jsonb));
END $$;

SELECT * FROM check_query('count(*)', $q$ SELECT dim_a_id, kind, count(*) AS n FROM events GROUP BY 1, 2 $q$);
SELECT * FROM check_query('count of the primary key outside the index', $q$ SELECT dim_a_id, kind, count(id) AS n FROM events GROUP BY 1, 2 $q$);
SELECT * FROM check_query('count of a NOT NULL column outside the index, with a sum', $q$ SELECT dim_a_id, count(flag) AS n, sum(amt) AS s FROM events WHERE kind = 'a' GROUP BY 1 $q$);
SELECT * FROM check_query('count FILTER', $q$ SELECT dim_a_id, count(id) FILTER (WHERE amt > 50) AS n, count(*) AS m FROM events GROUP BY 1 $q$);
SELECT * FROM check_query('count of a column the index holds', $q$ SELECT dim_a_id, count(amt) AS n FROM events GROUP BY 1 $q$);
SELECT * FROM check_query('a nullable column outside the index', $q$ SELECT dim_a_id, count(note) AS n FROM events GROUP BY 1 $q$);
SELECT * FROM check_query('the NOT NULL column of the nullable side of a left join', $q$
 SELECT d.id, count(e.id) AS n FROM dim_a d LEFT JOIN events e ON e.dim_a_id = d.id AND e.kind = 'a' GROUP BY 1 $q$);
SELECT * FROM check_query('count DISTINCT of the primary key', $q$ SELECT dim_a_id, count(DISTINCT id) AS n FROM events GROUP BY 1 $q$);

DROP TABLE events, dim_a;
DROP FUNCTION check_query(text,text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
DROP EXTENSION pg_sift;
