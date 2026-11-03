-- A window function over sort keys that tie has no one answer in PostgreSQL: the rows of the tie reach the WindowAgg in the order of the plan below it, and lag() of a row of
-- the tie is the row before it in that order. The scan below a WindowAgg changes that order and nothing else: the window functions are PostgreSQL's (the scan only computes
-- the aggregates under them, and the first row of each group when it can prove a total order), so the rows come out as a multiset equal to the plan without the scan, and a
-- LIMIT over the tied keys keeps one row of that multiset.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, event_date date NOT NULL, value_num numeric(8,2) NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + g % 6, date '2025-01-01' + g % 40, (g % 17)::numeric / 4 FROM generate_series(1, 6000) g;
CREATE INDEX events_col ON events USING sift_col (dim_a_id, event_date, value_num);
VACUUM (FREEZE, ANALYZE) events;

-- two identical rows of the current period for dim_a_id 1 (as two grouping sets give) and one of the period before: lag of the first row of the tie is the earlier period, of the second its twin
CREATE VIEW periods AS
SELECT dim_a_id, 'current' AS period, sum(value_num) AS v, count(*) AS n FROM events WHERE event_date >= '2025-01-21' GROUP BY dim_a_id
UNION ALL SELECT 1, 'current', sum(value_num), count(*) FROM events WHERE event_date >= '2025-01-21' AND dim_a_id = 1
UNION ALL SELECT dim_a_id, 'previous', sum(value_num), count(*) FROM events WHERE event_date < '2025-01-21' GROUP BY dim_a_id;

CREATE FUNCTION rows_of(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN scan THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT string_agg(t::text, ''|'' ORDER BY t::text) FROM (%s) t', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION has_node(q text, node text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

\set lagged 'SELECT dim_a_id, period, v, lag(v) OVER (PARTITION BY dim_a_id ORDER BY period DESC) AS before FROM periods'
-- the rows of the window, scan on against off (the twin rows are alike, so which of them lags the other is not in the multiset), and the window is PostgreSQL's
SELECT rows_of(:'lagged', true) = rows_of(:'lagged', false) AS same_rows, has_node(:'lagged', 'WindowAgg') AS window_agg;
SELECT count(*) AS twins_in_partition FROM periods WHERE dim_a_id = 1 AND period = 'current';
-- a LIMIT over the tied key keeps a row of the unlimited answer, with the scan on and off
SELECT bool_and(position(l IN u) > 0) AS limited_row_is_a_row
FROM (SELECT rows_of(:'lagged' || ' ORDER BY v DESC, dim_a_id LIMIT 1', s) AS l, rows_of(:'lagged', s) AS u FROM (VALUES (true), (false)) t(s)) x;
-- the first row of each group (row_number() = 1) over a tied order: the scan keeps the first row of the tie it meets, PostgreSQL's plan one of them
SELECT slug, used, (SELECT count(*) = count(DISTINCT dim_a_id) FROM (SELECT dim_a_id FROM (SELECT dim_a_id, row_number() OVER (PARTITION BY dim_a_id ORDER BY value_num DESC) AS rn FROM events) t WHERE rn = 1) o) AS one_row_per_group
FROM sift.explain_why($q$SELECT dim_a_id, event_date FROM (SELECT dim_a_id, event_date, row_number() OVER (PARTITION BY dim_a_id ORDER BY value_num DESC) AS rn FROM events) t WHERE rn = 1$q$);

DROP FUNCTION rows_of(text, bool), has_node(text, text);
DROP VIEW periods;
DROP TABLE events;
DROP EXTENSION pg_sift;
