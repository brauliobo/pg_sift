-- A join of a column with the ranges of a list (`event_date <@ periods.range`) is estimated by PostgreSQL at a constant 0.001 of the product of the two relations,
-- whatever the ranges hold: two ranges over a table of 30,000 rows are 60 rows where thousands come out, and the plans above are priced for 60. With the ranges
-- constants the library takes the selectivity of the join from the statistics of the column, the mean over the ranges of the share of the rows they take.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, set_id int NOT NULL, event_date date NOT NULL, pad text NOT NULL DEFAULT repeat('x', 100));
INSERT INTO events SELECT g, 1 + g % 20, date '2024-01-01' + (g * 7919 % 730) FROM generate_series(1, 30000) g;
CREATE INDEX events_col ON events USING sift_col (id, set_id, event_date);
VACUUM (FREEZE, ANALYZE) events;

-- the rows PostgreSQL expects of the join node, and the rows it returns
CREATE FUNCTION join_rows(q text, OUT planned float8, OUT actual float8) LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF, COSTS ON) ' || q INTO j;
	SELECT (n ->> 'Plan Rows')::float8, (n ->> 'Actual Rows')::float8 INTO planned, actual
	FROM jsonb_path_query(j, '$.** ? (@."Join Filter" like_regex "<@")') n LIMIT 1;
END $$;

CREATE FUNCTION q(extra text) RETURNS text LANGUAGE sql AS $f$
	SELECT format($q$WITH periods AS MATERIALIZED (SELECT * FROM (VALUES (daterange(DATE '2024-02-01', DATE '2024-04-30', '[]')), (daterange(DATE '2025-01-01', DATE '2025-03-31', '[]'))) p(range))
	SELECT e.id FROM events e JOIN periods ON e.event_date <@ periods.range %s$q$, extra) $f$;

-- within a factor of 2 of the rows it returns, with the filter of the table that the ranges also make and without it, for the other owners too
SELECT planned BETWEEN actual / 2 AND actual * 2 AS ranges_only FROM join_rows(q(''));
SELECT planned BETWEEN actual / 2 AND actual * 2 AS with_owner FROM join_rows(q('WHERE e.set_id = 3'));
SELECT planned BETWEEN actual / 2 AND actual * 2 AS with_the_same_ranges
FROM join_rows(q($$WHERE (e.event_date BETWEEN DATE '2024-02-01' AND DATE '2024-04-30') OR (e.event_date BETWEEN DATE '2025-01-01' AND DATE '2025-03-31')$$));
-- the estimate is the library's: PostgreSQL's own is far below
SET sift.enable_agg = off;
SELECT planned < actual / 10 AS stock_is_low FROM join_rows(q(''));
RESET sift.enable_agg;
DROP FUNCTION q(text), join_rows(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
