-- The JSON type of the values a leaf cannot hold: chunks of column format minor 4 keep it for every overflow
-- value, in segments and in the write buffer; a value written under minor 3 has none, and a merge carries that over.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;

CREATE TABLE d (id int, doc jsonb);
-- every path: a number, a string, a boolean, an object, an array, a JSON null, a missing key, and values out of range
CREATE FUNCTION doc(g int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS
$$ SELECT CASE g % 12
	WHEN 0 THEN '{"m": 1.5, "n": 3, "s": "x", "b": true}'
	WHEN 1 THEN '{"m": "str", "n": "a", "s": 5, "b": "no"}'
	WHEN 2 THEN '{"m": true, "n": 1.5, "s": true, "b": 3}'
	WHEN 3 THEN '{"m": {"a": 1}, "n": {}, "s": {"x": 1}, "b": {}}'
	WHEN 4 THEN '{"m": [1], "n": [], "s": [], "b": []}'
	WHEN 5 THEN '{"m": null, "n": null, "s": null, "b": null}'
	WHEN 6 THEN '{}'
	WHEN 7 THEN '{"m": 1e999, "n": 99999999999, "s": "y", "b": false}'
	WHEN 8 THEN '{"m": 2, "n": 4, "s": 1.5, "b": true}'
	ELSE '{"m": 2, "n": 4, "s": "z", "b": true}' END::jsonb $$;
INSERT INTO d SELECT g, doc(g) FROM generate_series(1, 30000) g;

CREATE FUNCTION fmt(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(format::text, ',' ORDER BY gen) FROM sift._test_col_formats(idx) $$;

-- the rows the index counts as of a JSON type at a leaf (the stored values when the leaf has that type) and the rows the heap has,
-- over every unit; allknown: no overflow value of any unit lacks its tag
CREATE FUNCTION types(idx regclass, leaf int, key text, own text)
RETURNS TABLE (type text, idx_rows int8, heap_rows int8, allknown bool) LANGUAGE sql AS
$$ SELECT t.name, sum(u.matched)::int8,
	(SELECT count(*) FROM d WHERE jsonb_typeof(doc->key) = t.name), bool_and(u.known)
	FROM (VALUES ('string', 1), ('number', 2), ('boolean', 3), ('object', 4), ('array', 5)) t(name, tag),
	LATERAL sift._test_col_types(idx, leaf, 1 << t.tag, t.name = own) u
	GROUP BY t.name, t.tag ORDER BY t.tag $$;
CREATE FUNCTION all_types(idx regclass) RETURNS TABLE (leaf text, type text, same bool, allknown bool) LANGUAGE sql AS
$$ SELECT k.key, r.type, r.idx_rows = r.heap_rows, r.allknown
	FROM (VALUES ('m', 1, 'number'), ('n', 2, 'number'), ('s', 3, 'string'), ('b', 4, 'boolean')) k(key, leaf, own),
	LATERAL types(idx, k.leaf, k.key, k.own) r ORDER BY k.leaf, r.type COLLATE "C" $$;

-- the units that overflow at leaf 1 (doc.m): a unit of the write buffer or of a segment whose overflow values all have their type, and a unit of a segment where some have none
CREATE FUNCTION unit_kinds(idx regclass, OUT buffered_known bool, OUT segment_known bool, OUT segment_unknown bool) LANGUAGE sql AS
$$ SELECT coalesce(bool_or(in_wbuf AND novf > 0 AND known), false), coalesce(bool_or(NOT in_wbuf AND novf > 0 AND known), false),
	coalesce(bool_or(NOT in_wbuf AND NOT known), false) FROM sift._test_col_types(idx, 1, 62, true) $$;

-- minor 4: the leaves id (0), doc.m float8 (1), doc.n int4 (2), doc.s text (3), doc.b bool (4)
CREATE INDEX d_col ON d USING sift_col (id, doc) WITH (paths = 'doc.m float8, doc.n int4, doc.s text, doc.b bool');
SELECT fmt('d_col');
SELECT * FROM all_types('d_col');
SELECT sift.col_verify('d_col', true);

-- minor 3 writes no tags: the stored values still count, an overflow value has no type
DROP INDEX d_col;
SET sift.col_format_minor = 3;
CREATE INDEX d_col ON d USING sift_col (id, doc) WITH (paths = 'doc.m float8, doc.n int4, doc.s text, doc.b bool');
SELECT fmt('d_col');
SELECT leaf, type, idx_rows, heap_rows, allknown FROM (VALUES ('m', 1), ('b', 4)) k(leaf, n),
	LATERAL types('d_col', k.n, k.leaf, CASE k.leaf WHEN 'm' THEN 'number' ELSE 'boolean' END) r
	WHERE type IN ('number', 'boolean') ORDER BY leaf, type;
SELECT sift.col_verify('d_col', true);

-- rows added under minor 4 sit in the write buffer with their types, are promoted to a minor 4 segment, and the merge with the minor 3 one
-- gives the old overflow values no type again
RESET sift.col_format_minor;
INSERT INTO d SELECT g, doc(g) FROM generate_series(30001, 40000) g;
SELECT * FROM unit_kinds('d_col');
SELECT sift.col_promote('d_col');
SELECT fmt('d_col');
SELECT * FROM unit_kinds('d_col');
SELECT sift.col_merge('d_col', true);
SELECT fmt('d_col');
SELECT * FROM unit_kinds('d_col');
SELECT sift.col_verify('d_col', true);

-- REINDEX writes the types again
REINDEX INDEX d_col;
SELECT fmt('d_col');
SELECT * FROM all_types('d_col') WHERE NOT same OR NOT allknown;
SELECT count(*) AS rows_checked FROM all_types('d_col');

-- verification: the type of a buffered overflow value is a tag, and it is the type of the heap value; the flag of a segment follows its minor
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	d text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
	RETURN regexp_replace(SQLSTATE || ': ' || SQLERRM || coalesce(' | ' || nullif(d, ''), ''), '[0-9]+', 'N', 'g');
END $$;
CREATE TABLE w (id int, doc jsonb);
CREATE INDEX w_idx ON w USING sift_col (doc) WITH (paths = 'doc.m float8');
INSERT INTO w VALUES (1, '{"m": "str"}');
CREATE TABLE pages AS SELECT b AS blk, pg.type, pg.lower FROM generate_series(0, pg_relation_size('w_idx') / 8192 - 1) b, LATERAL sift._test_page('w_idx', b) pg;
-- the first record of the buffer: its head follows the page head at 32, its first flag byte is at 44 (3: null and overflow, the type in the high nibble)
SELECT min(blk) AS rec_page FROM pages WHERE type = 7 AND lower > 32 \gset
SELECT sift._test_poke('w_idx', :rec_page, 44, 2 * 16 + 3, 1);
SELECT expect($$ SELECT sift.col_verify('w_idx', true) $$);
SELECT sift._test_poke('w_idx', :rec_page, 44, 7 * 16 + 3, 1);
SELECT expect($$ SELECT sift.col_verify('w_idx', true) $$);
SELECT sift._test_poke('w_idx', :rec_page, 44, 1 * 16 + 3, 1);
SELECT expect($$ SELECT sift.col_verify('w_idx', true) $$);
SELECT sift.col_promote('w_idx');
-- the directory entry of the segment says minor 3 or minor 4 (generation at 32, the minor 9 bytes after)
SELECT sift._test_poke('w_idx', 1, 41, 3, 1);
SELECT expect($$ SELECT sift.col_verify('w_idx', false) $$);
SELECT sift._test_poke('w_idx', 1, 41, 4, 1);
SELECT expect($$ SELECT sift.col_verify('w_idx', true) $$);
SET sift.col_format_minor = 3;
CREATE INDEX w3_idx ON w USING sift_col (doc) WITH (paths = 'doc.m float8');
RESET sift.col_format_minor;
SELECT sift._test_poke('w3_idx', 1, 41, 4, 1);
SELECT expect($$ SELECT sift.col_verify('w3_idx', false) $$);

DROP TABLE d, w, pages;
DROP FUNCTION expect(text);
DROP FUNCTION doc(int), fmt(regclass), types(regclass, int, text, text), all_types(regclass), unit_kinds(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
