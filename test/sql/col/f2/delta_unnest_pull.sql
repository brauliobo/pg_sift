-- An UPDATE that changes no array of a row of a published part is a column delta record: the row keeps its stored arrays, and its new heap address and the new cells of the scalars it changed
-- are laid over the part. The pull of the index (sift_f2_pull, read by the unnest of SiftAggV6) must read them as every other scan does: the rows an update moved are rows of the heap at their
-- new address, a scalar an update changed is read at its new value, in the vectors and in the predicates the zones of the part are asked for. Every query runs through the node and on a heap
-- twin the same UPDATEs ran on (no index, no sift settings); the first word says that the answers agree, the second that the plan is the node's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_create_format1 = off;
SET sift.agg_force = on;
SET sift.simd = scalar;
SET jit = off;

CREATE FUNCTION sift._test_f2_pull(index regclass, mode text, hint bool, preds text[], group_att int4, children int4[], elem_preds text[],
	OUT a int8, OUT b int8, OUT c int8, OUT d int8, OUT fallback int8, OUT groups text, OUT partitions_skipped int8, OUT parts_skipped int8, OUT key_skipped int8,
	OUT zone_skipped int8, OUT elem_skipped int8, OUT elem_elided int8, OUT granules_read int8, OUT dict_granules int8, OUT str_granules int8, OUT child_chunks int8)
	RETURNS record AS '$libdir/pg_sift', 'sift_test_f2_pull' LANGUAGE C;

CREATE TABLE u (id int NOT NULL, v int, other int, tag text, arr int[], items jsonb) WITH (autovacuum_enabled = false);
CREATE TABLE uh (LIKE u) WITH (autovacuum_enabled = false);
CREATE TABLE s (id int PRIMARY KEY, v int, tag text, arr int[]) USING sift_table;
CREATE TABLE sh (LIKE s);
CREATE INDEX u_other ON u (other);
CREATE INDEX u_idx ON u USING sift_col (id, v, tag, arr, items) WITH (key_order = 'id', paths = 'arr[] int4, items[]{kind text, qty int4}');
INSERT INTO u SELECT i, i % 50, i % 7, 't' || i % 4, ARRAY[i % 5, i % 3, 100],
	jsonb_build_array(jsonb_build_object('kind', 'k' || i % 3, 'qty', i % 9), jsonb_build_object('kind', 'k' || i % 2, 'qty', 1)) FROM generate_series(1, 20000) i;
INSERT INTO uh SELECT * FROM u;
INSERT INTO s SELECT id, v, tag, arr FROM u;
INSERT INTO sh SELECT * FROM s;
VACUUM (FREEZE, ANALYZE) u, uh;
ANALYZE s;
SELECT busy FROM sift.table_maintain('s');

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	a := answer(q);
	SET LOCAL sift.enable_agg = off;
	b := answer(regexp_replace(regexp_replace(q, '\yFROM u\y', 'FROM uh'), '\yFROM s\y', 'FROM sh'));
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6%' THEN 'node' ELSE 'normal' END) || ' ' || q;
END $$;

CREATE FUNCTION twin(stmt text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(stmt, '%t', 'u');
	EXECUTE replace(stmt, '%t', 'uh');
END $$;

CREATE FUNCTION pull(mode text, hint bool, preds text[] DEFAULT NULL)
	RETURNS TABLE (a int8, b int8, c int8, zone_skipped int8) LANGUAGE sql AS
	$$ SELECT a, b, c, zone_skipped FROM sift._test_f2_pull('u_idx', $1, $2, $3, 0, NULL, NULL) $$;

CREATE TEMP TABLE queries AS SELECT q FROM unnest(ARRAY[
	'SELECT e, count(*), sum(v) FROM u, unnest(arr) e GROUP BY e',
	'SELECT e, tag, count(*) FROM u, unnest(arr) e GROUP BY e, tag',
	'SELECT e, count(*) FROM u, unnest(arr) e WHERE v < 10 GROUP BY e',
	'SELECT e, count(*) FROM u, unnest(arr) e WHERE v IS NULL GROUP BY e',
	'SELECT e, count(*) FROM u, unnest(arr) e WHERE v >= 1000 GROUP BY e',
	'SELECT e, count(*), sum(v) FROM u LEFT JOIN LATERAL unnest(arr) e ON true GROUP BY e',
	'SELECT i.kind, count(*), sum(v) FROM u, jsonb_to_recordset(items) AS i(kind text, qty int) GROUP BY 1',
	'SELECT i.qty, tag, count(*) FROM u, jsonb_to_recordset(items) AS i(kind text, qty int) WHERE v < 20 GROUP BY 1, 2',
	'SELECT e->>''kind'', count(*), sum(v) FROM u, jsonb_array_elements(items) e GROUP BY 1',
	'SELECT e, count(*), sum(v) FROM s, unnest(arr) e GROUP BY e',
	'SELECT e, tag, count(*) FROM s, unnest(arr) e WHERE v IS NOT NULL GROUP BY e, tag'
]) q;

SELECT same(q) FROM queries;

-- the new address of a row: the B-tree index on other changes, no column of the sift_col index does
SELECT twin('UPDATE %t SET other = other + 1 WHERE id % 10 = 0');
SELECT dead_rows = 0 AS no_tombstones FROM sift.status('u_idx');
SELECT same(q) FROM queries;

-- a scalar of the index changes: a value past the stored zones, and a NULL
SELECT twin('UPDATE %t SET v = v + 1000 WHERE id % 10 = 3');
SELECT twin('UPDATE %t SET v = NULL WHERE id % 10 = 7');
UPDATE s SET v = v + 1000, tag = 'zz' WHERE id % 10 = 3;
UPDATE sh SET v = v + 1000, tag = 'zz' WHERE id % 10 = 3;
UPDATE s SET v = NULL WHERE id % 10 = 7;
UPDATE sh SET v = NULL WHERE id % 10 = 7;
SELECT dead_rows = 0 AS no_tombstones FROM sift.status('u_idx');
SELECT same(q) FROM queries;

-- a string of the index changes
SELECT twin('UPDATE %t SET tag = ''zz'' || id % 3 WHERE id % 10 = 5');
SELECT same(q) FROM queries;

-- the transaction that wrote the records sees them, and a rolled back one leaves nothing
BEGIN;
SELECT twin('UPDATE %t SET v = v + 5000, other = other + 1 WHERE id % 10 = 1');
SELECT same(q) FROM queries;
SELECT h.a = w.n AND h.b = w.s AS pulled_own FROM pull('rows', true, ARRAY['2:ge:5000']) h, (SELECT count(*) n, sum(v) s FROM uh WHERE v >= 5000) w;
ROLLBACK;
SELECT same(q) FROM queries;

-- the pull on its own: the values it reads, and the granules the zones of a leaf leave for a predicate on a leaf that was patched
SELECT h.a = w.n AND h.b = w.s AND h.zone_skipped = 0 AND h.c = (SELECT count(*) FROM uh) AS pulled_rows
	FROM pull('rows', true, ARRAY['2:ge:1000']) h, (SELECT count(*) n, sum(v) s FROM uh WHERE v >= 1000) w;
SELECT h.a = w.n AND h.b = w.s AS pulled_unhinted FROM pull('rows', false, ARRAY['2:ge:1000']) h, (SELECT count(*) n, sum(v) s FROM uh WHERE v >= 1000) w;
SELECT h.a = w.n AND h.b = w.s AS pulled_below FROM pull('rows', true, ARRAY['2:le:3']) h, (SELECT count(*) n, sum(v) s FROM uh WHERE v <= 3) w;

-- the same after the part was rewritten
VACUUM (FREEZE) u, uh;
SELECT same(q) FROM queries;
SELECT h.a = w.n AND h.b = w.s AS pulled_after_vacuum FROM pull('rows', true, ARRAY['2:ge:1000']) h, (SELECT count(*) n, sum(v) s FROM uh WHERE v >= 1000) w;
SELECT count(*) AS check_problems FROM sift.check('u_idx'::regclass);

DROP TABLE u, uh, s, sh;
DROP FUNCTION same(text), answer(text), twin(text), pull(text, bool, text[]), sift._test_f2_pull(regclass, text, bool, text[], int4, int4[], text[]);
DROP EXTENSION pg_sift;
