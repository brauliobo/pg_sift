-- What a statement reads of the keys of a jsonb document when some documents hold what the leaf of the key cannot store (a numeric string, a boolean, a container, an array or a scalar root, a value
-- of another type than the column of a recordset), as SiftAggV6 answers it from the index of format major 2 without the settings of the tests: the `->>` text of a key and its float8 cast in the
-- WHERE clause, the keys, the arguments and the FILTER of a statement, and jsonb_to_recordset of `doc -> 'key'`. Every statement runs over the index and over a heap twin (the truth), errors included:
-- the first word says that the answers agree (the text of an error is part of the answer), the second whether the plan is the node's. A cast raises for a row that PostgreSQL's own plan reaches
-- with it and for no other: where the node cannot be sure of the order of PostgreSQL's conditions it keeps the normal plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

-- k: the value of the key is of each JSON type in turn; the rows of the groups 2 to 6 hold a text that is no float8 or a container, the others what float8 reads
CREATE TABLE jh (id int, grp int, m int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO jh
SELECT i, i % 12, i % 9,
	jsonb_build_object('k', CASE i % 12
			WHEN 0 THEN to_jsonb(i * 0.5)
			WHEN 1 THEN to_jsonb((i % 10)::text)
			WHEN 2 THEN to_jsonb('abc'::text)
			WHEN 3 THEN '{"x": 1}'::jsonb
			WHEN 4 THEN '[1, 2]'::jsonb
			WHEN 5 THEN 'null'::jsonb
			WHEN 6 THEN 'true'::jsonb
			WHEN 7 THEN '"NaN"'::jsonb
			WHEN 8 THEN '"Infinity"'::jsonb
			WHEN 9 THEN to_jsonb(' ' || (i % 10) || ' ')
			WHEN 10 THEN to_jsonb(i)
			ELSE to_jsonb(i % 7) END,
		's', CASE WHEN i % 5 = 0 THEN to_jsonb(i) ELSE to_jsonb('v' || i % 5) END,
		'l', CASE i % 9 WHEN 0 THEN '5'::jsonb WHEN 1 THEN 'null'::jsonb WHEN 2 THEN '{"k": "a"}'::jsonb WHEN 3 THEN '[]'::jsonb WHEN 4 THEN '[1]'::jsonb
			WHEN 5 THEN '[{"k": "z", "v": "3", "n": "4", "ok": "t"}, {"k": 1}]'::jsonb
			ELSE jsonb_build_array(jsonb_build_object('k', 'a' || i % 3, 'v', i * 0.5, 'n', i % 5, 'ok', i % 2 = 0), jsonb_build_object('k', 'b', 'v', i, 'n', 7, 'ok', true)) END)
FROM generate_series(1, 3000) i;
UPDATE jh SET doc = '[1, 2]' WHERE id % 211 = 0;
UPDATE jh SET doc = '"scalar"' WHERE id % 223 = 0;
UPDATE jh SET doc = NULL WHERE id % 227 = 0;
CREATE TABLE jc (LIKE jh) WITH (autovacuum_enabled = false);
INSERT INTO jc SELECT * FROM jh;
CREATE INDEX jc_col ON jc USING sift_col (id, grp, m, doc) WITH (paths = 'doc->k float8, doc->s text, doc->l[]{k text, v float8, n int4, ok bool}');
VACUUM (FREEZE, ANALYZE) jh;
VACUUM (FREEZE, ANALYZE) jc;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; line text; node boolean := false;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'jc') LOOP node := node OR line LIKE '%SiftAgg%'; END LOOP;
	BEGIN
		EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', 'jc')) INTO a;
	EXCEPTION WHEN OTHERS THEN a := 'error: ' || SQLERRM;
	END;
	BEGIN
		EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', 'jh')) INTO b;
	EXCEPTION WHEN OTHERS THEN b := 'error: ' || SQLERRM;
	END;
	RETURN (CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END) || ' ' || (CASE WHEN node THEN 'node' ELSE 'normal' END) || ' ' || q ||
		CASE WHEN b LIKE 'error: %' THEN ' => ' || b ELSE '' END;
END $$;

-- the float8 cast of a key in the WHERE clause, beside conditions the scan keys decide first: the groups that raise are kept out, or are not
SELECT same(q) FROM unnest(ARRAY[
	'SELECT grp, count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (0, 1, 5, 7, 8) GROUP BY grp',
	'SELECT count(*), sum(id) FROM @T WHERE (doc->>''k'')::float8 <= 3 AND grp <> 2 AND grp <> 3 AND grp <> 4 AND grp <> 6',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 IS NULL AND grp IN (0, 1, 5, 7, 8)',
	'SELECT grp, count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (0, 1, 2) GROUP BY grp',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (3, 4)',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 > 1',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 = ''NaN'' AND grp IN (7, 8)',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (0, 1, 5, 7, 8) AND grp <> 100'
]) q;

-- a condition that PostgreSQL may test after the cast (written before it with the same cost, or dearer: the list of eight is), another that reads the key, an OR: PostgreSQL's order of the rows decides the error
SELECT same(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (0, 1, 5, 7, 8, 9, 10, 11) AND id > 100',
	'SELECT count(*) FROM @T WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11) AND (doc->>''k'')::float8 > 1',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND (doc->>''s'' = ''v1'' OR grp IN (0, 1, 5, 7, 8, 9, 10, 11))',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 OR grp IN (0, 1, 5, 7, 8, 9, 10, 11)',
	'SELECT count(*) FROM @T WHERE (doc->>''k'')::float8 + (doc->>''k'')::float8 > 2 AND grp IN (0, 1, 5, 7, 8, 9, 10, 11)',
	'SELECT count(*) FROM @T WHERE (CASE WHEN grp > 3 THEN (doc->>''k'')::float8 END) > 1 AND grp IN (0, 1, 5, 7, 8, 9, 10, 11)'
]) q;

-- the cast as an argument, a key and a FILTER; the groups that raise are in or out of the statement
SELECT same(q) FROM unnest(ARRAY[
	'SELECT grp, sum((doc->>''k'')::float8), avg((doc->>''k'')::float8), min((doc->>''k'')::float8), max((doc->>''k'')::float8) FROM @T WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11) GROUP BY grp',
	'SELECT count(*), sum((doc->>''k'')::float8) FROM @T WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11) AND id > 20',
	'SELECT (doc->>''k'')::float8 AS k, count(*) FROM @T WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11) GROUP BY 1',
	'SELECT grp, sum((doc->>''k'')::float8 * 2 + 1) FROM @T WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11) GROUP BY 1',
	'SELECT grp, sum((doc->>''k'')::float8) FROM @T GROUP BY grp',
	'SELECT sum((doc->>''k'')::float8) FROM @T WHERE grp = 3',
	'SELECT grp, sum((doc->>''k'')::float8) FILTER (WHERE id > 50) FROM @T WHERE grp IN (0, 1) GROUP BY grp',
	'SELECT count(*) FILTER (WHERE grp IN (0, 1, 5)), sum((doc->>''k'')::float8) FILTER (WHERE grp IN (0, 1, 5)) FROM @T',
	'SELECT sum((doc->>''k'')::float8) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (0, 1, 5, 7, 8, 9, 10, 11)',
	'SELECT sum((doc->>''s'')::float8) FROM @T WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11)'
]) q;

-- the text of a key: a number, a boolean or a document is its text where the leaf holds strings, and no cast raises
SELECT same(q) FROM unnest(ARRAY[
	'SELECT doc->>''s'' AS s, count(*) FROM @T GROUP BY 1',
	'SELECT count(*) FROM @T WHERE doc->>''s'' = ''v1''',
	'SELECT count(*) FROM @T WHERE doc->>''s'' = ''10''',
	'SELECT count(*) FROM @T WHERE doc->>''s'' > ''v2'' AND grp < 6',
	'SELECT count(*) FROM @T WHERE doc->>''k'' IS NULL',
	'SELECT doc->>''k'' AS k, count(*) FROM @T WHERE grp IN (2, 3, 4, 5, 6) GROUP BY 1',
	'SELECT count(*), count(doc->>''s'') FILTER (WHERE id > 100) FROM @T WHERE doc->>''s'' <> ''v3'''
]) q;

-- jsonb_to_recordset of a key of the document: the groups that raise are read again from the heap and take the function's own errors
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.k, count(*), sum(o.v) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8, n int4, ok bool) WHERE d.grp = 4 GROUP BY o.k',
	'SELECT o.k, count(*), sum(o.n) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8, n int4, ok bool) WHERE d.grp = 9 GROUP BY o.k',
	'SELECT count(*), sum(o.v) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m IN (6, 7, 8) AND d.grp < 12 AND d.id > 0',
	'SELECT count(*) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m = 0',
	'SELECT count(*) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m = 1',
	'SELECT count(*) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m = 2',
	'SELECT count(*) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m = 3',
	'SELECT count(*) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m = 4',
	'SELECT count(*) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8, n int4) WHERE d.m = 5',
	'SELECT d.grp, count(*), count(o.k) FROM @T d LEFT JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) ON true WHERE d.m IN (3, 6, 7, 8) GROUP BY 1'
]) q;

-- the same, over the rows that the index holds whole (no document it cannot take apart) and the rows that were left to the heap: the statement counts them
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.k, o.ok, count(*), sum(o.v), sum(o.n) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8, n int4, ok bool) WHERE d.m IN (6, 7, 8) GROUP BY 1, 2',
	'SELECT d.grp, count(*), sum(o.v) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m IN (6, 7, 8) AND o.v > 100 GROUP BY 1'
]) q;

CREATE FUNCTION reread(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; n text := '-';
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off, BUFFERS off) ' || q LOOP
		IF line LIKE '%Rows Read Again From The Heap: %' THEN n := trim(substr(line, position('Heap: ' in line) + 6)); END IF;
	END LOOP;
	RETURN n;
END $$;
CREATE TABLE jq AS SELECT * FROM jh WHERE m IN (6, 7, 8) AND id % 211 <> 0 AND id % 223 <> 0 AND id % 227 <> 0;
CREATE INDEX jq_col ON jq USING sift_col (id, grp, m, doc) WITH (paths = 'doc->k float8, doc->s text, doc->l[]{k text, v float8, n int4, ok bool}');
VACUUM (FREEZE, ANALYZE) jq;
SELECT reread('SELECT o.k, count(*) FROM jq d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) GROUP BY 1') AS clean_rows_read_again;
SELECT reread('SELECT o.k, count(*) FROM jc d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m = 5 GROUP BY 1') <> '0' AS dirty_rows_read_again;

-- rows that change: the new document, a deleted row and a vacuumed index answer as the heap does
UPDATE jh SET doc = jsonb_set(doc, '{k}', '"zzz"') WHERE id % 97 = 0;
UPDATE jc SET doc = jsonb_set(doc, '{k}', '"zzz"') WHERE id % 97 = 0;
DELETE FROM jh WHERE id % 89 = 0;
DELETE FROM jc WHERE id % 89 = 0;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT grp, count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (0, 1, 5, 7, 8, 9, 10, 11) GROUP BY grp',
	'SELECT grp, sum((doc->>''k'')::float8) FROM @T WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11) GROUP BY grp',
	'SELECT count(*), sum(o.v) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m IN (6, 7, 8)'
]) q;
VACUUM jc;
VACUUM jh;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT grp, count(*) FROM @T WHERE (doc->>''k'')::float8 > 1 AND grp IN (0, 1, 5, 7, 8, 9, 10, 11) GROUP BY grp',
	'SELECT count(*), sum(o.v) FROM @T d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> ''l'') AS o(k text, v float8) WHERE d.m IN (6, 7, 8)'
]) q;

DROP FUNCTION same(text), reread(text);
DROP TABLE jq, jc, jh;
DROP EXTENSION pg_sift;
