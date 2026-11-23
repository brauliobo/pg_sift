-- The pull of an index of format major 2 (sift_f2_pull, col/f2/f2_sink_ext.h) and the unnest primitives of the aggregate scan (agg/v6/v6_unnest.h), through sift._test_f2_pull: the rows and the
-- elements a consumer reads, tested by the test itself, beside what the pull left unread. Each check says whether the answer is the heap's and whether the pruning happened.
--   rows     key marks (SEC_KEY_MARKS), zones of a leaf, a key no part holds
--   weights  the pairs of a parent from the layout alone: no element chunk is read
--   elems    the elements of the live parents, zones of an element leaf, a granule read for its poison rows with no element
--   codes    SVEC_DICT32 children counted in direct cells, and a text child no dictionary codes
--   semi     EXISTS over the elements and the NULL, empty and non-array documents it must leave alone
-- with a deletion vector, pending deletes, the deletes of the transaction itself and the rows of the slab.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.test_create_format1 = off;

CREATE FUNCTION sift._test_f2_pull(index regclass, mode text, hint bool, preds text[], group_att int4, children int4[], elem_preds text[],
	OUT a int8, OUT b int8, OUT c int8, OUT d int8, OUT fallback int8, OUT groups text, OUT partitions_skipped int8, OUT parts_skipped int8, OUT key_skipped int8,
	OUT zone_skipped int8, OUT elem_skipped int8, OUT elem_elided int8, OUT granules_read int8, OUT dict_granules int8, OUT str_granules int8, OUT child_chunks int8)
	RETURNS record AS '$libdir/pg_sift', 'sift_test_f2_pull' LANGUAGE C;
CREATE FUNCTION chk(label text, ok bool) RETURNS text LANGUAGE sql AS $$ SELECT label || ': ' || CASE WHEN ok THEN 'ok' ELSE 'FAILED' END $$;

-- the attributes of the index: id 1, store 2, placed 3, items 4; the children of items: category 0, kind 1, sku 2, qty 3, price 4. Documents: NULL, an empty array, arrays of 1 to 4 elements (a NULL
-- kind where the key is missing, a sku of its own, a qty that grows with the id), and, in the first granules only, an object and a string that no array column holds
CREATE TABLE o (id int PRIMARY KEY, store int, placed int, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO o SELECT i, 1 + i % 40, i / 100,
	CASE WHEN i % 97 = 0 THEN NULL
		WHEN i % 101 = 0 AND i < 12000 THEN '{"x": 1}'::jsonb
		WHEN i % 103 = 0 AND i < 12000 THEN '"scalar"'::jsonb
		WHEN i % 5 = 0 THEN '[]'::jsonb
		ELSE (SELECT jsonb_agg(jsonb_strip_nulls(jsonb_build_object('category', 'c' || (i + j) % 6, 'kind', CASE WHEN (i + j) % 11 = 0 THEN NULL ELSE 'k' || (i * j) % 3 END,
				'sku', 'u' || (i * 10 + j), 'qty', 1 + (i / 8192) * 100 + j, 'price', (i % 50) * 1.5))) FROM generate_series(1, 1 + i % 4) j) END
FROM generate_series(1, 40000) i;
CREATE INDEX o_col ON o USING sift_col (id, store, placed, items) WITH (key_order = id, paths = 'items[]{category text, kind text, sku text, qty int4, price float8}');
ANALYZE o;
CREATE FUNCTION pull(mode text, hint bool, preds text[] DEFAULT NULL, grp int DEFAULT 0, children int[] DEFAULT NULL, epreds text[] DEFAULT NULL)
	RETURNS TABLE (a int8, b int8, c int8, d int8, fallback int8, groups text, partitions_skipped int8, parts_skipped int8, key_skipped int8, zone_skipped int8, elem_skipped int8,
		elem_elided int8, granules_read int8, dict_granules int8, str_granules int8, child_chunks int8)
	LANGUAGE sql AS $$ SELECT * FROM sift._test_f2_pull('o_col', $1, $2, $3, $4, $5, $6) $$;

-- the rows of a pull are the rows of the table; a predicate of the pull is a hint
SELECT chk('rows: all', (SELECT a = (SELECT count(*) FROM o) AND c = a AND granules_read > 1 FROM pull('rows', false)));
SELECT chk('rows: a zone of a leaf', h.a = w.n AND h.b = w.s AND u.a = w.n AND u.b = w.s AND h.zone_skipped > 0 AND h.granules_read < u.granules_read)
	FROM pull('rows', true, ARRAY['3:ge:200', '3:le:250']) h, pull('rows', false, ARRAY['3:ge:200', '3:le:250']) u, (SELECT count(*) n, sum(placed) s FROM o WHERE placed BETWEEN 200 AND 250) w;
SELECT chk('rows: the key marks', h.a = w.n AND h.b = w.s AND u.a = w.n AND u.b = w.s AND h.key_skipped > 0 AND h.granules_read < u.granules_read)
	FROM pull('rows', true, ARRAY['1:ge:30000']) h, pull('rows', false, ARRAY['1:ge:30000']) u, (SELECT count(*) n, sum(id) s FROM o WHERE id >= 30000) w;
SELECT chk('rows: a point of the key', h.a = 1 AND h.b = 12345 AND h.key_skipped > 0 AND h.granules_read <= 2)
	FROM pull('rows', true, ARRAY['1:eq:12345']) h;
SELECT chk('rows: a range between keys and a column no zone orders', h.a = w.n AND u.a = w.n AND h.key_skipped > 0)
	FROM pull('rows', true, ARRAY['1:ge:5000', '1:lt:9000', '2:eq:7']) h, pull('rows', false, ARRAY['1:ge:5000', '1:lt:9000', '2:eq:7']) u,
		(SELECT count(*) n FROM o WHERE id >= 5000 AND id < 9000 AND store = 7) w;
SELECT chk('rows: predicates that contradict', h.a = 0 AND h.granules_read = 0 AND h.partitions_skipped > 0)
	FROM pull('rows', true, ARRAY['1:ge:100', '1:le:50']) h;
SELECT chk('rows: a key past the table', h.a = 0 AND h.granules_read = 0)
	FROM pull('rows', true, ARRAY['1:gt:99999']) h;

-- the weight of a parent: the pairs of every unnest of it, from the layout; the elements are not read, and the answer is the one of the pairs
SELECT chk('weights: pairs and no element chunk', w.a = (SELECT sum(jsonb_array_length(items)) FROM o WHERE jsonb_typeof(items) = 'array') AND w.child_chunks = 0 AND w.a = e.a
		AND w.d = (SELECT sum(id * jsonb_array_length(items)) FROM o WHERE jsonb_typeof(items) = 'array')
		AND w.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) NOT IN ('array')) AND e.child_chunks > 0)
	FROM pull('weights', false, ARRAY['1:ge:1'], 4) w, pull('elems', false, ARRAY['1:ge:1'], 4, ARRAY[3]) e;
SELECT chk('weights: parents that pass', w.b = (SELECT count(*) FROM o WHERE placed BETWEEN 200 AND 250 AND (items IS NULL OR jsonb_typeof(items) = 'array'))
		AND w.a = (SELECT sum(jsonb_array_length(items)) FROM o WHERE placed BETWEEN 200 AND 250 AND jsonb_typeof(items) = 'array') AND w.child_chunks = 0
		AND w.zone_skipped > 0 AND w.a = u.a AND w.d = u.d)
	FROM pull('weights', true, ARRAY['3:ge:200', '3:le:250'], 4) w, pull('weights', false, ARRAY['3:ge:200', '3:le:250'], 4) u;

SELECT chk('weights: a left join', w.a = (SELECT sum(coalesce(greatest(jsonb_array_length(items), 1), 1)) FROM o WHERE items IS NULL OR jsonb_typeof(items) = 'array')
		AND w.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) <> 'array') AND w.child_chunks = 0)
	FROM pull('weights_left', false, NULL, 4) w;

-- the elements of the live parents, and the zones of an element leaf: a granule none of whose elements passes is not read, or read for its poison rows alone, with no element
SELECT chk('elems: all of them', e.a = (SELECT count(*) FROM o, jsonb_to_recordset(items) AS i(qty int) WHERE jsonb_typeof(items) = 'array')
		AND e.b = (SELECT sum(qty) FROM o, jsonb_to_recordset(items) AS i(qty int) WHERE jsonb_typeof(items) = 'array') AND e.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) <> 'array'))
	FROM pull('elems', false, NULL, 4, ARRAY[3]) e;
SELECT chk('elems: zones of an element leaf', h.a = w.n AND h.b = w.s AND u.a = w.n AND u.b = w.s AND h.fallback = u.fallback AND h.elem_skipped > 0 AND h.elem_elided > 0
		AND h.child_chunks < u.child_chunks AND h.granules_read < u.granules_read + h.elem_skipped)
	FROM pull('elems', true, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) h, pull('elems', false, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) u,
		(SELECT count(*) n, sum(qty) s FROM o, jsonb_to_recordset(items) AS i(qty int) WHERE jsonb_typeof(items) = 'array' AND qty >= 350) w;
SELECT chk('elems: parents and elements together', h.a = w.n AND u.a = w.n AND h.b = w.s AND h.zone_skipped + h.key_skipped > 0)
	FROM pull('elems', true, ARRAY['1:ge:20000', '3:le:330'], 4, ARRAY[3], ARRAY['0:ge:150', '0:le:260']) h, pull('elems', false, ARRAY['1:ge:20000', '3:le:330'], 4, ARRAY[3], ARRAY['0:ge:150', '0:le:260']) u,
		(SELECT count(*) n, sum(qty) s FROM o, jsonb_to_recordset(items) AS i(qty int) WHERE jsonb_typeof(items) = 'array' AND id >= 20000 AND placed <= 330 AND qty >= 150 AND qty <= 260) w;
SELECT chk('elems: no element passes', h.a = 0 AND h.elem_skipped + h.elem_elided > 0 AND h.child_chunks = 0)
	FROM pull('elems', true, NULL, 4, ARRAY[3], ARRAY['0:gt:100000']) h;

-- dictionary codes of the children, counted in direct cells; a NULL element is a cell of its own
SELECT chk('codes: two dictionaries', c.groups = (SELECT string_agg(format('%s|%s=%s', coalesce(category, '~'), coalesce(kind, '~'), n), ';' ORDER BY coalesce(category, '~') COLLATE "C", coalesce(kind, '~') COLLATE "C")
		FROM (SELECT category, kind, count(*) n FROM o, jsonb_to_recordset(items) AS i(category text, kind text) WHERE jsonb_typeof(items) = 'array' GROUP BY 1, 2) q)
		AND c.dict_granules > 0 AND c.str_granules = 0 AND c.a = (SELECT sum(jsonb_array_length(items)) FROM o WHERE jsonb_typeof(items) = 'array'))
	FROM pull('codes', false, NULL, 4, ARRAY[0, 1]) c;
SELECT chk('codes: one dictionary and a filter of the elements', c.groups = (SELECT string_agg(format('%s|=%s', coalesce(category, '~'), n), ';' ORDER BY coalesce(category, '~') COLLATE "C")
		FROM (SELECT category, count(*) n FROM o, jsonb_to_recordset(items) AS i(category text, qty int) WHERE jsonb_typeof(items) = 'array' AND qty >= 300 GROUP BY 1) q)
		AND c.elem_skipped + c.elem_elided > 0)
	FROM pull('codes', true, NULL, 4, ARRAY[0, 3], ARRAY['1:ge:300']) c;
SELECT chk('codes: a text child no dictionary codes', c.groups = (SELECT string_agg(format('%s|=%s', sku, n), ';' ORDER BY sku COLLATE "C") FROM (SELECT sku, count(*) n FROM o, jsonb_to_recordset(items) AS i(sku text, qty int)
			WHERE jsonb_typeof(items) = 'array' AND qty = 103 AND id BETWEEN 9000 AND 9300 GROUP BY 1) q) AND c.str_granules > 0)
	FROM pull('codes', true, ARRAY['1:ge:9000', '1:le:9300'], 4, ARRAY[2, 3], ARRAY['1:eq:103']) c;

-- EXISTS over the elements: a NULL document, an empty array and an element that fails are no hit; the parents that are not answered are the poison ones
SELECT chk('semi: EXISTS', s.a = (SELECT count(*) FROM o WHERE jsonb_typeof(items) = 'array' AND EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS i(qty int) WHERE qty >= 350))
		AND s.b - s.a = (SELECT count(*) FROM o WHERE (items IS NULL OR jsonb_typeof(items) = 'array') AND NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS i(qty int) WHERE qty >= 350))
		AND s.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) <> 'array') AND s.a = h.a AND h.elem_skipped + h.elem_elided > 0)
	FROM pull('semi', false, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) s, pull('semi', true, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) h;
SELECT chk('any: three-valued, the poison left out', y.a = (SELECT count(*) FROM o WHERE jsonb_typeof(items) = 'array' AND EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS i(qty int) WHERE qty >= 350))
		AND y.b = (SELECT count(*) FROM o WHERE jsonb_typeof(items) = 'array'))
	FROM pull('any', false, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) y;
SELECT chk('semi: any element', s.a = (SELECT count(*) FROM o WHERE jsonb_typeof(items) = 'array' AND jsonb_array_length(CASE WHEN jsonb_typeof(items) = 'array' THEN items END) > 0) AND s.child_chunks > 0)
	FROM pull('semi', false, NULL, 4, ARRAY[3]) s;
SELECT chk('semi: distinct parents of the pairs', s.a = (SELECT count(DISTINCT id) FROM o, jsonb_to_recordset(items) AS i(qty int) WHERE jsonb_typeof(items) = 'array' AND qty BETWEEN 140 AND 220 AND placed < 300))
	FROM pull('semi', true, ARRAY['3:lt:300'], 4, ARRAY[3], ARRAY['0:ge:140', '0:le:220']) s;

-- the rows a delete has ended: pending deletes, then the transaction's own, then a deletion vector, and rows in the slab; the rows of the pull are the heap's after each
DELETE FROM o WHERE id % 7 = 0;
SELECT chk('deletes pending: rows', (SELECT a FROM pull('rows', false)) = (SELECT count(*) FROM o));
SELECT chk('deletes pending: weights', w.a = (SELECT sum(jsonb_array_length(items)) FROM o WHERE jsonb_typeof(items) = 'array') AND w.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) <> 'array'))
	FROM pull('weights', true, ARRAY['1:ge:1'], 4) w;
SELECT chk('deletes pending: elements and codes', e.a = (SELECT count(*) FROM o, jsonb_to_recordset(items) AS i(qty int) WHERE jsonb_typeof(items) = 'array' AND qty >= 350)
		AND c.groups = (SELECT string_agg(format('%s|=%s', category, n), ';' ORDER BY category COLLATE "C") FROM (SELECT category, count(*) n FROM o, jsonb_to_recordset(items) AS i(category text, qty int)
			WHERE jsonb_typeof(items) = 'array' AND qty >= 350 GROUP BY 1) q))
	FROM pull('elems', true, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) e, pull('codes', true, NULL, 4, ARRAY[0, 3], ARRAY['1:ge:350']) c;
BEGIN;
DELETE FROM o WHERE id % 11 = 0;
SELECT chk('deletes of the transaction: rows', (SELECT a FROM pull('rows', false)) = (SELECT count(*) FROM o));
SELECT chk('deletes of the transaction: semi', (SELECT a FROM pull('semi', true, NULL, 4, ARRAY[3], ARRAY['0:ge:350'])) = (SELECT count(*) FROM o WHERE jsonb_typeof(items) = 'array'
	AND EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS i(qty int) WHERE qty >= 350)));
ROLLBACK;
SELECT chk('deletes rolled back: rows', (SELECT a FROM pull('rows', false)) = (SELECT count(*) FROM o));
VACUUM o;
SELECT chk('deletion vector: rows', (SELECT a FROM pull('rows', true, ARRAY['3:ge:200', '3:le:250'])) = (SELECT count(*) FROM o WHERE placed BETWEEN 200 AND 250));
SELECT chk('deletion vector: weights', w.a = (SELECT sum(jsonb_array_length(items)) FROM o WHERE jsonb_typeof(items) = 'array') AND w.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) <> 'array') AND w.child_chunks = 0)
	FROM pull('weights', false, NULL, 4) w;
SELECT chk('deletion vector: semi', (SELECT a FROM pull('semi', true, NULL, 4, ARRAY[3], ARRAY['0:ge:350'])) = (SELECT count(*) FROM o WHERE jsonb_typeof(items) = 'array'
	AND EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS i(qty int) WHERE qty >= 350)));
INSERT INTO o SELECT i, 1 + i % 40, i / 100, jsonb_build_array(jsonb_build_object('category', 'c9', 'kind', 'k9', 'sku', 'n' || i, 'qty', 900, 'price', 1.5)) FROM generate_series(40001, 40100) i;
SELECT chk('slab: rows', (SELECT a FROM pull('rows', true, ARRAY['1:ge:39000'])) = (SELECT count(*) FROM o WHERE id >= 39000));
SELECT chk('slab: weights', w.a = (SELECT sum(jsonb_array_length(items)) FROM o WHERE jsonb_typeof(items) = 'array' AND id <= 40000) AND w.child_chunks = 0
		AND w.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) <> 'array' OR id > 40000))
	FROM pull('weights', false, NULL, 4) w;
SELECT chk('slab: elements and semi', e.a = (SELECT count(*) FROM o, jsonb_to_recordset(items) AS i(qty int) WHERE jsonb_typeof(items) = 'array' AND qty >= 350 AND id <= 40000)
		AND s.a = (SELECT count(*) FROM o WHERE jsonb_typeof(items) = 'array' AND id <= 40000 AND EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS i(qty int) WHERE qty >= 350))
		AND e.fallback = s.fallback AND e.fallback = (SELECT count(*) FROM o WHERE jsonb_typeof(items) <> 'array' OR id > 40000))
	FROM pull('elems', true, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) e, pull('semi', true, NULL, 4, ARRAY[3], ARRAY['0:ge:350']) s;

DROP TABLE o;
DROP FUNCTION pull(text, bool, text[], int, int[], text[]);
DROP FUNCTION chk(text, bool);
DROP FUNCTION sift._test_f2_pull(regclass, text, bool, text[], int4, int4[], text[]);
DROP EXTENSION pg_sift;
