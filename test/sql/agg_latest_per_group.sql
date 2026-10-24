-- `SELECT .. FROM (SELECT .., row_number() OVER (PARTITION BY k ORDER BY o) rn FROM t) s WHERE rn = 1` is `SELECT DISTINCT ON (k) .. ORDER BY k, o`: the scan
-- runs it in one pass where the window sorts every row. The answers are the normal plan's where the ORDER BY has no ties; with ties the row kept is one of
-- the tie, in the window and in DISTINCT ON alike. A window that is not a lone row_number() with a PARTITION BY and an ORDER BY stays as written.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, grp int NOT NULL, sub int, ts int, amount int NOT NULL, tie int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, i % 37, CASE WHEN i % 5 = 0 THEN NULL ELSE i % 3 END, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 31) % 1000 END, i % 11, i % 4
FROM generate_series(1, 5000) i;
CREATE INDEX events_col ON events USING sift_col (id, grp, sub, ts, amount, tie);
CREATE TABLE groups (grp int PRIMARY KEY, name text) WITH (autovacuum_enabled = false);
INSERT INTO groups SELECT i, 'g' || i FROM generate_series(0, 36) i;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) groups;

\ir test/agg_helpers.sql
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, in_scan bool, same bool, rows bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), (SELECT count(*) FROM jsonb_array_elements(outcome(q, true)::jsonb));
END $$;
-- why the rewrite was not made, from sift.explain_why
CREATE FUNCTION window_why(q text) RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(DISTINCT substring(reason from '\(([^()]*)\)$'), '; ') FROM sift.explain_why(q) WHERE slug = 'window_first'
$$;

-- the filters that keep the first row, the column it makes read 1
SELECT * FROM check_query('rn = 1', $q$ SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('rn <= 1', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE rn <= 1 $q$);
SELECT * FROM check_query('rn < 2', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE rn < 2 $q$);
SELECT * FROM check_query('1 = rn', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE 1 = rn $q$);
SELECT * FROM check_query('1 >= rn', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE 1 >= rn $q$);
SELECT * FROM check_query('2 > rn', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE 2 > rn $q$);
SELECT * FROM check_query('rn = 1::bigint, rn read twice', $q$ SELECT id, grp, rn, rn + 1 AS next FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE rn = 1::bigint AND rn <= 1 $q$);
-- another condition of the outer query, a condition of the sub-select, a join
SELECT * FROM check_query('conditions around', $q$
 SELECT t.id, t.grp, g.name FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM events WHERE amount > 2) t JOIN groups g ON g.grp = t.grp
 WHERE t.rn = 1 AND t.amount > 5 $q$);
-- the window's own shapes: two partition keys, an order key that is a partition key, a descending order of an expression, a frame (row_number() has none)
SELECT * FROM check_query('two partition keys', $q$ SELECT * FROM (SELECT id, grp, sub, row_number() OVER (PARTITION BY grp, sub ORDER BY id DESC) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('order key is a partition key', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY grp, id) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('order by an expression', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY amount * 1000 - id DESC) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('a frame', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('a named window', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER w rn FROM events WINDOW w AS (PARTITION BY grp ORDER BY id DESC)) t WHERE rn = 1 $q$);
-- a CTE read once, a sub-select of a sub-select, a sublink
SELECT * FROM check_query('CTE', $q$ WITH latest AS (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) SELECT id, grp FROM latest WHERE rn = 1 $q$);
SELECT * FROM check_query('nested', $q$ SELECT count(*), sum(id) FROM (SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1) u $q$);
SELECT * FROM check_query('sublink', $q$
 SELECT g.grp, (SELECT t.id FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE t.rn = 1 AND t.grp = g.grp) AS latest FROM groups g $q$);

-- NULLs: a NULL partition key is a group of its own, a NULL order key sorts where NULLS FIRST / LAST (and the default of ASC / DESC) put it
SELECT * FROM check_query('NULL partition key', $q$ SELECT * FROM (SELECT id, sub, row_number() OVER (PARTITION BY sub ORDER BY id DESC) rn FROM events) t WHERE rn = 1 $q$);
SELECT o AS "order", (check_query('NULL order key ' || o, format($q$ SELECT * FROM (SELECT id, sub, ts, row_number() OVER (PARTITION BY sub ORDER BY ts %s, id) rn FROM events) t WHERE rn = 1 $q$, o))).*
FROM unnest(ARRAY['', 'NULLS FIRST', 'DESC', 'DESC NULLS LAST']) o;
-- under an ORDER BY of the outer query, on rn or not, and a LIMIT
SELECT * FROM check_query('ORDER BY rn, LIMIT', $q$ SELECT id, grp, rn FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1 ORDER BY rn, id LIMIT 5 $q$);
SELECT * FROM check_query('ORDER BY rn DESC', $q$ SELECT id, grp, rn FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1 ORDER BY rn DESC, grp $q$);
SELECT runq($q$ SELECT id, grp, rn FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1 ORDER BY id DESC LIMIT 3 $q$, true) =
       runq($q$ SELECT id, grp, rn FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1 ORDER BY id DESC LIMIT 3 $q$, false) AS same_limit;

-- the window stays as written: rank(), a window function besides it, no PARTITION BY or ORDER BY, a sub-select that limits, a CTE read twice, the first row
-- in a condition that is not a conjunct, more than the first row
SELECT * FROM check_query('rank', $q$ SELECT * FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1 $q$);
SELECT window_why($q$ SELECT * FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('another window function', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn, count(*) OVER (PARTITION BY grp) n FROM events) t WHERE rn = 1 $q$);
SELECT window_why($q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn, count(*) OVER (PARTITION BY grp) n FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('no PARTITION BY', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (ORDER BY id DESC) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('no ORDER BY, a group of one', $q$ SELECT count(*) FROM (SELECT id, grp, row_number() OVER (PARTITION BY id) rn FROM events) t WHERE rn = 1 $q$);
SELECT window_why($q$ SELECT * FROM (SELECT id, grp, row_number() OVER (ORDER BY id DESC) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM check_query('a limit in the sub-select', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events ORDER BY id DESC LIMIT 1000) t WHERE rn = 1 $q$);
SELECT window_why($q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events ORDER BY id DESC LIMIT 1000) t WHERE rn = 1 $q$);
SELECT * FROM check_query('a CTE read twice', $q$
 WITH latest AS (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events)
 SELECT a.id, b.id AS other FROM latest a JOIN latest b ON b.grp = a.grp AND b.rn = 2 WHERE a.rn = 1 $q$);
SELECT * FROM check_query('a MATERIALIZED CTE', $q$ WITH latest AS MATERIALIZED (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) SELECT id, grp FROM latest WHERE rn = 1 $q$);
SELECT * FROM check_query('an OR', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 1 OR grp = 3 $q$);
SELECT * FROM check_query('the second row', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 2 $q$);
SELECT * FROM check_query('the first three rows', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('a left join', $q$
 SELECT g.grp, t.id FROM groups g LEFT JOIN (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events WHERE amount = 3) t ON t.grp = g.grp AND t.rn = 1 $q$);

-- ties: the row of each group is one of the rows that tie for the first, whatever the plan (the scan keeps DISTINCT ON for an ORDER BY that is a total order)
CREATE FUNCTION first_of_tie(q text) RETURNS TABLE (groups bigint, not_first bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN QUERY EXECUTE format($f$
		SELECT count(*), count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM events e WHERE e.grp = r.grp AND e.tie = r.tie AND e.id = r.id)
		                                  OR EXISTS (SELECT 1 FROM events e WHERE e.grp = r.grp AND e.tie < r.tie))
		FROM (%s) r$f$, q);
END $$;
SELECT * FROM first_of_tie($q$ SELECT grp, id, tie FROM (SELECT id, grp, tie, row_number() OVER (PARTITION BY grp ORDER BY tie) rn FROM events) t WHERE rn = 1 $q$);
SELECT runs_in_scan($q$ SELECT grp, id, tie FROM (SELECT id, grp, tie, row_number() OVER (PARTITION BY grp ORDER BY tie) rn FROM events) t WHERE rn = 1 $q$);
SELECT * FROM first_of_tie($q$ SELECT DISTINCT ON (grp) grp, id, tie FROM events ORDER BY grp, tie $q$);

DROP FUNCTION first_of_tie(text);
DROP TABLE events, groups;
DROP FUNCTION window_why(text), check_query(text,text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
DROP EXTENSION pg_sift;
