-- A jsonb_each member read through a cast to a number (`CAST(e.value AS float8)`) is a float8 leaf of a sift_col index; a value of another JSON type
-- (a string, a boolean, an object, an array) is an overflow value of the leaf. The cast raises for it in the normal plan, for the rows that reach it
-- and nowhere else, so a flagged leaf does not keep the scan from answering the queries whose rows hold none: the scan raises the same error for a
-- row that reaches the cast with such a value. Every query runs with and without the pushdown; the rows, or the error, must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE FUNCTION runq(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- a day is a unit apart: the dirty rows sit in days 900 and up, and in the days of the sources 7 and 8
CREATE TABLE m_src (id int PRIMARY KEY, enabled bool);
INSERT INTO m_src VALUES (1, true), (2, true), (7, false), (8, true);
CREATE TABLE m_t (id int PRIMARY KEY, src int, day int, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO m_t
SELECT i, 1 + i % 2, i / 10, jsonb_build_object('m_total', i % 11, 'catg_i', (i % 7) + 0.5, 'catg_k', i % 5)
FROM generate_series(1, 6000) i;
UPDATE m_t SET src = 7, metrics = jsonb_set(metrics, '{catg_i}', '"9"') WHERE id = 100;
UPDATE m_t SET metrics = jsonb_set(metrics, '{catg_i}', '"9"') WHERE id = 6000;
UPDATE m_t SET metrics = jsonb_set(metrics, '{catg_k}', 'true') WHERE id = 5990;
UPDATE m_t SET metrics = jsonb_set(metrics, '{catg_k}', '{"a": 1}') WHERE id = 5980;
UPDATE m_t SET metrics = jsonb_set(metrics, '{catg_k}', '[1]') WHERE id = 5970;
CREATE INDEX m_col ON m_t USING sift_col (id, src, day, metrics) WITH (paths = 'metrics.* float8');
VACUUM ANALYZE m_t;

CREATE FUNCTION mq(sel text, keep text DEFAULT 'true', grp text DEFAULT 'e.key') RETURNS text LANGUAGE sql AS $$
	SELECT 'SELECT ' || sel || ' FROM m_t t JOIN LATERAL jsonb_each(t.metrics) e ON true WHERE ' || keep || ' GROUP BY ' || grp
$$;

-- the leaf is flagged and the scan is in the plan for the rows that hold no such value
SELECT pushed(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'day < 500')) AS catg_a_window_pushed;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8)), min(CAST(e.value AS float8))', 'day < 500')) AS catg_a_window;
SELECT same(mq('t.src, e.key, sum(CAST(e.value AS float8))', 'day BETWEEN 20 AND 400', 't.src, e.key')) AS catg_a_window_keys;
-- the errors: a row that reaches the cast with a value that is no number, whatever its type
SELECT runq(mq('e.key, avg(CAST(e.value AS float8))', 'day = 600'), true) AS string;
SELECT runq(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5990'), true) AS boolean;
SELECT runq(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5980'), true) AS object;
SELECT runq(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5970'), true) AS array;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'day = 600')) AS string_same;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5990')) AS boolean_same;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5980')) AS object_same;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5970')) AS array_same;
-- several rows hold one: both plans raise, the one the scan reaches first may be another row's than the heap order's
SELECT left(runq(mq('e.key, count(*), avg(CAST(e.value AS float8))'), false), 9) = left(runq(mq('e.key, count(*), avg(CAST(e.value AS float8))'), true), 9) AS whole_table_raises;
-- two rows that raise different errors: each plan raises the one it reaches first, which is no property of the query (a sort by the group key, a hash
-- aggregate and the order of the access path each reach the pairs in another order), so the plans may name different rows; none of them answers
SELECT runq(q, false) = ANY (e) AND runq(q, true) = ANY (e) AS raises_one_of_them
FROM (VALUES (mq('e.key, avg(CAST(e.value AS float8))', 'id IN (100, 5990)'), ARRAY['ERR 22023 cannot cast jsonb string to type double precision', 'ERR 22023 cannot cast jsonb boolean to type double precision']),
             (mq('e.key, avg(CAST(e.value AS float8))', 'id IN (5980, 5970)'), ARRAY['ERR 22023 cannot cast jsonb object to type double precision', 'ERR 22023 cannot cast jsonb array to type double precision'])) v(q, e);
-- a row filtered out reaches nothing: by a leaf of its row, by the key of the member, by a join that drops it
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id <> 100 AND id < 5900')) AS row_filtered;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'e.key <> $$catg_i$$ AND id <> 5990 AND id <> 5980 AND id <> 5970')) AS member_filtered;
SELECT same('SELECT e.key, avg(CAST(e.value AS float8)) FROM m_t t JOIN m_src s ON s.id = t.src AND s.enabled JOIN LATERAL jsonb_each(t.metrics) e ON true WHERE t.id <= 5900 GROUP BY e.key') AS join_drops_the_row;
SELECT pushed('SELECT e.key, avg(CAST(e.value AS float8)) FROM m_t t JOIN m_src s ON s.id = t.src AND s.enabled JOIN LATERAL jsonb_each(t.metrics) e ON true WHERE t.id <= 5900 GROUP BY e.key') AS join_drops_the_row_pushed;
-- a cast that only a FILTER or a CASE evaluates is evaluated for the members it keeps: the dirty one raises nothing here, so the plan stays
SELECT same(mq('avg(CAST(e.value AS float8)) FILTER (WHERE e.key = $$m_total$$)', 'id = 100 OR id = 5990', 't.src')) AS filter_only;
SELECT pushed(mq('avg(CAST(e.value AS float8)) FILTER (WHERE e.key = $$m_total$$)', 'id = 100 OR id = 5990', 't.src')) AS filter_only_pushed;
SELECT same(mq('sum(CASE WHEN e.key = $$m_total$$ THEN CAST(e.value AS float8) END)', 'id = 100 OR id = 5990', 't.src')) AS case_only;
-- reads of the member the row read again cannot answer for keep the normal plan: the `->` and `->>` forms, an EXISTS over the members (alone and under
-- an OR), a materialized CTE that reads them where no walk of the query sees, and a filter on the member of a key
SELECT label, CASE WHEN pushed(q) THEN 'PUSHED' ELSE same(q) END AS refused
FROM (VALUES ('arrow cast', 'SELECT t.src, sum((t.metrics -> $$catg_i$$)::float8) FROM m_t t WHERE id <= 100 GROUP BY t.src'),
             ('arrow text', 'SELECT t.src, sum((t.metrics ->> $$catg_i$$)::float8) FROM m_t t WHERE id <= 100 GROUP BY t.src'),
             ('exists', 'SELECT t.src, count(*) FROM m_t t WHERE id <= 100 AND EXISTS (SELECT 1 FROM jsonb_each(t.metrics) x WHERE CAST(x.value AS float8) > 100) GROUP BY t.src'),
             ('exists under or', mq('e.key, avg(CAST(e.value AS float8))', 'id <= 100 OR EXISTS (SELECT 1 FROM jsonb_each(t.metrics) x WHERE CAST(x.value AS float8) > 100)')),
             ('materialized cte', 'WITH x AS MATERIALIZED (SELECT t.id, e.key, e.value FROM m_t t JOIN LATERAL jsonb_each(t.metrics) e ON true) SELECT key, avg(CAST(value AS float8)) FROM x WHERE id <= 100 GROUP BY key'),
             ('key filter', mq('e.key, avg(CAST(e.value AS float8))', 'id <= 100 AND CAST(jsonb_extract_path(t.metrics, $$catg_k$$) AS integer) > 1'))) v(label, q);
-- the same cast also in an unconditional aggregate: the dirty member raises
SELECT same(mq('avg(CAST(e.value AS float8)) FILTER (WHERE e.key = $$m_total$$), count(CAST(e.value AS float8))', 'id = 5990', 't.src')) AS filter_and_plain;
-- the member cast to two types: the error names the type of the cast that raises first, in the order the aggregates run, which the row read again does not know
SELECT runq(mq('e.key, sum(CAST(e.value AS integer)), avg(CAST(e.value AS float8))', 'id = 100'), false) AS two_types_error;
SELECT same(mq('e.key, sum(CAST(e.value AS integer)), avg(CAST(e.value AS float8))', 'id = 100')) AS two_types_integer_first;
SELECT same(mq('e.key, avg(CAST(e.value AS float8)), sum(CAST(e.value AS integer))', 'id = 100')) AS two_types_float_first;
SELECT same(mq('e.key, avg(CAST(e.value AS float8)), sum(CAST(e.value AS integer))', 'day < 500')) AS two_types_clean;
SELECT pushed(mq('e.key, avg(CAST(e.value AS float8)), sum(CAST(e.value AS integer))', 'day < 500')) AS two_types_pushed;
-- a conjunct that reads the leaf evaluates the cast before the aggregates: the plan stays
SELECT same(mq('e.key, count(*)', 'CAST(e.value AS float8) > 3 AND id = 5990')) AS conjunct;
SELECT pushed(mq('e.key, count(*)', 'CAST(e.value AS float8) > 3 AND id = 5990')) AS conjunct_pushed;
-- a conjunct that tests the member alone casts every member of a row that reaches it: the row raises before the test, whatever the test says
SELECT pushed(mq('e.key, avg(CAST(e.value AS float8))', 'day < 500 AND CAST(e.value AS float8) > 3')) AS tested_pushed;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'day < 500 AND CAST(e.value AS float8) > 3')) AS tested_clean;
SELECT runq(mq('e.key, avg(CAST(e.value AS float8))', 'day = 600 AND CAST(e.value AS float8) > 1000'), true) AS tested_string;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'day = 600 AND CAST(e.value AS float8) > 1000')) AS tested_string_same;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5990 AND CAST(e.value AS float8) IS NULL')) AS tested_boolean;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5980 AND CAST(e.value AS float8) > 3')) AS tested_object;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 5970 AND CAST(e.value AS float8) > 3')) AS tested_array;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id <> 100 AND id < 5900 AND CAST(e.value AS float8) > 3')) AS tested_row_filtered;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id IN (100, 6000) AND CAST(e.value AS float8) > 1 AND CAST(e.value AS float8) < 10')) AS tested_twice;
SELECT same(mq('t.src, e.key, sum(CAST(e.value AS float8)), count(*) FILTER (WHERE CAST(e.value AS float8) > 2)', 'day BETWEEN 20 AND 400 AND CAST(e.value AS float8) > 3', 't.src, e.key')) AS tested_filter;
-- another test of the members may drop the dirty one before the cast, a join or a conjunct that may raise runs where the planner puts it: the plan stays
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 100 AND e.key <> $$catg_i$$ AND CAST(e.value AS float8) > 3')) AS tested_key_first;
SELECT pushed(mq('e.key, avg(CAST(e.value AS float8))', 'id = 100 AND e.key <> $$catg_i$$ AND CAST(e.value AS float8) > 3')) AS tested_key_first_pushed;
SELECT pushed(mq('e.key, avg(CAST(e.value AS float8))', 'id = 100 AND (CAST(e.value AS float8) > 3 OR e.key = $$m_total$$)')) AS tested_or_pushed;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id = 100 AND (CAST(e.value AS float8) > 3 OR e.key = $$m_total$$)')) AS tested_or;
SELECT same('SELECT e.key, avg(CAST(e.value AS float8)) FROM m_t t JOIN m_src s ON s.id = t.src AND s.enabled JOIN LATERAL jsonb_each(t.metrics) e ON true WHERE t.id <= 5900 AND CAST(e.value AS float8) > 3 GROUP BY e.key') AS tested_join;
SELECT pushed('SELECT e.key, avg(CAST(e.value AS float8)) FROM m_t t JOIN m_src s ON s.id = t.src AND s.enabled JOIN LATERAL jsonb_each(t.metrics) e ON true WHERE t.id <= 5900 AND CAST(e.value AS float8) > 3 GROUP BY e.key') AS tested_join_pushed;
SELECT same(mq('e.key, avg(CAST(e.value AS float8))', 'id IN (100, 5990) AND 100 / (id % 7 + 1) > 3 AND CAST(e.value AS float8) > 3')) AS tested_late;
SELECT pushed(mq('e.key, avg(CAST(e.value AS float8))', 'id IN (100, 5990) AND 100 / (id % 7 + 1) > 3 AND CAST(e.value AS float8) > 3')) AS tested_late_pushed;

-- rows in the write buffer, updated and deleted ones, and after promotion
INSERT INTO m_t VALUES (7001, 1, 10, '{"m_total": 3, "catg_i": 4}'), (7002, 1, 11, '{"m_total": "x"}');
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'day < 500')) AS write_buffer;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'id <> 7002 AND day < 500')) AS write_buffer_catg_a;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'id > 7000 AND CAST(e.value AS float8) > 1000')) AS write_buffer_tested;
UPDATE m_t SET metrics = '{"m_total": 1}' WHERE id = 7002;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'day < 500')) AS after_update;
SELECT sift.col_promote('m_col') >= 0 AS promoted;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'day < 500')) AS after_promotion;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'day = 600 AND CAST(e.value AS float8) > 1000')) AS after_promotion_tested;
DELETE FROM m_t WHERE id IN (100, 6000, 5990, 5980, 5970);
VACUUM m_t;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))')) AS after_delete;
SELECT pushed(mq('e.key, count(*), avg(CAST(e.value AS float8))')) AS after_delete_pushed;
SELECT same(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'CAST(e.value AS float8) > 3')) AS after_delete_tested;
SELECT pushed(mq('e.key, count(*), avg(CAST(e.value AS float8))', 'CAST(e.value AS float8) > 3')) AS after_delete_tested_pushed;

DROP FUNCTION mq(text, text, text);
DROP FUNCTION pushed(text);
DROP FUNCTION same(text);
DROP FUNCTION runq(text, bool);
DROP TABLE m_t, m_src;
DROP EXTENSION pg_sift;
