-- An UPDATE of a document of a row of a published part is a column delta record when it changes few leaves. A key the UPDATE added or removed, a promoted value it moved out of the
-- range of the zone, and a sort value it moved past the zone of a granule must be decided on the new row, not on the zones of the part: the rows are those of the heap twin the same
-- UPDATEs ran on: ? through the bitmap scan and SiftAggV6, a promoted leaf and the top-N of 40 tenants through SiftAggV6, and ?|, ?&, @>, @? as the heap decides them.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE t (id int NOT NULL, a int, b int, c int, doc jsonb);
CREATE TABLE th (LIKE t);
INSERT INTO t SELECT i, i % 7, i % 11, i % 13, jsonb_build_object('k1', i, 'p', (10 + 10 * (i % 5))::text) FROM generate_series(1, 1000) i;
INSERT INTO th SELECT * FROM t;
CREATE INDEX t_c ON t USING sift_col (id, a, b, c, doc sift.col_jsonb_ops) WITH (paths = 'doc.k1 int, doc.flag text, doc.p astext');

CREATE TABLE tp (id int NOT NULL, tenant int NOT NULL, ts timestamp NOT NULL, v int) PARTITION BY HASH (tenant);
CREATE TABLE tp_0 PARTITION OF tp FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE tp_1 PARTITION OF tp FOR VALUES WITH (MODULUS 2, REMAINDER 1);
CREATE TABLE tph (LIKE tp);
INSERT INTO tp SELECT i, i % 40, timestamp '2024-01-01' + i * interval '1 second', i % 50 FROM generate_series(1, 8000) i;
INSERT INTO tph SELECT * FROM tp;
CREATE INDEX tp_c ON tp USING sift_col (id, tenant, ts, v) WITH (order_array_columns = 'ts', order_array_group = 'tenant');
VACUUM (FREEZE, ANALYZE) t, th, tp, tph;

-- a key that no row has gains it; the key every row has leaves some; a promoted value leaves its range, and a number becomes a word; a ts moves past the greatest of its granule
UPDATE t SET doc = jsonb_set(doc, '{flag}', '"a"') WHERE id <= 5;
UPDATE th SET doc = jsonb_set(doc, '{flag}', '"a"') WHERE id <= 5;
UPDATE t SET doc = jsonb_set(doc, '{flag}', 'null'::jsonb) WHERE id = 6;
UPDATE th SET doc = jsonb_set(doc, '{flag}', 'null'::jsonb) WHERE id = 6;
UPDATE t SET doc = doc - 'k1' WHERE id BETWEEN 11 AND 15;
UPDATE th SET doc = doc - 'k1' WHERE id BETWEEN 11 AND 15;
UPDATE t SET doc = jsonb_set(doc, '{p}', '"999"') WHERE id = 20;
UPDATE th SET doc = jsonb_set(doc, '{p}', '"999"') WHERE id = 20;
UPDATE t SET doc = jsonb_set(doc, '{p}', '"abc"') WHERE id = 21;
UPDATE th SET doc = jsonb_set(doc, '{p}', '"abc"') WHERE id = 21;
UPDATE tp SET ts = timestamp '2026-01-01' + id * interval '1 second' WHERE id % 211 = 0;
UPDATE tph SET ts = timestamp '2026-01-01' + id * interval '1 second' WHERE id % 211 = 0;
INSERT INTO tp SELECT 20000 + i, i % 40, timestamp '2025-04-01' + i * interval '1 minute', i % 50 FROM generate_series(1, 400) i;
INSERT INTO tph SELECT * FROM tp WHERE id > 20000;
-- no row of an UPDATE went to the write buffer: only the 400 inserted rows are there
SELECT l2_parts, dirty_rows FROM sift.status('t_c');
SELECT sum(st.dirty_rows) AS tp_dirty_rows FROM pg_index i, LATERAL sift.status(i.indexrelid) st WHERE i.indrelid IN ('tp_0'::regclass, 'tp_1'::regclass);

CREATE FUNCTION chk(n int, q text, rel text, twin text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; need bool := n IN (1, 6) OR (mode = 'agg' AND (n BETWEEN 9 AND 12 OR n >= 21)); ra text; rb text;
BEGIN
	PERFORM set_config('sift.enable_agg', (mode = 'agg')::text, true);
	PERFORM set_config('sift.agg_force', (mode = 'agg')::text, true);
	PERFORM set_config('enable_seqscan', 'false', true);
	PERFORM set_config('enable_indexscan', 'false', true);
	PERFORM set_config('enable_indexonlyscan', 'false', true);
	PERFORM set_config('enable_bitmapscan', (mode = 'index')::text, true);
	PERFORM set_config('enable_sort', (mode <> 'agg')::text, true);
	FOR pl IN EXECUTE 'EXPLAIN (COSTS off) ' || format(q, rel) LOOP
		used := used OR (mode = 'agg' AND pl LIKE '%Custom Scan (SiftAgg%') OR (mode = 'index' AND pl LIKE '%Bitmap Index Scan%');
	END LOOP;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || format(q, rel) || ') r) x' INTO ra;
	PERFORM set_config('sift.enable_agg', 'false', true);
	PERFORM set_config('enable_seqscan', 'true', true);
	PERFORM set_config('enable_indexscan', 'true', true);
	PERFORM set_config('enable_indexonlyscan', 'true', true);
	PERFORM set_config('enable_sort', 'true', true);
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || format(q, twin) || ') r) x' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS q' || n || ': ' || ra || ' vs ' || rb WHEN need AND NOT used THEN 'no scan q' || n ELSE 'same' END;
END $$;

CREATE TABLE q (n int, q text);
INSERT INTO q VALUES
	(1, 'SELECT count(*) FROM %1$s WHERE doc ? ''flag'''),
	(2, 'SELECT count(*) FROM %1$s WHERE doc ?| ARRAY[''flag'', ''nope'']'),
	(3, 'SELECT count(*) FROM %1$s WHERE doc ?& ARRAY[''k1'', ''flag'']'),
	(4, 'SELECT count(*) FROM %1$s WHERE doc @> ''{"flag": "a"}'''),
	(5, 'SELECT count(*) FROM %1$s WHERE doc @? ''$.flag'''),
	(6, 'SELECT count(*) FROM %1$s WHERE doc ? ''k1'''),
	(7, 'SELECT count(*) FROM %1$s WHERE NOT doc ? ''k1'''),
	(8, 'SELECT count(*) FROM %1$s WHERE doc ?& ARRAY[''k1'', ''p'']'),
	(9, 'SELECT count(*) FROM %1$s WHERE doc->>''p'' = ''999'''),
	(10, 'SELECT count(*) FROM %1$s WHERE doc->>''p'' = ''abc'''),
	(11, 'SELECT count(*) FROM %1$s WHERE doc->>''p'' = ''30'''),
	(12, 'SELECT count(*) FROM %1$s WHERE doc->>''p'' IN (''999'', ''abc'', ''10'')');

CREATE TABLE pq (n int, q text);
INSERT INTO pq VALUES
	(21, 'SELECT id, ts FROM %1$s ORDER BY ts DESC, id LIMIT 10'),
	(22, 'SELECT id, ts FROM %1$s WHERE tenant = 17 ORDER BY ts DESC, id LIMIT 5'),
	(23, 'SELECT id, ts FROM %1$s WHERE tenant = 3 ORDER BY ts DESC, id LIMIT 5');

SELECT rel, mode, result, count(*) FROM (
	SELECT 't' AS rel, mode, chk(n, q, 't', 'th', mode) AS result FROM q, unnest(ARRAY['agg', 'index']) mode
	UNION ALL SELECT 'tp', 'agg', chk(n, pq.q, 'tp', 'tph', 'agg') FROM pq
) r GROUP BY rel, mode, result ORDER BY rel, mode, result;

-- the top rows of each of the 40 tenants

SELECT count(*) FILTER (WHERE pl_ok) AS tenants_ok, count(*) AS tenants FROM (
	SELECT chk(24, 'SELECT id, ts FROM %1$s WHERE tenant = ' || g || ' ORDER BY ts DESC, id LIMIT 3', 'tp', 'tph', 'agg') = 'same' AS pl_ok FROM generate_series(0, 39) g
) r;

DROP FUNCTION chk(int, text, text, text, text);
DROP TABLE q, pq, t, th, tp, tph;
DROP EXTENSION pg_sift CASCADE;
