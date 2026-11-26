-- Format major 2: the zones (SEC_ZONES) of a float column hold its least and greatest non-NaN value and say whether a NaN is there, and a zone says of a comparison that no row of a granule passes
-- it, that every row does, or that it cannot tell, in the order of PostgreSQL (-Infinity < every number < Infinity < NaN, a NaN equals a NaN). The zones of the integers do the same.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION zdump(index regclass, leaf text) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_zone_dump';
CREATE FUNCTION zcensus(index regclass, leaf text, op int, c float8[]) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_zone_census';
CREATE FUNCTION zv_f8(op int, c float8[], lo float8, hi float8, nnull int, nrows int, flags int) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_zone_verdict_f8';
CREATE FUNCTION zv_i8(op int, c int8[], lo int8, hi int8, nnull int, nrows int, flags int) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_zone_verdict_i8';

-- SiftColOp: EQ 1, NE 2, LT 3, LE 4, GT 5, GE 6, IN 7. Zone flags: NONE 1, FLOAT 32, HAS_NAN 64
CREATE FUNCTION sat(op int, v float8, c float8[]) RETURNS bool LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE op WHEN 1 THEN v = c[1] WHEN 2 THEN v <> c[1] WHEN 3 THEN v < c[1] WHEN 4 THEN v <= c[1] WHEN 5 THEN v > c[1] WHEN 6 THEN v >= c[1] ELSE v = ANY (c) END $$;

-- Zones of float columns and the values a granule of such a zone may hold: its two ends, or every value of a pool between them; a verdict must hold for all of them
CREATE TABLE pool AS SELECT v::float8 FROM unnest(ARRAY['-Infinity', '-5', '-0', '0', '5', '10', '15', '20', '25', 'Infinity']::float8[]) v;
CREATE TABLE zones (name text, lo float8, hi float8, nan bool, nnull int);
INSERT INTO zones VALUES ('finite', 10, 20, false, 0), ('finite with nulls', 10, 20, false, 5), ('finite and nan', 10, 20, true, 0), ('all nan', NULL, NULL, true, 0), ('one value', 15, 15, false, 0),
	('both infinities', '-Infinity', 'Infinity', false, 0), ('-inf', '-Infinity', '-Infinity', false, 0), ('+inf', 'Infinity', 'Infinity', false, 0), ('zero', 0, 0, false, 0),
	('negative to zero', -5, 0, false, 0), ('infinity and nan', 25, 'Infinity', true, 0), ('one value and nan', 15, 15, true, 0);
CREATE TABLE consts (c float8[]) ;
INSERT INTO consts SELECT ARRAY[v] FROM pool UNION ALL SELECT ARRAY['NaN'::float8] UNION ALL SELECT ARRAY[10, 20] UNION ALL SELECT ARRAY['NaN'::float8, 15] UNION ALL SELECT ARRAY[1, 2, 3]
	UNION ALL SELECT ARRAY['Infinity'::float8, 'NaN'::float8] UNION ALL SELECT ARRAY['-0'::float8];
CREATE TABLE realizations AS
	SELECT z.name, 'ends' AS how, ARRAY(SELECT x FROM unnest(ARRAY[z.lo, z.hi]) x WHERE x IS NOT NULL) || CASE WHEN z.nan THEN ARRAY['NaN'::float8] ELSE '{}' END AS vals FROM zones z
	UNION ALL SELECT z.name, 'pool', ARRAY(SELECT v FROM pool WHERE v BETWEEN z.lo AND z.hi) || ARRAY(SELECT z.lo WHERE z.lo IS NOT NULL) || ARRAY(SELECT z.hi WHERE z.hi IS NOT NULL)
		|| CASE WHEN z.nan THEN ARRAY['NaN'::float8] ELSE '{}' END FROM zones z;
CREATE VIEW verdicts AS
	SELECT z.name, o.op, k.c, zv_f8(o.op, k.c, coalesce(z.lo, 0), coalesce(z.hi, 0), z.nnull, 8192, 32 | CASE WHEN z.nan THEN 64 ELSE 0 END | CASE WHEN z.lo IS NULL THEN 1 ELSE 0 END) AS v, z.nnull
	FROM zones z, (SELECT generate_series(1, 7) op) o, consts k WHERE o.op = 7 OR cardinality(k.c) = 1;

-- No zone rules out a granule that has a row passing, or passes one that has a row failing or a NULL
SELECT count(*) AS cases, count(*) FILTER (WHERE v = 'none') AS none, count(*) FILTER (WHERE v = 'all') AS "all", count(*) FILTER (WHERE v = 'maybe') AS maybe FROM verdicts;
SELECT v.name, v.op, v.c, v.v, r.how, r.vals FROM verdicts v JOIN realizations r USING (name)
WHERE (v.v = 'none' AND EXISTS (SELECT 1 FROM unnest(r.vals) x WHERE sat(v.op, x, v.c)))
	OR (v.v = 'all' AND (v.nnull > 0 OR EXISTS (SELECT 1 FROM unnest(r.vals) x WHERE NOT sat(v.op, x, v.c))))
ORDER BY 1, 2, 3, 5;
-- The verdicts the order of PostgreSQL gives, one by one: (zone, op, constants) -> verdict
SELECT z.name, o.op, k.c, zv_f8(o.op, k.c, coalesce(z.lo, 0), coalesce(z.hi, 0), z.nnull, 8192, 32 | CASE WHEN z.nan THEN 64 ELSE 0 END | CASE WHEN z.lo IS NULL THEN 1 ELSE 0 END) AS verdict
FROM (VALUES ('finite', 10::float8, 20::float8, false, 0), ('finite and nan', 10, 20, true, 0), ('all nan', NULL, NULL, true, 0), ('finite with nulls', 10, 20, false, 5)) z(name, lo, hi, nan, nnull),
	(VALUES (1), (2), (3), (4), (5), (6)) o(op),
	(VALUES (ARRAY['NaN'::float8]), (ARRAY['Infinity'::float8]), (ARRAY['-Infinity'::float8]), (ARRAY[15::float8]), (ARRAY[5::float8]), (ARRAY[25::float8])) k(c)
WHERE z.name <> 'finite with nulls' OR k.c = ARRAY[25::float8]
ORDER BY 1, 2, 3;
SELECT zv_f8(7, ARRAY[10, 20], 10, 20, 0, 8192, 32) AS in_inside, zv_f8(7, ARRAY[1, 2], 10, 20, 0, 8192, 32) AS in_outside, zv_f8(7, ARRAY['NaN'::float8], 10, 20, 0, 8192, 32) AS in_nan_without,
	zv_f8(7, ARRAY['NaN'::float8], 10, 20, 0, 8192, 96) AS in_nan_with, zv_f8(7, ARRAY['NaN'::float8], 0, 0, 0, 8192, 97) AS in_nan_all_nan, zv_f8(7, ARRAY[15, 'NaN'::float8], 15, 15, 0, 8192, 96) AS in_both,
	zv_f8(7, ARRAY[15, 16], 15, 15, 0, 8192, 32) AS in_one_value;
-- A zone of a part written before floats had zones (no SIFT_ZONE_FLOAT) says nothing
SELECT zv_f8(5, ARRAY[1000], 0, 0, 0, 8192, 1) AS no_float_flag, zv_f8(5, ARRAY[1000], 10, 20, 0, 8192, 0) AS no_flags;

-- The zones of the integers: every row of a granule that has no NULL passes a comparison its ends decide
SELECT o.name, c.c, zv_i8(o.op, c.c, 10, 20, 0, 8192, 0) AS verdict FROM (VALUES ('EQ', 1), ('NE', 2), ('LT', 3), ('LE', 4), ('GT', 5), ('GE', 6)) o(name, op),
	(VALUES (ARRAY[5]), (ARRAY[9]), (ARRAY[10]), (ARRAY[15]), (ARRAY[20]), (ARRAY[21])) c(c) ORDER BY o.op, c.c;
SELECT zv_i8(1, ARRAY[7], 7, 7, 0, 8192, 0) AS eq_one_value, zv_i8(2, ARRAY[7], 7, 7, 0, 8192, 0) AS ne_one_value, zv_i8(7, ARRAY[7, 9], 7, 7, 0, 8192, 0) AS in_one_value,
	zv_i8(7, ARRAY[5, 9], 7, 7, 0, 8192, 0) AS in_other, zv_i8(7, ARRAY[7, 9], 7, 8, 0, 8192, 0) AS in_two_values, zv_i8(1, ARRAY[7], 7, 7, 3, 8192, 0) AS eq_one_value_with_nulls,
	zv_i8(3, ARRAY[21], 10, 20, 1, 8192, 0) AS lt_with_nulls, zv_i8(5, ARRAY[9], 10, 20, 8192, 8192, 1) AS all_null;
CREATE TABLE ipool AS SELECT generate_series(5, 25) v;
SELECT count(*) AS int_violations FROM (SELECT lo, hi, o.op, c.c FROM generate_series(8, 22, 3) lo, generate_series(8, 22, 2) hi, generate_series(1, 6) o(op), generate_series(5, 25) c(c) WHERE lo <= hi) q
WHERE (zv_i8(op, ARRAY[c], lo, hi, 0, 8192, 0) = 'none' AND EXISTS (SELECT 1 FROM ipool WHERE v IN (lo, hi) AND CASE op WHEN 1 THEN v = c WHEN 2 THEN v <> c WHEN 3 THEN v < c WHEN 4 THEN v <= c WHEN 5 THEN v > c ELSE v >= c END))
	OR (zv_i8(op, ARRAY[c], lo, hi, 0, 8192, 0) = 'all' AND EXISTS (SELECT 1 FROM ipool WHERE v BETWEEN lo AND hi AND NOT CASE op WHEN 1 THEN v = c WHEN 2 THEN v <> c WHEN 3 THEN v < c WHEN 4 THEN v <= c WHEN 5 THEN v > c ELSE v >= c END));

-- What the writer keeps: eight granules of 8192 rows, each of a shape (increasing, high, finite and NaN, all NaN, both infinities and zeros of both signs, all NULL, half NULL, one value)
CREATE TABLE fz (id int, price float8, p4 float4);
INSERT INTO fz SELECT i, CASE (i - 1) / 8192
		WHEN 0 THEN i::float8
		WHEN 1 THEN 10000 + i % 1000
		WHEN 2 THEN CASE WHEN i % 7 = 0 THEN 'NaN'::float8 ELSE 50 END
		WHEN 3 THEN 'NaN'::float8
		WHEN 4 THEN CASE i % 4 WHEN 0 THEN '-0'::float8 WHEN 1 THEN 0 WHEN 2 THEN 'Infinity' ELSE '-Infinity' END
		WHEN 5 THEN NULL
		WHEN 6 THEN CASE WHEN i % 2 = 0 THEN NULL ELSE 3.5 END
		ELSE 7.25 END,
	CASE (i - 1) / 8192 WHEN 0 THEN i::float4 WHEN 3 THEN 'NaN'::float4 WHEN 5 THEN NULL WHEN 4 THEN CASE i % 2 WHEN 0 THEN '-0'::float4 ELSE 0 END ELSE 7.25 END
FROM generate_series(1, 8192 * 8) i;
CREATE INDEX fz_i ON fz USING sift_col (id, price, p4) WITH (key_order = 'id');
SELECT sift._test_f2_check('fz_i') ~ '^parts=[0-9]+ ' AS consistent;
-- granule:flags:min:max:nulls, the flags 20 = float, 60 = float and NaN, 61 = nothing but NaN, 1 = no value
SELECT zdump('fz_i', 'price') AS price_zones;
SELECT zdump('fz_i', 'p4') AS p4_zones;

-- How many of the eight granules the zones rule out, cannot decide and pass whole (none/maybe/all)
SELECT n, zcensus('fz_i', 'price', op, c) AS price, zcensus('fz_i', 'p4', op, c) AS p4 FROM (VALUES
	(1, 3, ARRAY[100.0]), (2, 5, ARRAY[100.0]), (3, 5, ARRAY[20000.0]), (4, 3, ARRAY[0.0]), (5, 6, ARRAY[7.25]), (6, 6, ARRAY[10000.0]), (7, 1, ARRAY[7.25]),
	(8, 2, ARRAY[7.25]), (9, 1, ARRAY['NaN'::float8]), (10, 5, ARRAY['NaN'::float8]), (11, 6, ARRAY['NaN'::float8]), (12, 3, ARRAY['NaN'::float8]), (13, 4, ARRAY['NaN'::float8]),
	(14, 5, ARRAY['Infinity'::float8]), (15, 6, ARRAY['-Infinity'::float8]), (16, 1, ARRAY['-0'::float8]), (17, 7, ARRAY[1, 7.25, 9]), (18, 7, ARRAY['NaN'::float8, 50])) v(n, op, c) ORDER BY n;

-- Every answer is the heap's, through zones that rule granules out and pass others whole
CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT' END;
END $$;
CREATE FUNCTION zone_skipped(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM sift._test_f2_scan_counters(true);
	PERFORM f2_same(q);
	SELECT c[3] INTO n FROM (SELECT sift._test_f2_scan_counters(true) AS c) s;
	RETURN n;
END $$;
SELECT count(*) AS statements, count(*) FILTER (WHERE f2_same(q) <> 'same') AS different FROM (
	SELECT format('SELECT id FROM fz WHERE %s %s %s', col, op, k) AS q
	FROM (VALUES ('price'), ('p4')) c(col), (VALUES ('='), ('<>'), ('<'), ('<='), ('>'), ('>=')) o(op),
		(VALUES ('100'), ('7.25'), ('0'), ('-0.0'), ('10000'), ('''NaN'''), ('''Infinity'''), ('''-Infinity'''), ('50'), ('3.5')) k(k)
	UNION ALL SELECT format('SELECT id FROM fz WHERE %s IN (%s)', col, l) FROM (VALUES ('price'), ('p4')) c(col),
		(VALUES ('1, 7.25, 9'), ('''NaN'', 50'), ('''Infinity'', ''-Infinity'''), ('0'), ('-0.0, 3.5')) l(l)
	UNION ALL SELECT format('SELECT id FROM fz WHERE %s %s', col, o) FROM (VALUES ('price'), ('p4')) c(col), (VALUES ('IS NULL'), ('IS NOT NULL')) o(o)
	UNION ALL SELECT format('SELECT id FROM fz WHERE price > %s AND id %s', k, i) FROM (VALUES ('5'), ('7000'), ('''NaN''')) k(k), (VALUES ('< 100'), ('> 20000'), ('BETWEEN 8000 AND 40000')) i(i)
	) x;
-- a scan skips the granules the zones rule out
SELECT zone_skipped('SELECT id FROM fz WHERE price > 20000') AS skipped_all_but_inf_and_nan, zone_skipped('SELECT id FROM fz WHERE price = 7.25') > 0 AS skipped_eq,
	zone_skipped('SELECT id FROM fz WHERE p4 > 100') > 0 AS skipped_p4;

-- Zero of both signs is one value, and a granule of nothing but them is an exact point
CREATE TABLE zeros (id int, a float8, b float4);
INSERT INTO zeros SELECT i, CASE i % 2 WHEN 0 THEN '-0'::float8 ELSE 0 END, CASE i % 2 WHEN 0 THEN '-0'::float4 ELSE 0 END FROM generate_series(1, 4000) i;
CREATE INDEX zeros_i ON zeros USING sift_col (id, a, b) WITH (key_order = 'id');
SELECT zdump('zeros_i', 'a') AS a_zones, zdump('zeros_i', 'b') AS b_zones;
SELECT n, zcensus('zeros_i', 'a', op, c) AS a, f2_same(format('SELECT id FROM zeros WHERE a %s %s', (ARRAY['=', '<>', '<', '<=', '>', '>='])[op], c[1])) AS parity
FROM (VALUES (1, 1, ARRAY[0.0]), (2, 1, ARRAY['-0'::float8]), (3, 2, ARRAY[0.0]), (4, 3, ARRAY[0.0]), (5, 4, ARRAY['-0'::float8]), (6, 5, ARRAY[0.0]), (7, 6, ARRAY[0.0])) v(n, op, c) ORDER BY n;

-- Floats of the elements of the arrays of a jsonb document have zones of their own, by granule (of 4096 documents here): all small, all high, all small, high; a stray high value in the fifth; two elements a document, so a granule of 8192 elements is 4096 documents
CREATE TABLE docs (id int, doc jsonb);
INSERT INTO docs SELECT i, jsonb_build_object('items',
	(SELECT jsonb_agg(jsonb_build_object('sku', 's' || j, 'price', CASE (i - 1) / 4096 % 4 WHEN 0 THEN j * 1.5 WHEN 1 THEN 500 + j WHEN 2 THEN j * 2.5 ELSE 2000 + j END + CASE WHEN (i - 1) / 4096 = 4 AND i % 4096 = 7 AND j = 1 THEN 5000 ELSE 0 END, 'qty', j))
	 FROM generate_series(1, 2) j)) FROM generate_series(1, 32768) i;
CREATE INDEX docs_i ON docs USING sift_col (id, doc) WITH (key_order = 'id', paths = 'doc.items[]{sku text, price float8, qty int}');
SELECT sift._test_f2_check('docs_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT zdump('docs_i', 'doc.items[].price') AS price_zones, zdump('docs_i', 'doc.items[].qty') AS qty_zones;
SELECT n, zcensus('docs_i', 'doc.items[].price', op, c) AS price FROM (VALUES (1, 5, ARRAY[100.0]), (2, 6, ARRAY[100.0]), (3, 3, ARRAY[100.0]), (4, 1, ARRAY[501.0]), (5, 5, ARRAY[2000.0])) v(n, op, c) ORDER BY n;
-- and they are the least and greatest price of the elements of the documents of the granule, as the heap has them
SELECT string_agg(g || ':' || mn || ':' || mx, ' ' ORDER BY g) = (SELECT string_agg(split_part(z, ':', 1) || ':' || split_part(z, ':', 3) || ':' || split_part(z, ':', 4), ' ' ORDER BY ord)
	FROM unnest(string_to_array(zdump('docs_i', 'doc.items[].price'), ' ')) WITH ORDINALITY u(z, ord)) AS zones_are_the_heap_ranges
FROM (SELECT (d.id - 1) / 4096 AS g, min(i.price) AS mn, max(i.price) AS mx FROM docs d, jsonb_to_recordset(d.doc -> 'items') AS i(price float8) GROUP BY 1) x;

DROP VIEW verdicts;
DROP TABLE pool, zones, consts, realizations, ipool, fz, zeros, docs;
DROP FUNCTION zdump(regclass, text), zcensus(regclass, text, int, float8[]), zv_f8(int, float8[], float8, float8, int, int, int), zv_i8(int, int8[], int8, int8, int, int, int),
	sat(int, float8, float8[]), f2_same(text), zone_skipped(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
