-- A typed leaf of a sift_col index cannot store every value a document holds (a numeric string in a float8 path, a non-object document, a
-- number in a text path): the rows that hold one are read again from the heap by the aggregate scan as the SQL of jsonb_to_recordset reads
-- them (the text of the scalar through the input function of the column's type), so the scan answers, errors included, as the normal plan.
-- Every query runs with and without the pushdown; the rows, or the error, must be the same.
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

-- the rows (or the error) of the normal plan and of the scan; the scan is in the plan
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

-- the rows the scan read again from the heap
CREATE FUNCTION reread(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; n bigint := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows Read Again From The Heap' THEN n := regexp_replace(l, '\D', '', 'g')::bigint; END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION rec_q(sel text, keep text DEFAULT 'true', grp text DEFAULT 'o.c') RETURNS text LANGUAGE sql AS $$
	SELECT 'SELECT ' || sel || ' FROM d_t CROSS JOIN LATERAL jsonb_to_recordset(doc -> ''opinions'') AS o(c text, n int4, t text, score float8, ok bool) WHERE '
	       || keep || ' GROUP BY ' || grp
$$;

CREATE TABLE d_t (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO d_t
SELECT i, i % 5, jsonb_build_object('opinions', jsonb_build_array(jsonb_build_object('c', 'a', 'n', i, 't', 'x', 'score', i % 10, 'ok', i % 2 = 0),
                                                                 jsonb_build_object('c', 'b', 'n', i, 't', 'y', 'score', (i % 7) + 0.5, 'ok', i % 3 = 0)))
FROM generate_series(1, 3000) i;
-- a document that is not an object (an array: a key of it is NULL), scalars, a null
UPDATE d_t SET doc = '[1, 2]' WHERE id % 100 = 1;
UPDATE d_t SET doc = '"text"' WHERE id % 100 = 2;
UPDATE d_t SET doc = '7' WHERE id % 100 = 3;
UPDATE d_t SET doc = 'null' WHERE id % 100 = 4;
-- values the leaves cannot store that SQL reads: a numeric string, a number in a text path, a boolean in a text path, a string for a boolean and an integer
UPDATE d_t SET doc = '{"opinions": [{"c": "a", "n": "5", "t": 12, "score": "8.5", "ok": "yes"}, {"c": "b", "n": 7, "t": true, "score": " 9 ", "ok": "f"}]}'
WHERE id % 100 = 5;
CREATE INDEX d_t_col ON d_t USING sift_col (id, grp, doc) WITH (paths = 'doc.opinions[]{c text, n int4, t text, score float8, ok bool}');
VACUUM ANALYZE d_t;

-- the leaves are dirty and the scan is in the plan; it reads the 60 documents (30 of them not objects, 30 with strings) again
SELECT pushed(rec_q('o.c, count(*), avg(o.score)')) AS pushed, reread(rec_q('o.c, count(*), avg(o.score)')) AS reread;
SELECT same(rec_q('o.c, count(*), avg(o.score), sum(o.n), count(o.ok)')) AS all_leaves;
SELECT same(rec_q('o.t, count(*), min(o.score), max(o.score)', 'true', 'o.t')) AS text_key;
SELECT same(rec_q('grp, count(*), sum(o.score)', 'o.c = $$a$$', 'grp')) AS filtered_by_text;
SELECT same(rec_q('o.c, count(*) FILTER (WHERE o.score > 5), count(*) FILTER (WHERE o.ok)')) AS filtered_aggregates;
SELECT same(rec_q('o.c, count(*), avg(o.score)', 'o.score > 3')) AS element_conjunct;
SELECT same(rec_q('o.c, count(*), avg(o.score)', 'grp = 2 AND id > 1000')) AS row_conjunct;
SELECT pushed(rec_q('o.c, count(*), avg(o.score)', 'o.score > 3')) AS element_conjunct_pushed;

-- errors: the document reaches the function, whatever the aggregates read of it, and the error is the one SQL raises
INSERT INTO d_t VALUES (9001, 9, '{"opinions": [{"c": "a", "score": "abc"}]}');
INSERT INTO d_t VALUES (9002, 9, '{"opinions": [{"c": "a", "score": true}]}');
INSERT INTO d_t VALUES (9003, 9, '{"opinions": [{"c": "a", "score": {"a": 1}}]}');
INSERT INTO d_t VALUES (9005, 9, '{"opinions": {"c": "a"}}');
INSERT INTO d_t VALUES (9007, 9, '{"opinions": [{"c": "a", "score": 1e400}]}');
INSERT INTO d_t VALUES (9008, 9, '{"opinions": [{"c": "a", "n": "1.5"}]}');
INSERT INTO d_t VALUES (9009, 9, '{"opinions": [{"c": "a", "ok": "maybe"}]}');
VACUUM ANALYZE d_t;
SELECT id, same(rec_q('count(*), avg(o.score)', 'id = ' || id, 'grp')) AS score_read, same(rec_q('count(*), sum(o.n)', 'id = ' || id, 'grp')) AS n_read,
       same(rec_q('count(*)', 'id = ' || id, 'grp')) AS count_only
FROM d_t WHERE id > 9000 ORDER BY id;
SELECT runq(rec_q('count(*), avg(o.score)', 'id = 9001', 'grp'), true) AS the_error;
SELECT runq(rec_q('count(*)', 'id = 9005', 'grp'), true) AS the_list_error;
SELECT runq(rec_q('count(*)', 'id = 9008', 'grp'), true) AS the_unread_column_error;
-- the rows that do not reach the function raise nothing
SELECT same(rec_q('o.c, count(*), avg(o.score)', 'id < 9000')) AS filtered_out;
SELECT same(rec_q('o.c, count(*), avg(o.score)', 'grp <> 9')) AS filtered_out_by_a_leaf;
SELECT same(rec_q('o.c, count(*), avg(o.score)')) AS reached;
DELETE FROM d_t WHERE id IN (9001, 9002, 9003, 9005, 9007, 9008, 9009);
VACUUM d_t;
SELECT same(rec_q('o.c, count(*), avg(o.score), sum(o.n)')) AS after_vacuum;

-- rows in the write buffer, deleted and updated ones
INSERT INTO d_t VALUES (8001, 1, '[3]'), (8002, 1, '{"opinions": [{"c": "a", "n": "9", "t": false, "score": "1.25", "ok": 1}]}');
SELECT same(rec_q('o.c, count(*), avg(o.score), sum(o.n)', 'true')) AS write_buffer;
DELETE FROM d_t WHERE id = 8002;
UPDATE d_t SET doc = '{"opinions": [{"c": "b", "n": "4", "t": 1, "score": "2"}]}' WHERE id = 105;
UPDATE d_t SET grp = 3 WHERE id = 106;
SELECT same(rec_q('o.c, count(*), avg(o.score), sum(o.n)', 'true')) AS after_delete_update;
SELECT sift.col_promote('d_t_col') >= 0 AS promoted;
SELECT same(rec_q('o.c, count(*), avg(o.score), sum(o.n)', 'true')) AS after_promotion;

-- the plan keeps the normal plan when something else reads the leaf of the list or the query reads it in a form the source cannot read again
SELECT pushed('SELECT o.c, avg(o.score) FROM d_t, jsonb_to_recordset(doc -> $$opinions$$) AS o(c text, score float8) WHERE jsonb_array_length(doc -> $$opinions$$) > 1 GROUP BY o.c') AS list_conjunct_pushed;
SELECT pushed('SELECT o.c, count(*) FROM d_t, jsonb_array_elements(doc -> $$opinions$$) e, jsonb_to_record(e) AS o(c text) GROUP BY o.c') AS elements_pushed;
