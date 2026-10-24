-- `WHERE rn <= N` over `row_number() OVER (PARTITION BY k ORDER BY o)` is the first N rows of each group: the scan runs it as the group's arrays of each
-- column, ordered by o, cut at N and unnested with their ordinal (rn is the ordinal), where the window sorts every row. The answers are the normal plan's where
-- the ORDER BY has no ties; with ties the rows kept are N of the tie, in the window and in the arrays alike. Anything else stays as written.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, grp int NOT NULL, sub int, ts int, amount int NOT NULL, tie int NOT NULL, txt text, num numeric(10,2), tags int[]) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, i % 37, CASE WHEN i % 5 = 0 THEN NULL ELSE i % 3 END, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 31) % 1000 END, i % 11, i % 4,
       CASE WHEN i % 6 = 0 THEN NULL ELSE 't' || i % 13 END, CASE WHEN i % 9 = 0 THEN NULL ELSE i / 7.0 END, ARRAY[i, i + 1]
FROM generate_series(1, 5000) i;
CREATE INDEX events_col ON events USING sift_col (id, grp, sub, ts, amount, tie, txt, num);
CREATE TABLE plain_events (id int PRIMARY KEY, grp int NOT NULL, ts int) WITH (autovacuum_enabled = false);
INSERT INTO plain_events SELECT i, i % 37, i FROM generate_series(1, 500) i;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) plain_events;

\ir test/agg_helpers.sql
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, in_scan bool, same bool, rows bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), (SELECT count(*) FROM jsonb_array_elements(outcome(q, true)::jsonb));
END $$;
CREATE FUNCTION window_why(q text) RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(DISTINCT substring(reason from '\(([^()]*)\)$'), '; ') FROM sift.explain_why(q) WHERE slug = 'window_first'
$$;

-- the bound, in its forms; rn is read: it is 1 to N
SELECT * FROM check_query('rn <= 3', $q$ SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('rn < 4', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE rn < 4 $q$);
SELECT * FROM check_query('3 >= rn', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE 3 >= rn $q$);
SELECT * FROM check_query('4 > rn', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE 4 > rn $q$);
SELECT * FROM check_query('rn <= 2::bigint AND rn <= 5', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC NULLS LAST, id) rn FROM events) t WHERE rn <= 2::bigint AND rn <= 5 $q$);
SELECT * FROM check_query('N past the size of the groups', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id) rn FROM events) t WHERE rn <= 100000 $q$);
SELECT * FROM check_query('rn = 1 AND rn <= 3 is the first row', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id) rn FROM events) t WHERE rn = 1 AND rn <= 3 $q$);
SELECT * FROM check_query('rn read in expressions and ordered by', $q$
 SELECT id, grp, rn, rn * 10 AS tenth FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn <= 4 AND rn <> 3 ORDER BY rn DESC, grp LIMIT 20 $q$);
-- the columns the arrays hold: text, numeric, NULLs, expressions, a column twice
SELECT * FROM check_query('columns of several types', $q$ SELECT * FROM (SELECT id, grp, txt, num, ts, amount * 2 AS twice, txt || '!' AS bang, row_number() OVER (PARTITION BY grp ORDER BY id) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('a column twice', $q$ SELECT * FROM (SELECT id, id AS again, grp, row_number() OVER (PARTITION BY grp ORDER BY id) rn FROM events) t WHERE rn <= 3 $q$);
-- the window's shapes: NULL partition keys, two keys, orders with NULLS FIRST / LAST and DESC, an order key that is a partition key, a named window, a condition of the sub-select
SELECT * FROM check_query('NULL partition key', $q$ SELECT * FROM (SELECT id, sub, row_number() OVER (PARTITION BY sub ORDER BY id DESC) rn FROM events) t WHERE rn <= 4 $q$);
SELECT * FROM check_query('two partition keys', $q$ SELECT * FROM (SELECT id, grp, sub, row_number() OVER (PARTITION BY grp, sub ORDER BY id DESC) rn FROM events) t WHERE rn <= 2 $q$);
SELECT o AS "order", (check_query('order ' || o, format($q$ SELECT * FROM (SELECT id, sub, ts, row_number() OVER (PARTITION BY sub ORDER BY ts %s, id) rn FROM events) t WHERE rn <= 3 $q$, o))).*
FROM unnest(ARRAY['', 'NULLS FIRST', 'DESC', 'DESC NULLS LAST']) o;
SELECT * FROM check_query('two order keys', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY amount DESC, tie, id) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('order key is a partition key', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY grp, id) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('a named window', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER w rn FROM events WINDOW w AS (PARTITION BY grp ORDER BY id DESC)) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('a condition of the sub-select and of the outer query', $q$
 SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY amount DESC, id) rn FROM events WHERE ts > 100) t WHERE rn <= 3 AND amount > 5 $q$);
SELECT * FROM check_query('CTE', $q$ WITH ranked AS (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) SELECT id, grp, rn FROM ranked WHERE rn <= 2 $q$);
SELECT * FROM check_query('under a join and an aggregate', $q$
 SELECT t.grp, count(*) n, sum(t.amount) s FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t JOIN events e ON e.id = t.id WHERE t.rn <= 5 GROUP BY t.grp $q$);

-- the result keeps the type modifier of the column (an array element has none)
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE kept AS SELECT * FROM (SELECT id, num, row_number() OVER (PARTITION BY grp ORDER BY id) rn FROM events) t WHERE rn <= 3;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
SELECT format_type(atttypid, atttypmod) AS num_type FROM pg_attribute WHERE attrelid = 'kept'::regclass AND attname = 'num';
DROP TABLE kept;

-- ties: each group gives the N rows that come first in the order, whichever of the tie; rn is 1 to the number of rows it gave
CREATE FUNCTION first_n_of_tie(q text, n int) RETURNS TABLE (groups bigint, wrong_tie_values bigint, wrong_rn bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN QUERY EXECUTE format($f$
		WITH got AS (%s),
		want AS (SELECT grp, array_agg(tie ORDER BY tie) FILTER (WHERE r <= %s) AS ties FROM (SELECT grp, tie, row_number() OVER (PARTITION BY grp ORDER BY tie) r FROM events) x GROUP BY grp),
		have AS (SELECT grp, array_agg(tie ORDER BY tie) AS ties, array_agg(rn ORDER BY rn) AS rns FROM got GROUP BY grp)
		SELECT count(*), count(*) FILTER (WHERE have.ties IS DISTINCT FROM want.ties),
		       count(*) FILTER (WHERE have.rns IS DISTINCT FROM (SELECT array_agg(i::bigint) FROM generate_series(1, cardinality(want.ties)) i))
		FROM have JOIN want USING (grp)$f$, q, n);
END $$;
SELECT * FROM first_n_of_tie($q$ SELECT grp, id, tie, rn FROM (SELECT id, grp, tie, row_number() OVER (PARTITION BY grp ORDER BY tie) rn FROM events) t WHERE rn <= 3 $q$, 3);
SELECT * FROM first_n_of_tie($q$ SELECT grp, id, tie, rn FROM (SELECT id, grp, tie, row_number() OVER (PARTITION BY grp ORDER BY tie) rn FROM events) t WHERE rn <= 40 $q$, 40);
SELECT runs_in_scan($q$ SELECT grp, id, tie, rn FROM (SELECT id, grp, tie, row_number() OVER (PARTITION BY grp ORDER BY tie) rn FROM events) t WHERE rn <= 3 $q$);

-- as written: rank(), another window function, nothing to cut but the partition and rn, a column that is an array, a table with no column index, a grouped sub-select
SELECT * FROM check_query('rank', $q$ SELECT * FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('another window function', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn, count(*) OVER (PARTITION BY grp) n FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('only the partition and rn', $q$ SELECT * FROM (SELECT grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn <= 3 $q$);
SELECT window_why($q$ SELECT * FROM (SELECT grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('an array column', $q$ SELECT * FROM (SELECT id, grp, tags, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn <= 3 $q$);
SELECT window_why($q$ SELECT * FROM (SELECT id, grp, tags, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('no column index', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM plain_events) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('a grouped sub-select', $q$
 SELECT * FROM (SELECT grp, c, row_number() OVER (PARTITION BY c % 3 ORDER BY c DESC, grp) rn FROM (SELECT grp, count(*) c FROM events GROUP BY grp) g) t WHERE rn <= 3 $q$);
SELECT * FROM check_query('the second row only', $q$ SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id DESC) rn FROM events) t WHERE rn = 2 $q$);

DROP FUNCTION first_n_of_tie(text, int);
DROP TABLE events, plain_events;
DROP FUNCTION window_why(text), check_query(text,text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
DROP EXTENSION pg_sift;
