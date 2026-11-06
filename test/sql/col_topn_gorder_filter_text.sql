-- A condition on the text column of another column (a source, a code) next to the pin of the group (WHERE g = x AND src = 'web' ORDER BY k DESC LIMIT n), also as a LATERAL probe or a
-- correlated sub-select of an outer row: the global order of k grouped by g is walked and the heap row of each entry tests the string, as it tests a flag or a status, instead of the
-- units being read for each probe. The answers are those of a copy of the table without an index (the oracle) over equalities, ranges and absent values, NULLs in the column and the group,
-- both directions, OFFSET, and 150 random statements (fixed seed).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE tt AS SELECT i AS id, CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 7) % 40 END AS g, ((i::bigint * 7919) % 60001)::int AS k,
	CASE WHEN i % 29 = 0 THEN NULL ELSE (ARRAY['web', 'app', 'phone', 'mail', 'partner', 'kiosk', 'chat', 'other'])[1 + (i * 13) % 8] END AS src,
	CASE WHEN i % 31 = 0 THEN NULL ELSE 'v' || lpad(((i * 17) % 500)::text, 3, '0') END AS code
FROM generate_series(1, 40000) i;
CREATE INDEX tt_i ON tt USING sift_col (id, g, k, src, code) WITH (order_columns = 'k', order_array_columns = 'k', order_array_group = 'g');
VACUUM ANALYZE tt;
CREATE TABLE tp AS SELECT * FROM tt;
ANALYZE tp;

CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_reads_global_order(q) $$;
-- the rows, in order, as the oracle has them
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT md5(coalesce(string_agg(x::text, '','' ORDER BY ord), '''')) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', replace(q, '@', 'tt')) INTO a;
	EXECUTE format('SELECT md5(coalesce(string_agg(x::text, '','' ORDER BY ord), '''')) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', replace(q, '@', 'tp')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs', q; END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE TABLE tq (n int, q text);
INSERT INTO tq SELECT row_number() OVER (), q FROM unnest(ARRAY[
 'SELECT id FROM @ WHERE g = 7 AND src = ''web'' ORDER BY k DESC, id LIMIT 5',
 'SELECT id FROM @ WHERE g = 7 AND src = ''web'' ORDER BY k, id LIMIT 1',
 'SELECT id FROM @ WHERE g = 7 AND src = ''zzz'' ORDER BY k, id LIMIT 5',
 'SELECT id FROM @ WHERE g = 7 AND src >= ''mail'' ORDER BY k DESC, id LIMIT 5',
 'SELECT id FROM @ WHERE g = 7 AND src < ''chat'' ORDER BY k, id LIMIT 5 OFFSET 2',
 'SELECT id FROM @ WHERE g = 7 AND code = ''v123'' ORDER BY k, id LIMIT 3',
 'SELECT id FROM @ WHERE g = 7 AND code >= ''v400'' AND src = ''app'' ORDER BY k DESC, id LIMIT 5',
 'SELECT id FROM @ WHERE g = 7 AND src = ''web'' AND code IS NOT NULL ORDER BY k, id LIMIT 5',
 'SELECT id FROM @ WHERE g IS NULL AND src = ''web'' ORDER BY k, id LIMIT 5',
 'SELECT id FROM @ WHERE g = 7 AND src IS NULL ORDER BY k, id LIMIT 5',
 'SELECT id FROM @ WHERE g IN (3, 5, 7) AND src = ''web'' ORDER BY k DESC, id LIMIT 8',
 'SELECT id FROM @ WHERE g = 7 AND src = ''web'' OR src = ''app'' ORDER BY k, id LIMIT 5',
 'SELECT id FROM @ WHERE g = 7 AND src <> ''web'' ORDER BY k, id LIMIT 5',
 'SELECT id FROM @ WHERE g = 7 AND src LIKE ''ma%'' ORDER BY k, id LIMIT 5',
 'SELECT v.i, x.id FROM (VALUES (1, 3), (2, 7), (3, NULL), (4, 99), (5, 39)) v(i, g), LATERAL (SELECT id FROM @ WHERE g = v.g AND src = ''web'' ORDER BY k DESC, id LIMIT 1) x ORDER BY v.i',
 'SELECT v.i, x.id FROM (VALUES (1, 3), (2, 7), (3, NULL), (4, 99), (5, 39)) v(i, g) LEFT JOIN LATERAL (SELECT id FROM @ WHERE g = v.g AND code >= ''v250'' ORDER BY k, id LIMIT 3) x ON true ORDER BY v.i, x.id',
 'SELECT v.i, (SELECT id FROM @ WHERE g = v.g AND src = ''kiosk'' ORDER BY k DESC, id LIMIT 1) FROM (VALUES (1, 3), (2, 7), (3, NULL), (4, 99)) v(i, g) ORDER BY v.i',
 'SELECT d.g, x.id FROM (SELECT DISTINCT g FROM tp WHERE g < 12) d, LATERAL (SELECT id FROM @ WHERE g = d.g AND src = ''chat'' ORDER BY k DESC, id LIMIT 2) x ORDER BY d.g, x.id']) q;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;

-- the walk is forced (every fetch allowed), and cut short at once (the units answer after the first fetches): the same rows
SET sift.test_topn_gbudget = 1000000000;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;
SET sift.test_topn_gbudget = 1;
SELECT n, same(q) AS answers FROM tq ORDER BY n;
RESET sift.test_topn_gbudget;

-- writes: the write buffer, an update of the string, a delete, a rolled back insert; both tables get the same
CREATE FUNCTION twice(sql text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(sql, '@', 'tt');
	EXECUTE replace(sql, '@', 'tp');
END $$;
SELECT twice('INSERT INTO @ SELECT 40000 + i, (i * 3) % 40, (i * 977) % 60001, ''web'', ''v123'' FROM generate_series(1, 300) i');
SELECT twice('UPDATE @ SET src = ''web'' WHERE id % 211 = 0');
SELECT twice('UPDATE @ SET src = NULL WHERE id % 307 = 0');
SELECT twice('DELETE FROM @ WHERE id % 97 = 0');
BEGIN; DELETE FROM tt WHERE src = 'web'; ROLLBACK;
SELECT n, same(q) AS answers FROM tq ORDER BY n;
VACUUM tt, tp;
SELECT n, same(q) AS answers FROM tq ORDER BY n;

-- random statements: the shape, the group (a constant or an outer row), the string condition, the direction, the limit and the offset
CREATE FUNCTION fuzz_text(n int) RETURNS TABLE (statements int, differ int, used_global_order int) LANGUAGE plpgsql AS $$
DECLARE
	conds text[] := ARRAY['src = ''web''', 'src = ''partner''', 'src = ''zzz''', 'src >= ''mail''', 'src < ''chat''', 'src > ''kiosk'' AND src <= ''phone''', 'code = ''v123''', 'code >= ''v400''', 'code < ''v050''',
						  'src = ''web'' AND code < ''v250''', 'src = ''app'' AND code IS NOT NULL', 'src IS NULL', 'src = ''web'' AND k > 30000', 'src = ''other'' AND id % 3 = 0'];
	q text; shape int; c text; d text; nl text; lim int; off int; gv text;
BEGIN
	PERFORM setseed(0.37);
	statements := 0; differ := 0; used_global_order := 0;
	FOR i IN 1..n LOOP
		shape := floor(random() * 4)::int;
		c := conds[1 + floor(random() * 14)::int];
		d := (ARRAY['ASC', 'DESC'])[1 + floor(random() * 2)::int];
		nl := (ARRAY['NULLS FIRST', 'NULLS LAST'])[1 + floor(random() * 2)::int];
		lim := 1 + floor(random() * 6)::int;
		off := CASE WHEN random() < 0.3 THEN floor(random() * 4)::int ELSE 0 END;
		gv := (ARRAY['3', '7', '39', '0', '99', 'NULL', '12'])[1 + floor(random() * 7)::int];
		q := CASE shape
			WHEN 0 THEN format('SELECT id FROM @ WHERE g = %s AND %s ORDER BY k %s %s, id LIMIT %s OFFSET %s', gv, c, d, nl, lim, off)
			WHEN 1 THEN format('SELECT v.i, x.id FROM (VALUES (1, %s), (2, %s), (3, %s), (4, %s)) v(i, g), LATERAL (SELECT id FROM @ WHERE g = v.g AND %s ORDER BY k %s %s, id LIMIT %s) x ORDER BY v.i, x.id',
							   gv, (ARRAY['3', '7', '39', '0', '99', 'NULL', '12'])[1 + floor(random() * 7)::int], (ARRAY['3', '7', '39', '0'])[1 + floor(random() * 4)::int], (ARRAY['5', '8', '21'])[1 + floor(random() * 3)::int], c, d, nl, lim)
			WHEN 2 THEN format('SELECT v.i, (SELECT id FROM @ WHERE g = v.g AND %s ORDER BY k %s %s, id LIMIT 1) FROM (VALUES (1, %s), (2, %s), (3, %s)) v(i, g) ORDER BY v.i',
							   c, d, nl, gv, (ARRAY['3', '7', '39', '0', '99', 'NULL'])[1 + floor(random() * 6)::int], (ARRAY['5', '8', '21'])[1 + floor(random() * 3)::int])
			ELSE format('SELECT id FROM @ WHERE g IN (%s, %s) AND %s ORDER BY k %s %s, id LIMIT %s', gv, (ARRAY['3', '7', '39', '12'])[1 + floor(random() * 4)::int], c, d, nl, lim)
		END;
		statements := statements + 1;
		IF NOT same(q) THEN differ := differ + 1; END IF;
		IF global_order(replace(q, '@', 'tt')) THEN used_global_order := used_global_order + 1; END IF;
	END LOOP;
	RETURN NEXT;
END $$;
SELECT statements, differ, used_global_order > 0 AS used_global_order FROM fuzz_text(150);

DROP FUNCTION fuzz_text(int), twice(text), same(text), global_order(text);
DROP TABLE tq, tt, tp;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
