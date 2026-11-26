-- Arrays of objects, maps and scalar paths of a jsonb column shredded into the granules of format major 2: the lengths of an array and the typed columns of its elements are stored
-- apart, a value a leaf cannot hold (a type mismatch, a number out of range, an array where an object is declared) is an exception whose value sits inline beside its granule. Every leaf
-- of every row is read back from the parts and compared with what the heap says, through the build, inserts, updates, deletes, VACUUM and REINDEX.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION tcode(j jsonb) RETURNS int LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE jsonb_typeof(j) WHEN 'string' THEN 1 WHEN 'number' THEN 2 WHEN 'boolean' THEN 3 WHEN 'object' THEN 4 WHEN 'array' THEN 5 WHEN 'null' THEN 7 END $$;
CREATE FUNCTION arr(j jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN jsonb_typeof(j) = 'array' THEN j ELSE '[]' END $$;
CREATE FUNCTION obj(j jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN jsonb_typeof(j) = 'object' THEN j ELSE '{}' END $$;
CREATE FUNCTION maybe(k text, v jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN v IS NULL THEN '{}' ELSE jsonb_build_object(k, v) END $$;

-- An array or a scalar where an object is declared (an element of a list of objects): the leaves below it have no value at their path (tag 6)
-- What a leaf of a declared type holds for the value v found at its path (SQL NULL: the path leads to nothing): the value, the tag of a slot with none, the value an exception keeps
CREATE FUNCTION leaf_text(v jsonb, OUT val text, OUT tag int, OUT pay text) LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN jsonb_typeof(v) = 'string' THEN v #>> '{}' END,
		   CASE WHEN v IS NULL OR jsonb_typeof(v) = 'string' THEN NULL ELSE tcode(v) END,
		   CASE WHEN v IS NOT NULL AND jsonb_typeof(v) NOT IN ('string', 'null') THEN v::text END $$;
CREATE FUNCTION leaf_f8(v jsonb, OUT val text, OUT tag int, OUT pay text) LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN jsonb_typeof(v) = 'number' AND abs((v #>> '{}')::numeric) < 1e300 THEN v #>> '{}' END,
		   CASE WHEN v IS NULL OR (jsonb_typeof(v) = 'number' AND abs((v #>> '{}')::numeric) < 1e300) THEN NULL ELSE tcode(v) END,
		   CASE WHEN v IS NOT NULL AND jsonb_typeof(v) <> 'null' AND NOT (jsonb_typeof(v) = 'number' AND abs((v #>> '{}')::numeric) < 1e300) THEN v::text END $$;
CREATE FUNCTION leaf_astext(v jsonb, OUT val text, OUT tag int, OUT pay text) LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN jsonb_typeof(v) IN ('string', 'number', 'boolean') THEN v #>> '{}' END,
		   CASE WHEN v IS NULL OR jsonb_typeof(v) = 'string' THEN NULL ELSE tcode(v) END,
		   CASE WHEN jsonb_typeof(v) IN ('object', 'array') THEN v::text END $$;

CREATE FUNCTION mkdoc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT maybe('title', CASE i % 11 WHEN 0 THEN to_jsonb(i) WHEN 1 THEN 'null' WHEN 2 THEN NULL WHEN 3 THEN '[1]' ELSE to_jsonb('t' || (i % 13)) END)
		|| maybe('score', CASE i % 9 WHEN 0 THEN '"x"' WHEN 1 THEN 'true' WHEN 2 THEN NULL WHEN 3 THEN to_jsonb(1e400::numeric) ELSE to_jsonb(i * 0.25) END)
		|| maybe('notes', CASE i % 8 WHEN 0 THEN NULL WHEN 1 THEN '"nope"' WHEN 2 THEN '[]' WHEN 3 THEN '{"a": 1}' ELSE
			(SELECT coalesce(jsonb_agg(CASE (i + j) % 9 WHEN 0 THEN '{"kind": 5}'::jsonb WHEN 1 THEN '{"kind": null}' WHEN 2 THEN '{}' WHEN 3 THEN CASE WHEN j % 2 = 0 THEN '"x"'::jsonb ELSE '[1]'::jsonb END
				WHEN 4 THEN '{"kind": {"q": [1]}}' WHEN 5 THEN '{"kind": true}' ELSE jsonb_build_object('kind', 'k' || ((i + j) % 5), 'extra', j) END), '[]')
			 FROM generate_series(1, i % 7) j) END)
		|| maybe('metrics', CASE i % 6 WHEN 0 THEN NULL WHEN 1 THEN '[1]' WHEN 2 THEN '{}' ELSE
			(SELECT jsonb_object_agg('m' || j, CASE WHEN (i + j) % 7 = 0 THEN '"bad"'::jsonb WHEN (i + j) % 11 = 0 THEN 'null' WHEN (i + j) % 13 = 0 THEN to_jsonb(1e400::numeric)
				ELSE to_jsonb(j * 1.5) END) FROM generate_series(1, i % 4 + 1) j) END)
		|| maybe('analysis', CASE i % 5 WHEN 0 THEN NULL WHEN 1 THEN '{"entries": "nope"}' WHEN 2 THEN '{"entries": []}' ELSE
			jsonb_build_object('entries', (SELECT coalesce(jsonb_agg(CASE (i + j) % 10
				WHEN 0 THEN '{"cat": 1, "sub": true, "pol": {"x": 1}, "score": "s"}'::jsonb
				WHEN 1 THEN '{"cat": null, "sub": [1, 2], "pol": "p", "score": null}'
				WHEN 2 THEN '[1]'
				WHEN 3 THEN '{"score": 1e400}'
				ELSE jsonb_build_object('cat', 'c' || ((i + j) % 4), 'sub', (i + j) % 3, 'pol', 'p' || (j % 2), 'score', j * 0.5) END), '[]')
			 FROM generate_series(1, i % 4 + 1) j)) END) $$;

CREATE TABLE docs (id int, doc jsonb);
INSERT INTO docs SELECT i, mkdoc(i) FROM generate_series(1, 6000) i;
INSERT INTO docs VALUES (6001, NULL), (6002, 'null'), (6003, '{}');
CREATE INDEX docs_i ON docs USING sift_col (id, doc) WITH (key_order = 'id',
	paths = 'doc.title text, doc.score float8, doc.notes[]{kind text}, doc.metrics.* float8, doc.analysis.entries[]{cat astext, sub astext, pol astext, score float8}');

-- What every leaf of every row of the heap holds: (tid, leaf, element, value, tag, exception value)
CREATE VIEW expected AS
	SELECT d.ctid AS tid, 'id'::text AS leaf, NULL::int AS elem, d.id::text AS val, NULL::int AS tag, NULL::text AS pay FROM docs d
	UNION ALL SELECT d.ctid, 'doc.title', NULL, t.val, t.tag, t.pay FROM docs d, leaf_text(d.doc -> 'title') t
	UNION ALL SELECT d.ctid, 'doc.score', NULL, t.val, t.tag, t.pay FROM docs d, leaf_f8(d.doc -> 'score') t
	UNION ALL SELECT d.ctid, 'doc.notes[]', NULL, CASE WHEN jsonb_typeof(n) = 'array' THEN jsonb_array_length(n)::text END,
			CASE WHEN n IS NOT NULL AND jsonb_typeof(n) NOT IN ('array', 'null') THEN tcode(n) END, CASE WHEN jsonb_typeof(n) IN ('string', 'number', 'boolean', 'object') THEN n::text END
		FROM docs d, LATERAL (SELECT d.doc -> 'notes') x(n)
	UNION ALL SELECT d.ctid, 'doc.notes[].kind', (e.k - 1)::int, t.val, CASE WHEN jsonb_typeof(e.v) <> 'object' THEN 6 ELSE t.tag END, t.pay
		FROM docs d, jsonb_array_elements(arr(d.doc -> 'notes')) WITH ORDINALITY e(v, k), leaf_text(CASE WHEN jsonb_typeof(e.v) = 'object' THEN e.v -> 'kind' END) t
	UNION ALL SELECT d.ctid, 'doc.metrics.*', NULL, CASE WHEN jsonb_typeof(m) = 'object' THEN (SELECT count(*) FROM jsonb_object_keys(m))::text END,
			CASE WHEN m IS NOT NULL AND jsonb_typeof(m) NOT IN ('object', 'null') THEN tcode(m) END, CASE WHEN jsonb_typeof(m) IN ('string', 'number', 'boolean', 'array') THEN m::text END
		FROM docs d, LATERAL (SELECT d.doc -> 'metrics') x(m)
	UNION ALL SELECT d.ctid, 'doc.metrics.*', (e.k - 1)::int, t.val, t.tag, t.pay FROM docs d, jsonb_each(obj(d.doc -> 'metrics')) WITH ORDINALITY e(key, v, k), leaf_f8(e.v) t
	UNION ALL SELECT d.ctid, 'doc.metrics.*#key', (e.k - 1)::int, e.key, NULL, NULL FROM docs d, jsonb_each(obj(d.doc -> 'metrics')) WITH ORDINALITY e(key, v, k)
	UNION ALL SELECT d.ctid, 'doc.analysis.entries[]', NULL, CASE WHEN jsonb_typeof(n) = 'array' THEN jsonb_array_length(n)::text END,
			CASE WHEN n IS NOT NULL AND jsonb_typeof(n) NOT IN ('array', 'null') THEN tcode(n) END, CASE WHEN jsonb_typeof(n) IN ('string', 'number', 'boolean', 'object') THEN n::text END
		FROM docs d, LATERAL (SELECT d.doc #> '{analysis,entries}') x(n)
	UNION ALL SELECT d.ctid, 'doc.analysis.entries[].' || c.leaf, (e.k - 1)::int, t.val, CASE WHEN jsonb_typeof(e.v) <> 'object' THEN 6 ELSE t.tag END, t.pay
		FROM docs d, jsonb_array_elements(arr(d.doc #> '{analysis,entries}')) WITH ORDINALITY e(v, k), (VALUES ('cat'), ('sub'), ('pol'), ('score')) c(leaf),
			LATERAL (SELECT * FROM leaf_astext(CASE WHEN jsonb_typeof(e.v) = 'object' THEN e.v -> c.leaf END) WHERE c.leaf <> 'score'
					 UNION ALL SELECT * FROM leaf_f8(CASE WHEN jsonb_typeof(e.v) = 'object' THEN e.v -> c.leaf END) WHERE c.leaf = 'score') t;

CREATE FUNCTION norm(leaf text, val text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN leaf IN ('doc.score', 'doc.metrics.*', 'doc.analysis.entries[].score') THEN val::float8::text ELSE val END $$;

CREATE VIEW actual AS
	SELECT tid, leaf, elem, val, tag, payload AS pay FROM sift._test_f2_leaves('docs_i')
	WHERE leaf IN ('id', 'doc.title', 'doc.score', 'doc.notes[]', 'doc.notes[].kind', 'doc.metrics.*', 'doc.metrics.*#key', 'doc.analysis.entries[]',
				   'doc.analysis.entries[].cat', 'doc.analysis.entries[].sub', 'doc.analysis.entries[].pol', 'doc.analysis.entries[].score');

-- The rows only one side has, and how many each has
CREATE FUNCTION leaf_diff() RETURNS TABLE(side text, leaf text, elem int, val text, tag int, pay text, n bigint) LANGUAGE sql AS $$
	WITH e AS (SELECT tid, leaf, elem, norm(leaf, val) val, tag, pay FROM expected),
		 a AS (SELECT tid, leaf, elem, norm(leaf, val) val, tag, pay FROM actual)
	SELECT 'heap only', leaf, elem, val, tag, pay, count(*) FROM (SELECT * FROM e EXCEPT ALL SELECT * FROM a) x GROUP BY 2, 3, 4, 5, 6
	UNION ALL SELECT 'index only', leaf, elem, val, tag, pay, count(*) FROM (SELECT * FROM a EXCEPT ALL SELECT * FROM e) x GROUP BY 2, 3, 4, 5, 6
	ORDER BY 1, 2, 3, 4, 5, 6 LIMIT 20 $$;

SELECT sift._test_f2_check_keys('docs_i') AS keys_built;
VACUUM docs;
SELECT sift._test_f2_check_keys('docs_i') AS keys_vacuumed;
SELECT * FROM leaf_diff();
SELECT sift._test_f2_array_layout('docs_i') AS layout_built;
SELECT leaf, count(*) FILTER (WHERE tag BETWEEN 1 AND 5) AS exceptions, count(*) FILTER (WHERE tag BETWEEN 1 AND 5 AND pay IS NOT NULL) AS with_payload
	FROM actual GROUP BY leaf ORDER BY leaf;
SELECT (SELECT count(*) FROM expected) = (SELECT count(*) FROM actual) AS same_leaf_count;

-- Inserts, updates and deletes, each folded into the parts before the leaves are read again
INSERT INTO docs SELECT i, mkdoc(i) FROM generate_series(6004, 7500) i;
VACUUM docs;
SELECT * FROM leaf_diff();
UPDATE docs SET doc = mkdoc(id + 3) WHERE id % 10 = 0;
UPDATE docs SET doc = NULL WHERE id % 97 = 0;
VACUUM docs;
SELECT * FROM leaf_diff();
DELETE FROM docs WHERE id % 7 = 0;
VACUUM docs;
SELECT * FROM leaf_diff();
SELECT sift._test_f2_check_keys('docs_i') AS keys_mutated;
SELECT sift._test_f2_array_layout('docs_i') AS layout_mutated;
SELECT (SELECT count(*) FROM expected) = (SELECT count(*) FROM actual) AS same_leaf_count_mutated;

REINDEX INDEX docs_i;
SELECT * FROM leaf_diff();
SELECT sift._test_f2_check_keys('docs_i') AS keys_reindexed;
SELECT sift._test_f2_check('docs_i') ~ '^parts=[0-9]+ ' AS consistent;

-- The index answers like the heap
SET enable_seqscan = off; SET enable_indexscan = off;
SELECT count(*) FROM docs WHERE id BETWEEN 1000 AND 2000;
SET enable_seqscan = on; SET enable_bitmapscan = off;
SELECT count(*) FROM docs WHERE id BETWEEN 1000 AND 2000;

DROP VIEW actual, expected;
DROP TABLE docs;
DROP FUNCTION leaf_diff(), norm(text, text), mkdoc(int), leaf_astext(jsonb), leaf_f8(jsonb), leaf_text(jsonb), maybe(text, jsonb), obj(jsonb), arr(jsonb), tcode(jsonb);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
