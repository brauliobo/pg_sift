-- tests of the elements of an array column in SiftAggV6 (src/agg/v6/v6_unnest_exists.c, v6_unnest_path.c, src/agg/v6/unnest_test.c): jsonb_path_exists, @? and @> over the objects of the array, and EXISTS
-- and NOT EXISTS over jsonb_to_recordset that compare an element with a column of its row, in a WHERE and in an aggregate's FILTER. The data holds what the leaves tell apart: a member that is a JSON null, a missing
-- one, one of another JSON type than the leaf's, an element that is no object, a document that is no array, a NULL one, and rows still in the slab of the index; the columns of the row have NULLs, a NaN and
-- values that need a wider integer than the member's. Every statement runs over the index and over a heap twin (the truth): the first word says they agree, the second whether the plan is the node's.
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

CREATE TABLE sh (id int PRIMARY KEY, grp int, r4 int, r8 bigint, rf float8, items jsonb, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO sh
SELECT i, i % 7, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 6 END, CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN 3000000000 ELSE i % 6 END,
	CASE WHEN i % 13 = 0 THEN NULL WHEN i % 17 = 0 THEN 'NaN'::float8 ELSE (i % 6) * 0.5 END,
	CASE i % 23
		WHEN 0 THEN NULL
		WHEN 1 THEN '[]'
		WHEN 2 THEN '{"kind": "gift", "qty": 3}'
		WHEN 3 THEN '"gift"'
		WHEN 4 THEN '3'
		WHEN 5 THEN 'null'
		WHEN 6 THEN '[1, {"kind": "gift", "qty": 3}, null, "x"]'
		WHEN 7 THEN '[[{"kind": "gift"}], {"kind": "std", "qty": 1}]'
		WHEN 8 THEN '[{"kind": 5, "qty": 2}, {"kind": "std", "qty": 4}]'
		WHEN 9 THEN '[{"kind": ["gift"], "qty": 2}]'
		WHEN 10 THEN '[{"kind": {"a": 1}, "qty": 2}]'
		WHEN 11 THEN '[{"kind": "std", "qty": "3"}]'
		WHEN 12 THEN '[{"kind": "std", "qty": 3.5}]'
		WHEN 13 THEN '[{"kind": "std", "qty": 5000000000}]'
		WHEN 14 THEN '[{"kind": null, "qty": 1, "flag": null}]'
		WHEN 15 THEN '[{"qty": 2, "flag": true}, {"kind": "std"}]'
		WHEN 16 THEN '[{"kind": "gift", "qty": null, "flag": false}]'
		ELSE jsonb_build_array(
			jsonb_build_object('kind', (ARRAY['std', 'gift', 'express'])[i % 3 + 1], 'qty', i % 5, 'flag', i % 2 = 0),
			jsonb_build_object('kind', (ARRAY['express', 'std', 'gift'])[i % 3 + 1], 'qty', i % 4))
	END,
	CASE WHEN i % 10 = 0 THEN NULL ELSE coalesce((SELECT jsonb_agg(jsonb_build_object('q', CASE WHEN (i * 3 + j * 5) % 8 = 0 THEN NULL ELSE (i + j - 1) % 6 END,
		'q8', (i + j - 1) % 6 + CASE WHEN (i * 2 + j) % 13 = 0 THEN 3000000000 ELSE 0 END, 'f', ((i + j - 1) % 6) * 0.5) - CASE WHEN (i * 5 + j * 3) % 11 = 0 THEN 'q' ELSE '' END ORDER BY j)
		FROM generate_series(1, i % 5) j), '[]') END
FROM generate_series(1, 2400) i;
ALTER TABLE sh ADD arr jsonb;
UPDATE sh SET arr = CASE WHEN items IS NULL THEN NULL WHEN jsonb_typeof(items) = 'array' THEN items ELSE '[]' END;
CREATE TABLE sc (LIKE sh INCLUDING INDEXES);
INSERT INTO sc SELECT * FROM sh WHERE id <= 2000;
CREATE INDEX sc_col ON sc USING sift_col (id, grp, r4, r8, rf, items, doc, arr)
	WITH (paths = 'items[]{kind text, qty int4, flag bool}, doc[]{q int4, q8 int8, f float8}, arr[]{kind text, qty int4}');
INSERT INTO sc SELECT * FROM sh WHERE id > 2000;
ANALYZE sh;
ANALYZE sc;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; line text; node boolean := false;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'sc') LOOP node := node OR line LIKE '%SiftAggV6%'; END LOOP;
	EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', 'sc')) INTO a;
	EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', 'sh')) INTO b;
	RETURN (CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END) || ' ' || (CASE WHEN node THEN 'node' ELSE 'normal' END) || ' ' || q;
END $$;

-- jsonpath in a WHERE: != and == on a text member (a JSON null differs from a literal, a missing key from none), ! of them, a null literal, variables, strict mode without !
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != "gift")'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind == "gift")'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (!(@.kind == "gift"))'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (!(@.kind != "gift"))'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind == null)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != null)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (!(@.kind == null))'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != $v)'', ''{"v": "gift"}'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.qty == $q)'', ''{"q": 3}'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind == $z)'', ''{"z": null}'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.arr, ''strict $[*] ? (@.qty == 3)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.arr, ''strict $[*] ? (@.kind != "gift" && @.qty > 1)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE o.items @? ''$[*] ? (@.qty < 2 || @.kind == "gift")'' GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.flag == true)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.flag != true)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (3 != @.qty)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.qty >= 2 && !(@.kind == "gift" || @.qty > 3))'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != "gift")'', ''{}'', true) AND o.grp < 5 GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE NOT jsonb_path_exists(o.items, ''$[*] ? (@.kind != "gift")'') GROUP BY 1',
	'SELECT o.grp, sum(o.r4), count(DISTINCT o.id) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != "gift")'') GROUP BY 1'
]) q;

-- containment of a JSON null member and of values
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.grp, count(*) FROM @T o WHERE o.items @> ''[{"kind": null}]'' GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE o.items @> ''[{"kind": "gift", "qty": 3}]'' GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE o.items @> ''[{"flag": true}]'' GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE ''[{"qty": null}]'' <@ o.items GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE NOT o.items @> ''[{"kind": null}]'' GROUP BY 1'
]) q;

-- the same as the flags of an aggregate's FILTER
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.grp, count(*), count(*) FILTER (WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != "gift")'')) AS ne, count(*) FILTER (WHERE jsonb_path_exists(o.items, ''$[*] ? (!(@.kind == "gift"))'')) AS not_eq,
		count(*) FILTER (WHERE NOT jsonb_path_exists(o.items, ''$[*] ? (@.kind != "gift")'')) AS none_ne, count(*) FILTER (WHERE o.items @> ''[{"kind": null}]'') AS jnull,
		count(*) FILTER (WHERE o.items @? ''$[*] ? (@.qty == 0)'' AND o.r4 > 2) AS zero, sum(o.id) FILTER (WHERE jsonb_path_exists(o.items, ''$[*] ? (@.flag == true)'')) AS flags FROM @T o GROUP BY 1',
	'SELECT count(*), count(*) FILTER (WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind == null)'')) AS eq_null, count(*) FILTER (WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != null)'')) AS ne_null FROM @T o'
]) q;

-- a literal of another JSON type than the member's, a number the leaf does not hold, !() in strict mode, a second array and a second element of the pattern keep the normal plan
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.qty == "3")'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != 3)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.qty > 1.5)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.arr, ''strict $[*] ? (!(@.kind == "gift"))'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE jsonb_path_exists(o.items, ''$[*] ? (@.kind != "gift")'') AND jsonb_path_exists(o.doc, ''$[*] ? (@.q == 1)'') GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE o.items @> ''[{"kind": "gift"}, {"qty": null}]'' GROUP BY 1'
]) q;

-- EXISTS and NOT EXISTS over the elements against a column of their row: = <> < <= > >= of int4 members against int4 and bigint columns, float8 against float8, NULLs and a NaN
SELECT same(format('SELECT o.grp, count(*) FROM @T o WHERE %s EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int, q8 bigint, f float8) WHERE %s) GROUP BY 1', n, c))
FROM unnest(ARRAY['', 'NOT']) n, unnest(ARRAY['i.q = o.r4', 'i.q <> o.r4', 'i.q < o.r4', 'i.q <= o.r4', 'i.q > o.r4', 'i.q >= o.r4', 'i.q = o.r8', 'i.q < o.r8', 'i.q8 >= o.r8', 'i.q8 <> o.r4',
	'i.f = o.rf', 'i.f < o.rf', 'i.f >= o.rf', 'i.f <> o.rf', 'o.r4 > i.q', 'o.r4 <= i.q', 'i.q = o.r4 AND i.f >= 1.5', 'i.q > o.r4 AND i.q < 5 AND i.f <> 2', 'i.q >= 2']) c;

-- the same as the flags of an aggregate's FILTER, six of them and a NOT EXISTS beside it
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.grp, count(*) AS n,
		count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q = o.r4)) AS eq, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q <> o.r4)) AS ne,
		count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q < o.r4)) AS lt, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q <= o.r4)) AS le,
		count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q > o.r4)) AS gt, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q >= o.r4)) AS ge,
		count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q = o.r4)) AS none_eq FROM @T o GROUP BY 1',
	'SELECT o.grp, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q8 bigint) WHERE i.q8 >= o.r8) AND o.r4 > 2) AS wide, sum(o.id) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(f float8) WHERE i.f = o.rf)) AS floats FROM @T o GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE o.id > 100 AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q >= o.r4) AND NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q > 4) GROUP BY 1'
]) q;

-- a comparison inside an OR, with an expression, past an OFFSET, or against a column of another type class keeps the normal plan
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.grp, count(*) FROM @T o WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q = o.r4 OR i.q IS NULL) GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q = o.r4 + 1) GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q >= o.r4 OFFSET 1) GROUP BY 1',
	'SELECT o.grp, count(*) FROM @T o WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.doc) AS i(q int) WHERE i.q < o.rf) GROUP BY 1'
]) q;

DROP FUNCTION same(text);
DROP TABLE sc, sh;
DROP EXTENSION pg_sift;
