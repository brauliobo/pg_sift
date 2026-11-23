-- sift_col (format major 2) holds a column of any type: a type with no leaf of its own (point, tsvector, json, xml, the geometric types, oid, tid, a composite, a domain of one) is kept as the image of its datum, returned
-- by an index-only scan as it was written, and every qual on it is the heap's. The index never refuses a type at CREATE INDEX.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
-- the library of the testing build creates the indexes of format major 1 by default; these are of format major 2, which the release library creates
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

CREATE TYPE pair AS (a int, b text);
CREATE DOMAIN dpt AS point;
CREATE DOMAIN dpair AS pair;
CREATE TABLE o (id int, pt point, ln line, ls lseg, bx box, pa path, pg polygon, ci circle, tsv tsvector, tsq tsquery, j json, x xml, oi oid, rc regclass, xi xid, td tid, ch "char", lsn pg_lsn,
				v2 int2vector, cp pair, dp dpt, dc dpair, jp jsonpath, sn pg_snapshot, big json);
INSERT INTO o
SELECT g,
	CASE WHEN g % 13 = 0 THEN NULL ELSE point(g, g * 2) END,
	CASE WHEN g % 13 = 1 THEN NULL ELSE line(point(g, 0), point(g, 1)) END,
	CASE WHEN g % 13 = 2 THEN NULL ELSE lseg(point(g, 0), point(g, 5)) END,
	CASE WHEN g % 13 = 3 THEN NULL ELSE box(point(g, g), point(g + 1, g + 1)) END,
	CASE WHEN g % 13 = 4 THEN NULL ELSE path('((1,1),(' || g || ',2),(3,4))') END,
	CASE WHEN g % 13 = 5 THEN NULL ELSE polygon(box(point(g, g), point(g + 2, g + 2))) END,
	CASE WHEN g % 13 = 6 THEN NULL ELSE circle(point(g, g), 3) END,
	CASE WHEN g % 13 = 7 THEN NULL ELSE to_tsvector('simple', 'word' || g || ' common text') END,
	CASE WHEN g % 13 = 8 THEN NULL ELSE to_tsquery('simple', 'a' || g || ' & b') END,
	CASE WHEN g % 13 = 9 THEN NULL ELSE json_build_object('id', g, 'tags', json_build_array('a', 'b', g)) END,
	CASE WHEN g % 13 = 10 THEN NULL ELSE xmlparse(DOCUMENT '<r id="' || g || '"><c>x</c></r>') END,
	CASE WHEN g % 13 = 11 THEN NULL ELSE g::oid END,
	CASE WHEN g % 13 = 12 THEN NULL ELSE 'pg_class'::regclass END,
	CASE WHEN g % 7 = 0 THEN NULL ELSE g::text::xid END,
	CASE WHEN g % 7 = 1 THEN NULL ELSE ('(' || g || ',1)')::tid END,
	CASE WHEN g % 7 = 2 THEN NULL ELSE chr(65 + g % 26)::"char" END,
	CASE WHEN g % 7 = 3 THEN NULL ELSE ('0/' || to_hex(g))::pg_lsn END,
	CASE WHEN g % 7 = 4 THEN NULL ELSE ('1 ' || g)::int2vector END,
	CASE WHEN g % 7 = 5 THEN NULL ELSE ROW(g, 'row' || g)::pair END,
	CASE WHEN g % 7 = 6 THEN NULL ELSE point(-g, g)::dpt END,
	CASE WHEN g % 11 = 0 THEN NULL ELSE ROW(g, NULL)::dpair END,
	CASE WHEN g % 11 = 1 THEN NULL ELSE ('$.a[' || g || ']')::jsonpath END,
	CASE WHEN g % 11 = 2 THEN NULL ELSE pg_current_snapshot() END,
	CASE WHEN g % 50 = 0 THEN (SELECT json_build_object('k', string_agg(md5(g::text || i::text), '')) FROM generate_series(1, 1500) i) END
FROM generate_series(1, 3000) g;
CREATE INDEX o_i ON o USING sift_col (id, pt, ln, ls, bx, pa, pg, ci, tsv, tsq, j, x, oi, rc, xi, td, ch, lsn, v2, cp, dp, dc, jp, sn, big)
	WITH (return_columns = 'pt, ln, ls, bx, pa, pg, ci, tsv, tsq, j, x, oi, rc, xi, td, ch, lsn, cp, dp, dc, jp, sn, big');
VACUUM o;

-- one query for every plan: the rows of the table as text, in the order of id, under the plan each setting asks for
CREATE FUNCTION o_rows(plan text, cols text, lo int, hi int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('enable_seqscan', CASE WHEN plan = 'heap' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN plan = 'heap' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_indexscan', CASE WHEN plan = 'heap' THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('enable_indexonlyscan', CASE WHEN plan = 'index-only' THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT md5(string_agg(t::text, E''\n'' ORDER BY id)) FROM (SELECT id, %s FROM o WHERE id BETWEEN %s AND %s) t', cols, lo, hi) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION o_plan(cols text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT id, %s FROM o WHERE id BETWEEN 10 AND 20', cols) LOOP
		IF l ~ 'Scan' THEN p := p || regexp_replace(trim(l), ' using .*', ''); END IF;
	END LOOP;
	RETURN p;
END $$;

SELECT o_plan('pt, tsv, j, x');
SELECT o_plan('pt, tsv, j, x, big');

-- what an index-only scan returns of every type is what the heap holds (the md5 of the rows as text, per type and for all)
CREATE TABLE o_cols (c text);
INSERT INTO o_cols VALUES ('pt'), ('ln'), ('ls'), ('bx'), ('pa'), ('pg'), ('ci'), ('tsv'), ('tsq'), ('j'), ('x'), ('oi'), ('rc'), ('xi'), ('td'), ('ch'), ('lsn'), ('v2'), ('cp'), ('dp'), ('dc'), ('jp'), ('sn'), ('big');
SELECT c, o_rows('heap', c, 1, 3000) = o_rows('index-only', c, 1, 3000) AS same_rows, o_rows('heap', c, 1, 3000) = o_rows('index', c, 1, 3000) AS same_by_index_scan FROM o_cols ORDER BY c;
SELECT o_rows('heap', 'pt, ln, ls, bx, pa, pg, ci, tsv, tsq, j, x, oi, rc, xi, td, ch, lsn, v2, cp, dp, dc, jp, sn, big', 1, 3000)
     = o_rows('index-only', 'pt, ln, ls, bx, pa, pg, ci, tsv, tsq, j, x, oi, rc, xi, td, ch, lsn, v2, cp, dp, dc, jp, sn, big', 1, 3000) AS all_columns;

-- a value as it was written
SELECT id, pt, bx, tsv, j, x, oi, td, ch, lsn, cp, dp, dc FROM o WHERE id IN (5, 13) ORDER BY id;
SELECT id, left(big::text, 20) AS big_head, length(big::text) AS big_len FROM o WHERE id IN (50, 100) ORDER BY id;

-- NULL and not NULL, and COUNT(column), under every plan
CREATE FUNCTION o_counts(plan text) RETURNS TABLE (c text, nulls bigint, not_nulls bigint, counted bigint) LANGUAGE plpgsql AS $$
DECLARE col text;
BEGIN
	PERFORM set_config('enable_seqscan', CASE WHEN plan = 'heap' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN plan = 'heap' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('sift.enable_agg', CASE WHEN plan = 'heap' THEN 'off' ELSE 'on' END, true);
	FOR col IN SELECT o_cols.c FROM o_cols ORDER BY 1 LOOP
		c := col;
		EXECUTE format('SELECT count(*) FILTER (WHERE %1$I IS NULL), count(*) FILTER (WHERE %1$I IS NOT NULL), count(%1$I) FROM o', col) INTO nulls, not_nulls, counted;
		RETURN NEXT;
	END LOOP;
END $$;
SELECT (SELECT md5(string_agg(r::text, E'\n' ORDER BY r.c)) FROM o_counts('heap') r) = (SELECT md5(string_agg(r::text, E'\n' ORDER BY r.c)) FROM o_counts('index') r) AS same_counts;
SELECT c, nulls, not_nulls, counted FROM o_counts('index') WHERE c IN ('pt', 'big', 'oi', 'dc') ORDER BY c;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM o WHERE pt IS NULL AND ln IS NOT NULL AND tsv IS NOT NULL;
SELECT count(*) FROM o WHERE x IS NULL OR j IS NULL;
RESET enable_seqscan; RESET enable_bitmapscan;

-- the quals on the type's own operators are the heap's, whatever the plan
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT id FROM o WHERE pt ~= point(5, 10) AND id > 0;
SELECT id FROM o WHERE tsv @@ to_tsquery('simple', 'word77') AND id > 0;
SELECT count(*) FROM o WHERE bx && box(point(10, 10), point(20, 20)) AND id > 0;
SELECT id FROM o WHERE oi = 77::oid AND id > 0;
SELECT id FROM o WHERE cp = ROW(88, 'row88')::pair AND id > 0;
SELECT id FROM o WHERE xml_is_well_formed(x::text) AND id BETWEEN 1 AND 12 ORDER BY id;
RESET enable_seqscan; RESET enable_bitmapscan;

-- the writes: an insert, an update of the type, a delete, a vacuum and a maintenance step
INSERT INTO o (id, pt, tsv, j, x, oi, cp, dp) VALUES (4000, point(1, 1), to_tsvector('simple', 'new'), '{"new": true}', '<n/>', 4000, ROW(4000, 'new')::pair, point(4, 4)), (4001, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
UPDATE o SET pt = point(0, 0), tsv = to_tsvector('simple', 'changed'), cp = ROW(0, NULL)::pair, big = '{"k": 1}' WHERE id IN (5, 50);
UPDATE o SET pt = NULL, x = NULL WHERE id IN (6, 7);
DELETE FROM o WHERE id BETWEEN 100 AND 110;
SELECT c, o_rows('heap', c, 1, 4001) = o_rows('index', c, 1, 4001) AS same_rows FROM o_cols WHERE c IN ('pt', 'tsv', 'x', 'cp', 'j', 'oi', 'big') ORDER BY c;
VACUUM o;
SELECT c, o_rows('heap', c, 1, 4001) = o_rows('index-only', c, 1, 4001) AS same_rows FROM o_cols WHERE c IN ('pt', 'tsv', 'x', 'cp', 'j', 'oi', 'big') ORDER BY c;
SELECT sift.maintain('o_i') IS NOT NULL AS maintained;
SELECT c, o_rows('heap', c, 1, 4001) = o_rows('index-only', c, 1, 4001) AS same_rows FROM o_cols WHERE c IN ('pt', 'tsv', 'x', 'cp', 'j', 'oi', 'big') ORDER BY c;
SELECT count(*) AS problems FROM sift.check('o_i');

-- the same columns as INCLUDE columns of a key, the index-only scan reading them from there
CREATE INDEX o_inc ON o USING sift_col (id) INCLUDE (pt, tsv, j, x, oi, cp, dc) WITH (return_columns = 'pt, tsv, j, x, oi, cp, dc');
DROP INDEX o_i;
VACUUM o;
SELECT o_plan('pt, tsv, j, x, oi, cp, dc');
SELECT o_rows('heap', 'pt, tsv, j, x, oi, cp, dc', 1, 4001) = o_rows('index-only', 'pt, tsv, j, x, oi, cp, dc', 1, 4001) AS same_rows;

-- a column whose type is not named in return_columns is read from the heap
CREATE INDEX o_k ON o USING sift_col (id, pt, tsv);
DROP INDEX o_inc;
VACUUM o;
SELECT o_plan('pt, tsv');
SELECT o_rows('heap', 'pt, tsv', 1, 4001) = o_rows('index', 'pt, tsv', 1, 4001) AS same_rows;
DROP TABLE o, o_cols;
DROP FUNCTION o_rows(text, text, int, int), o_plan(text), o_counts(text);
DROP DOMAIN dpair;
DROP DOMAIN dpt;
DROP TYPE pair;
DROP EXTENSION pg_sift;
