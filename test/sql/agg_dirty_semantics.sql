-- A value a typed leaf cannot store is read again from the heap, and only where the query reads it: the member of the key a query picks, the
-- elements of a list that an EXISTS conjunct tests. Every query runs with and without the pushdown; the rows, or the error (message and SQLSTATE),
-- must be the same, and a dirty value that the query does not read must not keep the scan from answering.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

\ir test/agg_helpers.sql

-- the rows or the error must be the same; expect_pushed says whether the query is meant to be scanned
CREATE FUNCTION same(q text, expect_pushed bool DEFAULT true) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN (CASE WHEN pushed(q) <> expect_pushed
	             THEN CASE WHEN expect_pushed THEN 'NOT PUSHED: ' ELSE 'PUSHED: ' END ELSE '' END) ||
		CASE WHEN a = b THEN CASE WHEN a LIKE 'ERR%' THEN 'same ' || left(a, 90) ELSE 'same' END ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;

-- the scan answers where the normal plan raises: no error of the scan, whatever the normal plan does
CREATE FUNCTION answers(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' WHEN b LIKE 'ERR%' THEN 'RAISES ' || left(b, 100) WHEN a LIKE 'ERR%' THEN 'answers (the normal plan raises)' ELSE 'DIFF' END;
END $$;

-- ---- an EXISTS over the elements of a list whose score is dirty in some rows ----
CREATE TABLE items (id int PRIMARY KEY, kind text, day int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO items
SELECT i, 'cat_' || chr(97 + i % 3), i / 10,
	jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('kind', 'cat_a', 'score', i % 9), jsonb_build_object('kind', 'cat_b', 'score', i % 4)))
FROM generate_series(1, 3000) i;
UPDATE items SET doc = jsonb_set(doc, '{parts,1,score}', '"x"') WHERE id = 100;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', '"4"') WHERE id = 500;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', '"-0"') WHERE id = 501;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', '"1e3"') WHERE id = 502;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', '""') WHERE id = 700;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', 'null') WHERE id = 800;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', '[1, [2]]') WHERE id = 900;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', '"x"') WHERE id = 1000;
UPDATE items SET doc = jsonb_set(doc, '{parts,0,score}', 'true') WHERE id = 1100;
UPDATE items SET doc = '{"parts": "str"}' WHERE id = 1200;
UPDATE items SET doc = '{"parts": [1]}' WHERE id = 1201;
UPDATE items SET doc = '{"parts": null}' WHERE id = 1202;
CREATE INDEX items_col ON items USING sift_col (id, kind, day, doc) WITH (paths = 'doc.parts[]{kind text, score float8}');
VACUUM ANALYZE items;

CREATE FUNCTION iq(rng text, keep text, sel text DEFAULT 't.kind, count(*), sum(t.id)') RETURNS text LANGUAGE sql AS $f$
	SELECT 'SELECT ' || sel || ' FROM items t WHERE ' || rng || ' AND ' || keep || ' GROUP BY t.kind'
$f$;
CREATE FUNCTION ex(cond text) RETURNS text LANGUAGE sql AS $f$
	SELECT 'EXISTS (SELECT 1 FROM jsonb_to_recordset(t.doc -> $$parts$$) p(kind text, score float8) WHERE ' || cond || ')'
$f$;

SELECT pushed(iq('t.id < 90', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS exists_pushed;
SELECT pushed(iq('t.id < 3000', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS exists_over_dirty_pushed;
-- the ranges hold the rows of each kind of dirty value; the normal plan raises for the ones that are no number, the scan raises the same
SELECT r.name, same(iq(r.rng, ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS exists_cat_a
FROM (VALUES ('clean', 't.id < 90'), ('other element', 't.id BETWEEN 90 AND 110'), ('numeric string', 't.id BETWEEN 495 AND 499'),
             ('string 4', 't.id = 500'), ('string -0', 't.id = 501'), ('string 1e3', 't.id = 502'), ('empty', 't.id = 700'), ('null', 't.id = 800'),
             ('nested array', 't.id = 900'), ('string x', 't.id = 1000'), ('bool', 't.id = 1100'), ('list a string', 't.id = 1200'),
             ('beyond', 't.id > 1300')) r(name, rng)
ORDER BY r.name;
-- a list that is a JSON null or holds a scalar is flagged by no unit: the clean path of the scan reads it as before, and answers where the normal plan raises
SELECT answers(iq('t.id = 1201', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS list_of_numbers,
       answers(iq('t.id = 1202', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS list_null;
SELECT r.name, same(iq(r.rng, 'NOT ' || ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS not_exists
FROM (VALUES ('clean', 't.id < 90'), ('other element', 't.id BETWEEN 90 AND 110'), ('string 4', 't.id = 500'), ('empty', 't.id = 700'),
             ('null', 't.id = 800'), ('string x', 't.id = 1000')) r(name, rng)
ORDER BY r.name;
-- a conjunct that drops the row raises nothing in either plan; one that keeps it reaches the error
SELECT same(iq('t.id BETWEEN 90 AND 110 AND t.kind = $$cat_a$$', ex('p.score > 3'))) AS dropped_by_kind;
SELECT same(iq('t.id BETWEEN 90 AND 110 AND t.id <> 100', ex('p.score > 3'))) AS dropped_by_id;
SELECT same(iq('t.id BETWEEN 90 AND 110', ex('p.score > 3'))) AS kept;
SELECT same(iq('t.id BETWEEN 90 AND 1010', ex('p.score > 3'), 'count(*)') || ' ') AS whole_range;
SELECT same(iq('t.id BETWEEN 495 AND 505', ex('p.kind = $$cat_a$$ AND p.score > 3'), 't.kind, count(*), min(t.id), max(t.id)')) AS numeric_strings_convert;
-- two tests of the same list, a test of another, and one under OR (the OR keeps the normal plan: the other side may decide the row first)
SELECT same(iq('t.id BETWEEN 495 AND 505', ex('p.kind = $$cat_a$$') || ' AND ' || ex('p.score > 3'))) AS two_exists;
SELECT same(iq('t.id BETWEEN 495 AND 505', ex('p.kind = $$cat_a$$ AND p.score > 3') || ' OR t.kind = $$cat_b$$'), false) AS or_exists;
SELECT pushed(iq('t.id BETWEEN 495 AND 505', ex('p.kind = $$cat_a$$ AND p.score > 3') || ' OR t.kind = $$cat_b$$')) AS or_exists_pushed;
-- a call that declares other columns does not read the score: it cannot raise for it, and a call that declares them beside one that does keeps the normal plan
SELECT same(iq('t.id BETWEEN 90 AND 110', 'EXISTS (SELECT 1 FROM jsonb_to_recordset(t.doc -> $$parts$$) p(kind text) WHERE p.kind = $$cat_a$$)')) AS exists_other_columns;
SELECT same(iq('t.id BETWEEN 90 AND 110', ex('p.score > 3') || ' AND EXISTS (SELECT 1 FROM jsonb_to_recordset(t.doc -> $$parts$$) p(kind text) WHERE p.kind = $$cat_a$$)'), false) AS exists_two_calls;
SELECT same(iq('t.id BETWEEN 90 AND 99', ex('p.score > 3') || ' AND EXISTS (SELECT 1 FROM jsonb_to_recordset(t.doc -> $$parts$$) p(kind text) WHERE p.kind = $$cat_a$$)'), false) AS exists_two_calls_clean;
SELECT same(iq('t.id BETWEEN 495 AND 505', ex('p.kind = $$cat_a$$ AND p.score > 3'), 't.kind, sum((t.doc -> $$parts$$ -> 0 ->> $$score$$)::float8)'), false) AS exists_with_path_read;
SELECT same(iq('t.id BETWEEN 495 AND 505', ex('p.kind = $$cat_a$$ AND p.score > 3'), 't.kind, sum(jsonb_array_length(t.doc -> $$parts$$))'), false) AS exists_with_length;

-- the writer's rows: in the write buffer, updated, promoted, merged, vacuumed
INSERT INTO items VALUES (4001, 'cat_a', 1, '{"parts": [{"kind": "cat_a", "score": "5"}]}'), (4002, 'cat_b', 1, '{"parts": [{"kind": "cat_a", "score": "n/a"}]}'),
                         (4003, 'cat_c', 1, '{"parts": [{"kind": "cat_a", "score": 9}]}');
SELECT same(iq('t.id BETWEEN 4001 AND 4001', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS buffer_numeric_string;
SELECT same(iq('t.id BETWEEN 4002 AND 4002', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS buffer_string;
SELECT same(iq('t.id BETWEEN 4003 AND 4003', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS buffer_clean;
SELECT same(iq('t.id > 4000 AND t.id <> 4002', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS buffer_dirty_dropped;
UPDATE items SET doc = '{"parts": [{"kind": "cat_a", "score": 8}, {"kind": "cat_b", "score": "z"}]}' WHERE id IN (100, 4003);
SELECT same(iq('t.id BETWEEN 90 AND 110', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS after_update;
SELECT same(iq('t.id = 4003', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS after_update_buffer;
UPDATE items SET doc = '{"parts": [{"kind": "cat_a", "score": 8}, {"kind": "cat_b", "score": 2}]}' WHERE id IN (100, 4003);
SELECT same(iq('t.id BETWEEN 90 AND 110', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS healed;
SELECT sift.col_promote('items_col') >= 0 AS promoted;
SELECT same(iq('t.id BETWEEN 495 AND 1010', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS after_promotion;
SELECT same(iq('t.id BETWEEN 4001 AND 4003', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS after_promotion_buffer;
SELECT sift.col_merge('items_col', true) >= 0 AS merged;
SELECT same(iq('t.id BETWEEN 495 AND 1010', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS after_merge;
DELETE FROM items WHERE id IN (1000, 4002);
VACUUM items;
SELECT same(iq('t.id BETWEEN 495 AND 1300', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS after_vacuum;
SELECT same(iq('t.id BETWEEN 495 AND 1010', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS after_vacuum_other;

-- workers
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT same(iq('t.id BETWEEN 90 AND 110', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS workers_error;
SELECT same(iq('t.id BETWEEN 495 AND 505', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS workers_numeric_strings;
SELECT same(iq('t.id < 3000 AND t.id NOT IN (100, 700, 900, 1100, 1200, 1201, 1202)', ex('p.kind = $$cat_a$$ AND p.score > 3'))) AS workers_whole;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;
DROP TABLE items;

-- ---- the member of one key of a map, read through a cast: only that member can raise ----
CREATE TABLE events (id int PRIMARY KEY, kind text, day int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 'cat_' || chr(97 + i % 3), i / 10, jsonb_build_object('m_a', i % 7, 'm_b', i % 5 + 0.5, 'm_c', i % 3) FROM generate_series(1, 3000) i;
UPDATE events SET doc = jsonb_set(doc, '{m_b}', '"x"') WHERE id = 100;
UPDATE events SET doc = jsonb_set(doc, '{m_c}', '[1, 2]') WHERE id = 101;
UPDATE events SET doc = jsonb_set(doc, '{m_c}', 'true') WHERE id = 102;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '"x"') WHERE id = 1000;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '"4"') WHERE id = 1100;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '"-0"') WHERE id = 1101;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '"1e3"') WHERE id = 1102;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '""') WHERE id = 1103;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '[1, [2]]') WHERE id = 1104;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', 'true') WHERE id = 1105;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '{"a": 1}') WHERE id = 1106;
UPDATE events SET doc = doc - 'm_a' WHERE id = 1107;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '1e300') WHERE id = 1108;
CREATE INDEX events_col ON events USING sift_col (id, kind, day, doc) WITH (paths = 'doc.* float8');
VACUUM ANALYZE events;

CREATE FUNCTION kq(rng text, sel text, key text DEFAULT 'm_a', grp text DEFAULT 't.kind') RETURNS text LANGUAGE sql AS $f$
	SELECT 'SELECT ' || grp || ', ' || sel || ' FROM events t LEFT JOIN LATERAL jsonb_each(t.doc) r ON r.key = ' || quote_literal(key) || ' WHERE ' || rng || ' GROUP BY ' || grp
$f$;

SELECT pushed(kq('t.id < 90', 'sum(CAST(r.value AS float8))')) AS member_pushed;
SELECT pushed(kq('t.id < 3000', 'sum(CAST(r.value AS float8))')) AS member_over_dirty_pushed;
SELECT pushed(kq('t.id < 3000', 'sum(CAST(r.value AS integer))')) AS member_integer_pushed;
SELECT r.name, same(kq(r.rng, 'sum(CAST(r.value AS float8)), count(*)')) AS cast_float8, same(kq(r.rng, 'sum(CAST(r.value AS integer)), count(*)')) AS cast_integer
FROM (VALUES ('clean', 't.id < 90'), ('other member', 't.id BETWEEN 90 AND 110'), ('string 4', 't.id = 1100'), ('string -0', 't.id = 1101'),
             ('string 1e3', 't.id = 1102'), ('empty', 't.id = 1103'), ('string x', 't.id = 1000'), ('nested array', 't.id = 1104'), ('bool', 't.id = 1105'),
             ('object', 't.id = 1106'), ('missing', 't.id = 1107'), ('beyond float8', 't.id = 1108'), ('beyond', 't.id > 1200')) r(name, rng)
ORDER BY r.name;
-- the key a row is grouped by, and the other keys
SELECT same(kq('t.id BETWEEN 90 AND 110', 'count(*), min(t.id)', 'm_a', 'CAST(r.value AS integer)')) AS cast_as_group_key;
SELECT same(kq('t.id = 101', 'sum(CAST(r.value AS float8))', 'm_c')) AS key_m_c_array;
SELECT same(kq('t.id = 102', 'sum(CAST(r.value AS float8))', 'm_c')) AS key_m_c_bool;
SELECT same(kq('t.id BETWEEN 90 AND 99', 'sum(CAST(r.value AS float8))', 'm_c')) AS key_m_c_clean;
SELECT same(kq('t.id BETWEEN 90 AND 110', 'sum(CAST(r.value AS float8))', 'm_b')) AS key_m_b_dirty;
SELECT same(kq('t.id BETWEEN 90 AND 110', 'sum(CAST(r.value AS float8))', 'm_d')) AS key_missing;
-- a row that another conjunct drops raises nothing; one that is kept does
SELECT same(kq('t.id BETWEEN 990 AND 1010 AND t.id <> 1000', 'sum(CAST(r.value AS float8))')) AS dirty_dropped;
SELECT same(kq('t.id BETWEEN 990 AND 1010', 'sum(CAST(r.value AS float8))')) AS dirty_kept;
SELECT same(kq('t.id BETWEEN 990 AND 1010 AND t.kind = $$cat_b$$', 'sum(CAST(r.value AS float8))')) AS dirty_dropped_by_kind;
-- the member read next to a read of every member, or inside a conditional branch, keeps the normal plan or the same answer
SELECT same('SELECT t.kind, e.key, sum(CAST(e.value AS float8)) FROM events t JOIN LATERAL jsonb_each(t.doc) e ON true WHERE t.id BETWEEN 99 AND 100 GROUP BY 1, 2') AS every_member;
SELECT same(kq('t.id BETWEEN 90 AND 110', 'sum(CAST(r.value AS float8)) FILTER (WHERE t.id > 100)'), false) AS filtered_member;
SELECT same(kq('t.id BETWEEN 990 AND 1010', 'sum(CASE WHEN t.id < 1000 THEN CAST(r.value AS float8) END)'), false) AS conditional_member;
SELECT same(kq('t.id BETWEEN 990 AND 1010 AND CAST(r.value AS float8) > 2', 'count(*)'), false) AS filtered_on_member;
SELECT same(kq('t.id BETWEEN 90 AND 110 AND CAST(r.value AS float8) > 2', 'count(*)'), false) AS filtered_on_clean_member;
SELECT same(kq('t.id BETWEEN 990 AND 1010 AND CAST(r.value AS float8) + 1 > 3', 'count(*)'), false) AS filtered_on_expression;
SELECT same(kq('t.id BETWEEN 990 AND 1010', 'sum(CAST(r.value AS float8)), sum(CAST(r.value AS integer))'), false) AS two_casts;
SELECT same(kq('t.id BETWEEN 990 AND 1010', 'count(*)', 'm_a', 'CAST(r.value AS integer) > 3')) AS cast_in_group_key;
SELECT same(kq('t.id BETWEEN 90 AND 110', 'count(*), min(t.id)', 'm_a', 'CAST(r.value AS integer) > 3')) AS cast_in_group_key_clean;

-- the writer's rows
INSERT INTO events VALUES (4001, 'cat_a', 1, '{"m_a": "5", "m_b": 1}'), (4002, 'cat_b', 1, '{"m_a": 3, "m_b": "n/a"}'), (4003, 'cat_c', 1, '{"m_a": 4, "m_b": 2}');
SELECT same(kq('t.id = 4001', 'sum(CAST(r.value AS float8))')) AS buffer_selected_dirty;
SELECT same(kq('t.id = 4002', 'sum(CAST(r.value AS float8))')) AS buffer_other_dirty;
SELECT same(kq('t.id = 4003', 'sum(CAST(r.value AS float8))')) AS buffer_clean;
SELECT same(kq('t.id > 4000 AND t.id <> 4001', 'sum(CAST(r.value AS float8))')) AS buffer_dirty_dropped;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '"z"') WHERE id IN (100, 4003);
UPDATE events SET doc = jsonb_set(doc, '{m_b}', '2') WHERE id = 100;
SELECT same(kq('t.id BETWEEN 90 AND 110', 'sum(CAST(r.value AS float8))')) AS after_update;
SELECT same(kq('t.id = 4003', 'sum(CAST(r.value AS float8))')) AS after_update_buffer;
UPDATE events SET doc = jsonb_set(doc, '{m_a}', '3') WHERE id IN (100, 4003);
SELECT same(kq('t.id BETWEEN 90 AND 110', 'sum(CAST(r.value AS float8))')) AS healed;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT same(kq('t.id BETWEEN 90 AND 1010', 'sum(CAST(r.value AS float8))')) AS after_promotion;
SELECT same(kq('t.id BETWEEN 4001 AND 4003', 'sum(CAST(r.value AS float8))')) AS after_promotion_buffer;
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT same(kq('t.id BETWEEN 90 AND 1010', 'sum(CAST(r.value AS float8))')) AS after_merge;
DELETE FROM events WHERE id IN (1000, 4001);
VACUUM events;
SELECT same(kq('t.id BETWEEN 1001 AND 1300', 'sum(CAST(r.value AS float8))')) AS after_vacuum;

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT same(kq('t.id BETWEEN 1001 AND 1300', 'sum(CAST(r.value AS float8))')) AS workers_error;
SELECT same(kq('t.id BETWEEN 90 AND 110', 'sum(CAST(r.value AS float8))')) AS workers_other_member;
SELECT same(kq('t.id < 1100', 'sum(CAST(r.value AS float8)), count(*)')) AS workers_whole;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;
DROP TABLE events;

-- a JSON null member casts to NULL from PostgreSQL 18 and raises before: the answer is the normal plan's either way
CREATE TABLE nulls (id int PRIMARY KEY, kind text, day int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO nulls SELECT i, 'cat_' || chr(97 + i % 3), i / 10, jsonb_build_object('m_a', i % 7, 'm_b', i % 5) FROM generate_series(1, 2000) i;
UPDATE nulls SET doc = jsonb_set(doc, '{m_a}', 'null') WHERE id = 100;
UPDATE nulls SET doc = jsonb_set(doc, '{m_b}', '"x"') WHERE id = 200;
CREATE INDEX nulls_col ON nulls USING sift_col (id, kind, day, doc) WITH (paths = 'doc.* float8');
VACUUM ANALYZE nulls;
SELECT answers('SELECT t.kind, sum(CAST(r.value AS float8)), count(*) FROM nulls t LEFT JOIN LATERAL jsonb_each(t.doc) r ON r.key = $$m_a$$ WHERE t.id BETWEEN 90 AND 110 GROUP BY 1') AS json_null_member;
SELECT answers('SELECT t.kind, sum(CAST(r.value AS float8)), count(*) FROM nulls t LEFT JOIN LATERAL jsonb_each(t.doc) r ON r.key = $$m_a$$ WHERE t.id BETWEEN 190 AND 210 GROUP BY 1') AS other_member_string;
DROP TABLE nulls;

DROP FUNCTION kq(text, text, text, text);
DROP FUNCTION ex(text);
DROP FUNCTION iq(text, text, text);
DROP FUNCTION pushed(text);
DROP FUNCTION answers(text);
DROP FUNCTION same(text, bool);
DROP FUNCTION runq(text, bool), outcome, runs_in_scan;
DROP EXTENSION pg_sift;
