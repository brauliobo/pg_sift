-- The sift_colstub access method (SIFT_TESTING builds): the definition of an index is validated when it is created, the layout
-- lists the leaves of the column source, and the index is a shell that no ordinary plan uses.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE d (id int PRIMARY KEY, store_id int, order_date date, segment text, kind varchar(8), tag char(3), nm name, price float4,
				cost float8, qty int2, big int8, at timestamp, atz timestamptz, ok bool, n numeric, metrics jsonb, items jsonb, details jsonb);

-- the supported types, one leaf each
CREATE INDEX d_scalar ON d USING sift_colstub (id, store_id, order_date, segment, kind, tag, nm, price, cost, qty, big, at, atz, ok);
SELECT leaf, attno, parent, kind, vtype, typid, path, name, expr FROM sift._test_aggstub_layout('d_scalar') ORDER BY leaf;
SELECT sift._test_aggstub_is_index(relam) AS is_index FROM pg_class WHERE relname = 'd_scalar';
SELECT sift._test_aggstub_is_index(relam) AS is_index FROM pg_class WHERE relname = 'd_pkey';
DROP INDEX d_scalar;

-- expressions are leaves too; their expression is the one of the index, over the table row
CREATE INDEX d_expr ON d USING sift_colstub (id, (store_id + 1), (lower(segment)), (order_date + 7));
SELECT leaf, attno, vtype, typid, expr FROM sift._test_aggstub_layout('d_expr') ORDER BY leaf;
DROP INDEX d_expr;

-- the reference index of the analytics queries: scalars, a free-key object, an array of objects and a scalar path
CREATE INDEX d_col ON d USING sift_colstub (id, store_id, order_date, segment, kind, metrics, items, details)
	WITH (paths = 'metrics: *:float8; items: []{category text, sku text, qty int4, price float8}; details: lang text, nested.at timestamp, nested.ok bool');
SELECT leaf, attno, parent, kind, vtype, typid, path, name, expr FROM sift._test_aggstub_layout('d_col') ORDER BY leaf;
SELECT pg_get_indexdef('d_col'::regclass);
DROP INDEX d_col;

-- a path below an object, an array below an object, quoted names, any case and blanks of the grammar
CREATE INDEX d_col ON d USING sift_colstub (metrics, items, details)
	WITH (paths = ' metrics : *:Double Precision ; items : a.b[] { "x y" INTEGER , "z""q" bigint } ; details : "a.b".c Date , d BOOLEAN ; ');
SELECT leaf, attno, parent, kind, vtype, typid, path, name FROM sift._test_aggstub_layout('d_col') ORDER BY leaf;
DROP INDEX d_col;

-- what the definition refuses
CREATE INDEX ON d USING sift_colstub (id) WHERE id > 0;
CREATE INDEX ON d USING sift_colstub (id) INCLUDE (store_id);
CREATE UNIQUE INDEX ON d USING sift_colstub (id);
CREATE INDEX ON d USING sift_colstub (n);
CREATE INDEX ON d USING sift_colstub (id, (ARRAY[id]));
CREATE INDEX ON d USING sift_colstub (metrics);
CREATE INDEX ON d USING sift_colstub ((metrics || '{}'::jsonb)) WITH (paths = 'metrics: *:float8');
CREATE INDEX ON d USING sift_colstub (id, details) WITH (paths = 'metrics: *:float8');
CREATE INDEX ON d USING sift_colstub (id, metrics) WITH (paths = 'id: *:float8');
CREATE INDEX ON d USING sift_colstub (id, metrics) WITH (paths = 'metrics: *:float8; metrics: *:float8');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: *:float8, a.b int4');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a.b int4, *:float8');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a.b numeric');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a.b');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a.b int4,');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics a.b int4');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a..b int4');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a.b[]{}');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a.b[]{x}');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a.b[]{x int4');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: "a int4');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = 'metrics: a int4 x');
CREATE INDEX ON d USING sift_colstub (metrics) WITH (paths = '');
CREATE INDEX ON d USING sift_colstub (id) WITH (fillfactor = 50);
SELECT count(*) AS indexes FROM pg_index WHERE indrelid = 'd'::regclass;

-- more leaves than a source can hold
SELECT 'CREATE INDEX ON d USING sift_colstub (details) WITH (paths = ''details: ' ||
	string_agg('k' || i || ' int4', ', ') || ''')' AS ddl FROM generate_series(1, 129) i \gexec

-- values the index would refuse, found by CREATE INDEX
CREATE TABLE dv (id int, s text, details jsonb);
INSERT INTO dv VALUES (1, repeat('x', 255), '{"k": 1}'), (2, 'short', '{"k": "2"}');
CREATE INDEX dv_col ON dv USING sift_colstub (id, s, details) WITH (paths = 'details: k int4, t text');
INSERT INTO dv VALUES (3, repeat('x', 256), NULL);
INSERT INTO dv VALUES (4, 'a', '{"k": 7.5}');
INSERT INTO dv VALUES (5, 'a', '{"k": "seven"}');
INSERT INTO dv VALUES (6, 'a', jsonb_build_object('t', repeat('y', 256)));
INSERT INTO dv VALUES (7, 'a', '{"k": [1]}');
INSERT INTO dv VALUES (8, 'a', '{"k": 8, "t": {"a": [1, "b"]}}');
-- the same casts in SQL fail the same way
SELECT ('{"k": 7.5}'::jsonb #>> '{k}')::int4;
SELECT ('{"k": [1]}'::jsonb #>> '{k}')::int4;
-- the aborted rows stay in the heap until VACUUM, where the raw read finds them
SELECT count(*) AS dump_fails FROM (SELECT 1 FROM sift._test_aggstub_dump('dv_col', 1)) x;
VACUUM dv;
SELECT count(*) AS rows_after_vacuum FROM sift._test_aggstub_dump('dv_col', 1);
DROP INDEX dv_col;
INSERT INTO dv VALUES (9, repeat('z', 300), NULL);
CREATE INDEX dv_col ON dv USING sift_colstub (id, s, details) WITH (paths = 'details: k int4');
CREATE INDEX dv_col ON dv USING sift_colstub (id, details) WITH (paths = 'details: k int4');
SELECT count(*) AS rows FROM sift._test_aggstub_dump('dv_col', 0);

-- never chosen by an ordinary plan: no operators, and a cost that is never the best
CREATE FUNCTION plan_uses(q text, idx text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l LIKE '%' || idx || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE INDEX d_stub ON d USING sift_colstub (id, store_id, segment);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT plan_uses('SELECT * FROM d WHERE store_id = 5', 'd_stub') AS eq_filter,
	   plan_uses('SELECT store_id FROM d ORDER BY store_id', 'd_stub') AS order_by,
	   plan_uses('SELECT store_id FROM d WHERE store_id > 1', 'd_stub') AS index_only,
	   plan_uses('SELECT * FROM d WHERE id = 5 AND store_id = 5 AND segment = ''x''', 'd_stub') AS all_columns,
	   plan_uses('SELECT * FROM d WHERE id = 5', 'd_pkey') AS control_plan_uses_pkey;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP INDEX d_stub;

-- an index of this kind works through DML, maintenance and the other index builds
CREATE TABLE m (id int, s text, metrics jsonb);
CREATE INDEX m_col ON m USING sift_colstub (id, s, metrics) WITH (paths = 'metrics: *:float8');
INSERT INTO m SELECT i, 'v' || i, jsonb_build_object('a', i) FROM generate_series(1, 50) i;
UPDATE m SET s = 's' || id WHERE id % 2 = 0;
DELETE FROM m WHERE id > 40;
VACUUM m;
ANALYZE m;
REINDEX INDEX m_col;
REINDEX INDEX CONCURRENTLY m_col;
CREATE INDEX CONCURRENTLY m_col2 ON m USING sift_colstub (id, s);
SELECT count(*) AS rows FROM sift._test_aggstub_dump('m_col', 0);
SELECT count(*) AS rows FROM sift._test_aggstub_dump('m_col2', 0);
CREATE UNLOGGED TABLE mu (id int, s text);
CREATE INDEX mu_col ON mu USING sift_colstub (id, s);
INSERT INTO mu VALUES (1, 'a');
SELECT count(*) AS rows FROM sift._test_aggstub_dump('mu_col', 1);
CREATE TEMP TABLE mt (id int, s text);
CREATE INDEX mt_col ON mt USING sift_colstub (id, s);
INSERT INTO mt VALUES (1, 'a');
SELECT count(*) AS rows FROM sift._test_aggstub_dump('mt_col', 1);
-- a source needs the index
SELECT count(*) FROM sift._test_aggstub_dump('d_pkey', 0);
SELECT count(*) FROM sift._test_aggstub_dump('m_col', 7);

DROP TABLE d, dv, m, mu;
