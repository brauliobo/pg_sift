-- The jsonb element functions over a sift_table (sift.table_set_paths ... doc->items[]->kind text, doc->items[]->score float8): the table keeps the array of a row whole and the scan takes it apart
-- with the function itself, so jsonb_to_recordset, jsonb_array_elements read by members and jsonb_array_elements_text run in SiftAggV6 like unnest(array), on both formats.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

\ir test/common/sql/agg/base.sql

\ir test/common/sql/agg/same.sql

CREATE FUNCTION used(q text) RETURNS bool LANGUAGE sql AS $$ SELECT coalesce(bool_or(used), false) FROM sift.explain_why(q) $$;

CREATE TABLE tj (id int, qty int, doc jsonb, tags text[]) USING sift_table;
SELECT sift.table_set_paths('tj', 'doc', 'doc->channel text, doc->k int4, doc->items[]->kind text, doc->items[]->score float8');
INSERT INTO tj SELECT g, g % 5, jsonb_build_object('channel', 'ch_' || g % 4, 'k', g % 11,
	'items', (SELECT jsonb_agg(jsonb_build_object('kind', 'k' || (g + j) % 3, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 3) j), 'tags', jsonb_build_array('t' || g % 5, 't' || g % 3)), ARRAY['t' || g % 4]
FROM generate_series(1, 6000) g;
INSERT INTO tj VALUES (7001, 1, '{"items": [{"kind": "k1", "score": "7"}, {}]}', NULL), (7002, 2, '{"channel": "ch_0"}', ARRAY['t1', NULL]), (7003, 3, NULL, NULL), (7004, 4, '{"items": [], "tags": [null]}', '{}');
ANALYZE tj;
CREATE TABLE tx (id int, qty int, doc jsonb) USING sift_table;
INSERT INTO tx SELECT id, qty, doc FROM tj WHERE id <= 3000;
INSERT INTO tx VALUES (7005, 1, '{"items": [5, {"kind": "k1", "score": "abc"}]}');
ANALYZE tx;

-- an array that is the column itself, whose declared list paths keep the lengths of the arrays: the rows its elements make are estimated from them, not at the 100 a call
-- PostgreSQL assumes, so that a two-level rollup over the elements is no join of 100 times the rows it has
CREATE TABLE ti (id int, grp int, items jsonb) USING sift_table;
SELECT sift.table_set_paths('ti', 'items', 'items[]->kind text, items[]->qty int4');
INSERT INTO ti SELECT g, g % 7, (SELECT jsonb_agg(jsonb_build_object('kind', 'k' || (g + j) % 4, 'qty', j)) FROM generate_series(1, 1 + g % 3) j) FROM generate_series(1, 6000) g;
VACUUM ANALYZE ti;

CREATE TABLE kinds (kind text);
INSERT INTO kinds VALUES ('k1'), ('t2');
ANALYZE kinds;

CREATE TABLE q (n int, sql text);
INSERT INTO q VALUES
	(1, $$SELECT i.kind, count(*), sum(i.score) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) GROUP BY 1$$),
	(2, $$SELECT i.kind, count(*), avg(i.score) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) WHERE i.kind = 'k1' GROUP BY 1$$),
	(3, $$SELECT t.qty, count(i.kind), count(*) FROM tj t LEFT JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) ON true GROUP BY 1$$),
	(4, $$SELECT e ->> 'kind', count(*), sum((e ->> 'score')::float8) FROM tj t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'items') e GROUP BY 1$$),
	(5, $$SELECT t.qty, count(*) FROM tj t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'items') e WHERE e ->> 'kind' = 'k2' GROUP BY 1$$),
	(6, $$SELECT v, count(*) FROM tj t CROSS JOIN LATERAL jsonb_array_elements_text(t.doc -> 'tags') v GROUP BY 1$$),
	(7, $$SELECT t.qty, i.kind, max(i.score) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) WHERE t.qty > 1 GROUP BY 1, 2$$),
	-- the errors are the function's and the statement's: an element that is no object, a score that is no number
	(8, $$SELECT i.kind, count(*) FROM tx t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) GROUP BY 1$$),
	(9, $$SELECT e ->> 'kind', sum((e ->> 'score')::float8) FROM tx t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'items') e GROUP BY 1$$),
	(10, $$SELECT e ->> 'kind', count(*) FROM tx t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'items') e GROUP BY 1$$),
	-- expressions of the columns of the table and of the element are computed on the pairs, and the heap row each pair comes from decides any condition of the table
	(11, $$SELECT t.qty, sum(t.qty * i.score) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) GROUP BY 1$$),
	(12, $$SELECT i.kind, sum(i.score) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) WHERE t.doc ->> 'channel' = 'ch_1' GROUP BY 1$$),
	(15, $$SELECT coalesce(t.doc ->> 'channel', '-') || '/' || i.kind, count(*), sum(i.score * t.qty) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8)
		WHERE t.doc ->> 'channel' IN ('ch_1', 'ch_2') OR t.qty IS NULL GROUP BY 1$$),
	-- the members of a map
	(16, $$SELECT m.key, count(*), sum(m.value::float8) FROM tj t CROSS JOIN LATERAL jsonb_each_text(t.doc) m WHERE m.key = 'k' GROUP BY 1$$),
	(17, $$SELECT m.key, count(*), avg((m.value #>> '{}')::float8) FROM tj t CROSS JOIN LATERAL jsonb_each(t.doc) m WHERE m.key IN ('k', 'zz') GROUP BY 1$$),
	-- the elements an EXISTS tests against a column of the row
	(18, $$SELECT t.qty, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(t.doc -> 'items') i(score int) WHERE i.score > t.qty)) FROM tj t GROUP BY 1$$),
	(19, $$SELECT t.qty, count(*) FROM tj t WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(t.doc -> 'items') i(kind text) WHERE i.kind = 'k1') GROUP BY 1$$),
	-- containment as tests of the leaves of the declared paths; its negation also tests that the document is no SQL NULL, which the batches of a table do not: the normal plan
	(20, $$SELECT t.qty, count(*) FROM tj t WHERE t.doc @> '{"channel": "ch_1", "k": 3}' GROUP BY 1$$),
	(21, $$SELECT t.qty, count(*) FROM tj t WHERE NOT (t.doc @> '{"channel": "ch_0"}') GROUP BY 1$$),
	-- jsonpath and containment tests of a row's arrays: the call decides each row of the table, so != and !(), NOT of the test, strict mode, variables,
	-- arrays of two keys and an object of two elements answer as PostgreSQL does, errors included
	(22, $$SELECT t.qty, count(*) FROM tj t WHERE jsonb_path_exists(t.doc, '$.items[*] ? (@.kind == "k1" && @.score > 4)') GROUP BY 1$$),
	(23, $$SELECT t.qty, count(*) FILTER (WHERE t.doc @? '$.items[*] ? (!(@.kind == "k1"))'), count(*) FILTER (WHERE NOT (t.doc @? '$.items[*] ? (@.kind != "k2")'))
		FROM tj t GROUP BY 1$$),
	(24, $$SELECT t.qty, count(*) FILTER (WHERE jsonb_path_exists(t.doc, '$.items[*] ? (@.score == $s)', '{"s": 3}')),
		count(*) FILTER (WHERE jsonb_path_exists(t.doc, '$.tags[*] ? (@ == "t1")')), count(*) FILTER (WHERE t.doc @> '{"items": [{"kind": "k0"}, {"kind": "k2"}]}') FROM tj t GROUP BY 1$$),
	(25, $$SELECT count(*) FILTER (WHERE jsonb_path_exists(t.doc, 'strict $.items[*] ? (@.kind == "k1")')) FROM tj t$$),
	(26, $$SELECT count(*) FROM tx t WHERE jsonb_path_exists(t.doc, 'strict $.items[*] ? (@.score == "abc")')$$),
	-- a dimension joined to the elements of a table is the normal plan's: the pairs of its rows probe no dimension
	(13, $$SELECT i.kind, count(*) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) WHERE EXISTS (SELECT 1 FROM kinds k WHERE k.kind = i.kind) GROUP BY 1$$),
	(14, $$SELECT v, count(*) FROM tj t CROSS JOIN LATERAL unnest(t.tags) v WHERE EXISTS (SELECT 1 FROM kinds k WHERE k.kind = v) GROUP BY 1$$),
	-- a two-level rollup over the elements of each row: the inner groups of the order and its kind, and of the order alone
	(27, $$WITH base AS (SELECT t.id, t.grp, i.kind, i.qty FROM ti t CROSS JOIN LATERAL jsonb_to_recordset(t.items) i(kind text, qty int) WHERE t.grp < 5)
		SELECT count(DISTINCT id), count(*), sum(units) FROM (SELECT id, kind, sum(qty) AS units FROM base GROUP BY id, kind UNION ALL SELECT id, NULL, sum(qty) FROM base GROUP BY id) x$$),
	-- a key the pairs compute as a jsonb from a column of the table beside the elements
	(28, $$SELECT i.kind, jsonb_build_object('qty', t.qty) AS q, count(*), sum(i.score) FROM tj t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'items') i(kind text, score float8) GROUP BY 1, 2$$);
-- the plan the scan has over a table of this size where reading its pages costs what the scan does
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SELECT n, same(sql), pushed(sql) AS scan, used(sql) FROM q ORDER BY n;

-- the elements of a declared list come from the store of the table: a row is read again by its address only when the leaves cannot hold its array (tx has an element that is no object)
CREATE FUNCTION reread(q text) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows Read Again From The Heap' THEN RETURN substring(l FROM '\d+$')::int8; END IF;
	END LOOP;
	RETURN NULL;
END $$;
SELECT n, reread(sql) FROM q WHERE n IN (1, 3, 4, 6, 7, 10) ORDER BY n;

-- an update of a column beside the list (a column delta), a delete and an aborted insert are what the snapshot of the scan sees
UPDATE tj SET qty = qty + 1 WHERE id % 7 = 0;
DELETE FROM tj WHERE id % 11 = 0;
BEGIN;
INSERT INTO tj SELECT id + 10000, qty, doc FROM tj WHERE id <= 500;
ROLLBACK;
SELECT n, same(sql), pushed(sql) AS scan FROM q WHERE n IN (1, 3, 7, 11) ORDER BY n;

-- a numeric beside an EXISTS over the elements: the batches of an unnest hold no numeric read as datums, so a group key computed from it keeps its values
CREATE TABLE tn (id int, num numeric(12, 2), doc jsonb) USING sift_table;
SELECT sift.table_set_paths('tn', 'doc', 'doc->items[]->score float8');
INSERT INTO tn SELECT id, (id % 997) / 7.0, doc FROM tj WHERE id <= 3000;
ANALYZE tn;
SELECT same($$SELECT floor(t.num / 50) AS k, count(*) FROM tn t WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(t.doc -> 'items') p WHERE (p ->> 'score')::float8 > 2) GROUP BY 1 ORDER BY 1$$)
	AS numeric_key_beside_exists;
DROP TABLE tn;

-- at the default page costs the scan of a list the table declares costs less than the normal plan, and one that reads every row again by its address (a member no path declares) does not
RESET seq_page_cost;
RESET random_page_cost;
SELECT pushed($$SELECT i.kind, count(*), sum(i.qty) FROM ti t CROSS JOIN LATERAL jsonb_to_recordset(t.items) i(kind text, qty int) GROUP BY 1$$) AS declared,
	pushed($$SELECT i.kind, count(*), max(i.note) FROM ti t CROSS JOIN LATERAL jsonb_to_recordset(t.items) i(kind text, note text) GROUP BY 1$$) AS undeclared;

DROP TABLE q, tj, tx, ti, kinds;
DROP FUNCTION used(text), reread(text), runq(text, bool), same(text), pushed(text), outcome(text, bool), runs_in_scan(text);
DROP EXTENSION pg_sift;
