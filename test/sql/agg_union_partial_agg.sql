-- A GROUP BY over a UNION ALL of selects (`FROM (SELECT t0 AS ts, 1 AS q FROM events WHERE .. UNION ALL SELECT t1, -1 FROM events WHERE ..) ev GROUP BY bucket(ts)`)
-- groups each arm by the keys and sums, counts, mins and maxes those groups above the union: the arms return one row per group, not one per row, and each arm that
-- reads a table with a column index runs in the scan. The answers are the plain plan's, the aggregates that cannot be added up from groups (avg, DISTINCT, FILTER,
-- float sums, an ordered aggregate), a UNION that removes duplicates and an arm that limits its rows keep the query as written, and so do arms that return a row
-- for each group already (the grouping would only repeat the work of the one above).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, kind text NOT NULL, t0 timestamp NOT NULL, t1 timestamp, t2 timestamp, n int, big bigint, amt numeric, f float8) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 500, (ARRAY['a', 'a', 'a', 'b', 'c', 'd'])[1 + i % 6], t0, CASE WHEN i % 7 <> 0 THEN t0 + (i % 30) * interval '1 minute' END,
       CASE WHEN i % 7 NOT IN (0, 1) THEN t0 + (30 + i % 40) * interval '1 minute' END, CASE WHEN i % 11 <> 0 THEN i % 100 END, CASE WHEN i % 13 <> 0 THEN i * 100000000::bigint END, (i % 1000) / 8.0, i / 3.0
FROM (SELECT i, timestamp '2025-12-01' + (hashint4(i) & 16777215) % 864000 * interval '3 seconds' AS t0 FROM generate_series(1, 40000) i) q;
CREATE TABLE links_a (id int PRIMARY KEY, set_id int NOT NULL, dim_a_id int NOT NULL);
INSERT INTO links_a SELECT i, 4, i FROM generate_series(1, 400) i;
CREATE INDEX links_a_set ON links_a (set_id, dim_a_id);
CREATE INDEX events_t0 ON events (t0);
CREATE INDEX events_col ON events USING sift_col (dim_a_id, kind, t0, t1, t2, n, big, amt, f);
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) links_a;

\ir test/agg_helpers.sql
-- in_scan: arms of the plan run in the scan; same: the rows are the plan's without the extension
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, in_scan bool, same bool, rows bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT name, pushed(q), runq(q, true) = runq(q, false), (SELECT count(*) FROM jsonb_array_elements(runq(q, true)::jsonb));
END $$;

-- the bucketed depth of a queue from three timestamps of the same rows, one arm for each
SELECT * FROM check_query('date_bin over three arms, sums of constants', $q$
 SELECT date_bin('15 minutes'::interval, ev.ts, timestamp '2000-01-01') AS bucket, ev.kind, ev.ord, sum(ev.queued) AS queued, sum(ev.active) AS active
 FROM (
   SELECT e.t0 AS ts, e.kind, 1 AS queued, 0 AS active, 0 AS ord FROM events e
     WHERE e.kind IN ('a', 'b') AND e.t0 >= timestamp '2025-12-05' AND e.t0 < timestamp '2025-12-09' AND EXISTS (SELECT 1 FROM links_a l WHERE l.set_id = 4 AND l.dim_a_id = e.dim_a_id)
   UNION ALL
   SELECT e.t1, e.kind, -1, 1, 1 FROM events e
     WHERE e.kind IN ('a', 'b') AND e.t1 >= timestamp '2025-12-05' AND e.t1 < timestamp '2025-12-09' AND EXISTS (SELECT 1 FROM links_a l WHERE l.set_id = 4 AND l.dim_a_id = e.dim_a_id)
   UNION ALL
   SELECT coalesce(e.t2, e.t1 + interval '1 hour'), e.kind, 0, -1, 2 FROM events e
     WHERE e.kind IN ('a', 'b') AND e.t1 IS NOT NULL AND coalesce(e.t2, e.t1 + interval '1 hour') >= timestamp '2025-12-05' AND coalesce(e.t2, e.t1 + interval '1 hour') < timestamp '2025-12-09'
       AND EXISTS (SELECT 1 FROM links_a l WHERE l.set_id = 4 AND l.dim_a_id = e.dim_a_id)
 ) ev GROUP BY 1, 2, 3 $q$);
-- the aggregates that groups add up, over columns: the types of the answers are the plain plan's (a count and a sum of ints are bigint, a sum of bigint is numeric)
SELECT * FROM check_query('count, sum of int, bigint and numeric, min, max', $q$
 SELECT date_trunc('day', ev.ts) AS d, ev.kind, count(*) AS c, count(ev.n) AS cn, sum(ev.n) AS sn, sum(ev.big) AS sb, sum(ev.amt) AS sa, min(ev.ts) AS lo, max(ev.kind) AS hi, min(ev.amt) AS lo_a
 FROM (SELECT e.t0 AS ts, e.kind, e.n, e.big, e.amt FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.t1, e.kind, e.n, e.big, e.amt FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev
 GROUP BY 1, 2 $q$);
SELECT * FROM check_query('types of the answers', $q$
 SELECT pg_typeof(count(*))::text c, pg_typeof(sum(ev.n))::text sn, pg_typeof(sum(ev.big))::text sb, pg_typeof(sum(ev.amt))::text sa, pg_typeof(min(ev.kind))::text lo
 FROM (SELECT e.kind, e.n, e.big, e.amt, e.t0 FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.kind, e.n, e.big, e.amt, e.t1 FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY ev.kind $q$);
SELECT * FROM check_query('HAVING and an expression of the aggregates, ORDER BY', $q$
 SELECT ev.kind, sum(ev.n) * 2 AS twice
 FROM (SELECT e.kind, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.kind, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY ev.kind HAVING count(*) > 10 ORDER BY 1 $q$);
SELECT * FROM check_query('one key, an empty arm, an arm of NULL keys', $q$
 SELECT date_trunc('hour', ev.ts) AS h, sum(ev.v) AS s, count(ev.ts) AS c
 FROM (SELECT e.t0 AS ts, 1 AS v FROM events e WHERE e.t0 >= timestamp '2025-12-02' AND e.t0 < timestamp '2025-12-12'
       UNION ALL SELECT e.t0, 5 FROM events e WHERE false
       UNION ALL SELECT NULL::timestamp, 7 FROM events e WHERE e.id < 20) ev GROUP BY 1 $q$);
SELECT * FROM check_query('a nested union', $q$
 SELECT ev.kind, sum(ev.v) AS s
 FROM (SELECT e.kind, 1 AS v FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL (SELECT e.kind, 2 FROM events e WHERE e.t1 >= timestamp '2025-12-02' UNION ALL SELECT e.kind, 3 FROM events e WHERE e.t2 >= timestamp '2025-12-02')) ev GROUP BY 1 $q$);

-- kept as written
SELECT * FROM check_query('avg', $q$
 SELECT ev.kind, avg(ev.n) AS a FROM (SELECT e.kind, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.kind, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY 1 $q$);
SELECT * FROM check_query('count DISTINCT', $q$
 SELECT ev.kind, count(DISTINCT ev.n) AS a FROM (SELECT e.kind, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.kind, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY 1 $q$);
SELECT * FROM check_query('sum of float8 (the order of the additions changes the answer)', $q$
 SELECT ev.kind, sum(ev.f) AS a FROM (SELECT e.kind, e.f FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.kind, e.f FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY 1 $q$);
SELECT * FROM check_query('FILTER', $q$
 SELECT ev.kind, count(*) FILTER (WHERE ev.n > 50) AS a FROM (SELECT e.kind, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.kind, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY 1 $q$);
SELECT * FROM check_query('ordered aggregate', $q$
 SELECT ev.kind, array_agg(ev.n ORDER BY ev.n) AS a FROM (SELECT e.kind, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' AND e.id < 300 UNION ALL SELECT e.kind, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02' AND e.id < 300) ev GROUP BY 1 $q$);
SELECT * FROM check_query('UNION removes duplicates', $q$
 SELECT ev.kind, sum(ev.n) AS a FROM (SELECT e.kind, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION SELECT e.kind, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY 1 $q$);
SELECT * FROM check_query('an arm with LIMIT', $q$
 SELECT ev.kind, sum(ev.n) AS a FROM (SELECT e.kind, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL (SELECT e.kind, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02' ORDER BY e.id LIMIT 500)) ev GROUP BY 1 $q$);
SELECT * FROM check_query('a join next to the union', $q$
 SELECT d.kind, sum(ev.n) AS a FROM (SELECT e.id, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.id, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev JOIN events d ON d.id = ev.id GROUP BY 1 $q$);
-- the arms return a row for each group already: the grouping of each arm would only repeat the one above
SELECT * FROM check_query('the key of each arm is unique', $q$
 SELECT ev.id, sum(ev.n) AS a FROM (SELECT e.id, e.n FROM events e WHERE e.t0 >= timestamp '2025-12-02' UNION ALL SELECT e.id, e.n FROM events e WHERE e.t1 >= timestamp '2025-12-02') ev GROUP BY 1 $q$);
DROP TABLE events, links_a;
DROP FUNCTION check_query(text,text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
DROP EXTENSION pg_sift;
