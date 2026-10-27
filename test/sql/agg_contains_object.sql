-- doc @> '{...}' on a jsonb attribute whose paths are leaves of the index: the members of the object are tests of the leaves (a scalar equals the leaf, a nested object is the same
-- of its members, an array is contained by the array leaf), as the heap's containment has them. A clean table runs as the scan; a table with every wrong type, a root that is no
-- object, a JSON null and a scalar where a container is expected gives the rows of the normal plan whichever plan takes it.
CREATE EXTENSION pg_sift;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE co_clean (id int, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO co_clean
SELECT i, i % 5,
       CASE WHEN i % 17 = 0 THEN NULL
            WHEN i % 13 = 0 THEN '{}'
            ELSE jsonb_strip_nulls(jsonb_build_object('k', CASE WHEN i % 11 = 0 THEN NULL ELSE i % 7 END, 's', 'x' || i % 5, 'b', i % 2 = 0,
                       'o', jsonb_build_object('a', i % 3, 't', 'u' || i % 2), 'tags', jsonb_build_array('t' || i % 4, 't' || i % 6))) END
FROM generate_series(1, 3000) i;
CREATE INDEX co_clean_col ON co_clean USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.s text, doc.b bool, doc.o.a int4, doc.o.t text, doc.tags[] text');

CREATE TABLE co_dirty (id int, g int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO co_dirty
SELECT i, i % 5,
       CASE i % 14
            WHEN 0 THEN jsonb_build_object('k', i % 7, 's', 'x' || i % 5, 'b', i % 2 = 0, 'o', jsonb_build_object('a', i % 3, 't', 'u' || i % 2), 'tags', jsonb_build_array('t' || i % 4, 't' || i % 6))
            WHEN 1 THEN jsonb_build_object('k', (i % 7)::text, 's', 'x1')
            WHEN 2 THEN jsonb_build_object('k', i % 7 + 0.5, 's', 5)
            WHEN 3 THEN jsonb_build_object('k', NULL, 's', NULL, 'o', NULL)
            WHEN 4 THEN jsonb_build_array(1, 2, i % 7)
            WHEN 5 THEN to_jsonb('x' || i % 5)
            WHEN 6 THEN jsonb_build_object('o', jsonb_build_array(jsonb_build_object('a', i % 3)), 'tags', 't1')
            WHEN 7 THEN jsonb_build_object('o', jsonb_build_object('a', i % 3, 't', NULL), 'tags', jsonb_build_array(jsonb_build_object('x', 1)))
            WHEN 8 THEN NULL
            WHEN 9 THEN '{}'::jsonb
            WHEN 10 THEN jsonb_build_object('k', i % 7, 'b', 'true', 's', 'true')
            ELSE jsonb_build_object('k', i % 7, 's', 'x' || i % 5, 'b', i % 3 = 0, 'tags', jsonb_build_array('t' || i % 4))
       END
FROM generate_series(1, 3000) i;
CREATE INDEX co_dirty_col ON co_dirty USING sift_col (id, g, doc) WITH (paths = 'doc.k int4, doc.s text, doc.b bool, doc.o.a int4, doc.o.t text, doc.tags[] text');
VACUUM (FREEZE, ANALYZE) co_clean, co_dirty;

CREATE FUNCTION scan_same(q text) RETURNS TABLE (scan bool, same bool) LANGUAGE plpgsql AS $$
DECLARE l text; got bigint; want bigint;
BEGIN
	scan := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scan := scan OR l LIKE '%SiftAgg%';
	END LOOP;
	EXECUTE q INTO got;
	SET LOCAL sift.enable_agg = off;
	EXECUTE q INTO want;
	RESET sift.enable_agg;
	same := got = want;
	RETURN NEXT;
END $$;

CREATE TABLE patterns (name text, pat text);
INSERT INTO patterns VALUES
	('int', '{"k": 3}'),
	('int absent', '{"k": 99}'),
	('string', '{"s": "x2"}'),
	('string like a number', '{"s": "5"}'),
	('bool', '{"b": true}'),
	('bool false', '{"b": false}'),
	('two dim_b', '{"k": 3, "s": "x3"}'),
	('three dim_b', '{"k": 3, "s": "x3", "b": false}'),
	('nested', '{"o": {"a": 1}}'),
	('nested two', '{"o": {"a": 1, "t": "u1"}}'),
	('nested path of the attribute', '{"o": {"a": 1}, "k": 3}'),
	('array', '{"tags": ["t1"]}'),
	('array two', '{"tags": ["t1", "t2"]}'),
	('array and member', '{"tags": ["t1"], "k": 3}'),
	('array of objects', '{"tags": [{"x": 1}]}'),
	('float', '{"k": 3.5}'),
	('integral float', '{"k": 3.0}'),
	('JSON null', '{"k": null}'),
	('empty object', '{"o": {}}'),
	('empty array', '{"tags": []}'),
	('empty', '{}'),
	('unknown key', '{"zz": 1}'),
	('key of another type', '{"o": [{"a": 1}]}'),
	('root array', '[3]'),
	('root scalar', '3');

SELECT t.name AS tbl, p.name, (scan_same(format('SELECT count(*) FROM %s WHERE doc @> %L', t.name, p.pat))).*
FROM (VALUES ('co_clean'), ('co_dirty')) t(name), patterns p ORDER BY 1, p.name;

-- in a boolean tree, under a NOT, with a filter, over the path of a member, as a group filter
SELECT q, (scan_same(q)).* FROM (VALUES
	('SELECT count(*) FROM co_clean WHERE doc @> ''{"k": 3}'' OR doc @> ''{"k": 4}'''),
	('SELECT count(*) FROM co_clean WHERE doc @> ''{"k": 3}'' AND g = 2'),
	('SELECT count(*) FROM co_clean WHERE NOT doc @> ''{"k": 3}'''),
	('SELECT count(*) FROM co_clean WHERE doc @> ''{"k": 3}'' IS NOT TRUE'),
	('SELECT count(*) FROM co_clean WHERE doc -> ''o'' @> ''{"a": 1}'''),
	('SELECT count(*) FROM co_dirty WHERE doc @> ''{"k": 3}'' OR doc @> ''{"s": "x1"}'''),
	('SELECT count(*) FROM co_clean WHERE ''{"k": 3}'' <@ doc')
) v(q);

SELECT g, count(*), sum(id) FROM co_clean WHERE doc @> '{"o": {"a": 1}, "tags": ["t1"]}' GROUP BY g ORDER BY g;
SET sift.enable_agg = off;
SELECT g, count(*), sum(id) FROM co_clean WHERE doc @> '{"o": {"a": 1}, "tags": ["t1"]}' GROUP BY g ORDER BY g;
RESET sift.enable_agg;

DROP FUNCTION scan_same(text);
DROP TABLE patterns, co_clean, co_dirty;
DROP EXTENSION pg_sift;
