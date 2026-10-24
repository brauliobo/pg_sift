-- The bitmap scan of a sift_col index (docs/COL-SCAN.md): equality, IN lists, ranges, IS [NOT] NULL, several columns, cross-type
-- integer operators and OR/AND of bitmaps on a column index built with the sift.col_*_ops operator classes. Every query runs three ways and
-- must return the same rows: on the heap (sc, no index but the column index, sequential scan), on the column index (sc, bitmap scan) and on
-- btrees (sc_b, a copy that gets the same statements). Over the build, the write buffer, updates, deletes, a rollback, the promotion of the
-- buffer and a merge.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

-- pad makes a page hold about 15 rows: 30000 rows are about 8 superblocks
CREATE TABLE sc (id int, v int, k2 int2, k8 int8, d date, ts timestamp, tz timestamptz, s text, u uuid, g int, f4 float4, f8 float8, b bool, nm numeric(12,2), c char(8), vc varchar(20), pad text DEFAULT repeat('x', 500));
ALTER TABLE sc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sc (id, v, k2, k8, d, ts, tz, s, u, g, f4, f8, b, nm, c, vc)
SELECT i,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE (hashint4(i) & 2147483647) % 500 END,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE ((hashint4(i + 1) & 2147483647) % 200 - 100)::int2 END,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE ((hashint4(i + 2) & 2147483647) % 1000)::int8 * 5000000 - 1000000000 END,
	   CASE WHEN i % 19 = 0 THEN NULL WHEN i = 5 THEN 'infinity'::date ELSE date '2024-01-01' + (i / 100) END,
	   CASE WHEN i % 23 = 0 THEN NULL WHEN i = 7 THEN 'infinity'::timestamp ELSE timestamp '2024-01-01' + i * interval '97 seconds' END,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE timestamptz '2024-01-01 00:00:00+00' + ((hashint4(i + 3) & 2147483647) % 100000) * interval '1 minute' END,
	   CASE WHEN i % 31 = 0 THEN NULL WHEN i % 37 = 0 THEN '' WHEN i % 41 = 0 THEN 'caf' || chr(233) WHEN i % 43 = 0 THEN repeat('long', 100) || i % 3
			ELSE 'k' || (hashint4(i + 4) & 2147483647) % 300 END,
	   CASE WHEN i % 47 = 0 THEN NULL ELSE md5((i % 5000)::text)::uuid END,
	   i % 20,
	   CASE WHEN i % 59 = 0 THEN NULL WHEN i = 15 THEN 'NaN' ELSE ((hashint4(i + 6) & 2147483647) % 500) / 4.0 END,
	   CASE WHEN i % 53 = 0 THEN NULL WHEN i = 11 THEN 'NaN' WHEN i = 12 THEN 'Infinity' WHEN i = 13 THEN '-Infinity' WHEN i = 14 THEN '-0'
			ELSE ((hashint4(i + 5) & 2147483647) % 2000 - 1000) / 8.0 END,
	   CASE WHEN i % 61 = 0 THEN NULL ELSE (hashint4(i + 7) & 1) = 1 END,
	   CASE WHEN i % 67 = 0 THEN NULL WHEN i = 21 THEN 'NaN' ELSE ((hashint4(i + 8) & 2147483647) % 100000)::numeric / 100 END,
	   CASE WHEN i % 71 = 0 THEN NULL WHEN i % 73 = 0 THEN 'caf' || chr(233) WHEN i % 79 = 0 THEN 'ab' ELSE 'c' || (hashint4(i + 9) & 2147483647) % 200 END,
	   CASE WHEN i % 83 = 0 THEN NULL ELSE 'v' || (hashint4(i + 10) & 2147483647) % 150 END
FROM generate_series(1, 30000) i;
INSERT INTO sc (id, v, k2, k8) VALUES (2147483647, 2147483647, 32767, 9223372036854775807), (-2147483648, -2147483648, -32768, -9223372036854775807 - 1);
INSERT INTO sc (id, f8, nm) VALUES (2147483646, 1e308, 9999999999.99), (-2147483647, -1e308, -9999999999.99);
CREATE TABLE sc_b (LIKE sc INCLUDING DEFAULTS);
ALTER TABLE sc_b ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sc_b SELECT * FROM sc;

CREATE INDEX sc_col ON sc USING sift_col (id sift.col_int4_ops, v sift.col_int4_ops, k2 sift.col_int2_ops, k8 sift.col_int8_ops, d sift.col_date_ops,
	ts sift.col_timestamp_ops, tz sift.col_timestamptz_ops, s sift.col_text_ops, u sift.col_uuid_ops, g sift.col_int4_ops,
	f4 sift.col_float4_ops, f8 sift.col_float8_ops, b sift.col_bool_ops, nm sift.col_numeric_ops, c sift.col_bpchar_ops, vc sift.col_text_ops) WITH (write_buffer_limit = 256, return_columns = 's, u, vc');
CREATE INDEX sc_b_id ON sc_b (id);
CREATE INDEX sc_b_v ON sc_b (v);
CREATE INDEX sc_b_k2 ON sc_b (k2);
CREATE INDEX sc_b_k8 ON sc_b (k8);
CREATE INDEX sc_b_d ON sc_b (d);
CREATE INDEX sc_b_ts ON sc_b (ts);
CREATE INDEX sc_b_tz ON sc_b (tz);
CREATE INDEX sc_b_s ON sc_b (s);
CREATE INDEX sc_b_u ON sc_b (u);
CREATE INDEX sc_b_g ON sc_b (g);
CREATE INDEX sc_b_f4 ON sc_b (f4);
CREATE INDEX sc_b_f8 ON sc_b (f8);
CREATE INDEX sc_b_b ON sc_b (b);
CREATE INDEX sc_b_nm ON sc_b (nm);
CREATE INDEX sc_b_c ON sc_b (c);
CREATE INDEX sc_b_vc ON sc_b (vc);
VACUUM (ANALYZE) sc;
VACUUM (ANALYZE) sc_b;

-- a statement on both tables: %T is the table
CREATE FUNCTION sc_dml(stmt text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(stmt, '%T', 'sc');
	EXECUTE replace(stmt, '%T', 'sc_b');
END $$;

CREATE FUNCTION sc_mode(m text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('sift.enable_col_scan', (m IN ('col', 'tup'))::text, true);
	PERFORM set_config('enable_seqscan', (m = 'heap')::text, true);
	PERFORM set_config('enable_indexscan', (m IN ('btree', 'tup'))::text, true);
	PERFORM set_config('enable_indexonlyscan', (m IN ('btree', 'tup'))::text, true);
	PERFORM set_config('enable_bitmapscan', (m IN ('col', 'btree'))::text, true);
END $$;

-- the rows of a query in one mode (count and md5 of the sorted rows) and the index its plan reads
CREATE FUNCTION sc_run(q text, m text, OUT n bigint, OUT md5 text, OUT idx text) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM sc_mode(m);
	q := replace(q, '%T', CASE WHEN m = 'btree' THEN 'sc_b' ELSE 'sc' END);
	idx := '';
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'sc_col' THEN idx := idx || 'col'; END IF;
		IF line ~ 'sc_b_' THEN idx := idx || 'btree'; END IF;
		IF line ~ 'Seq Scan' THEN idx := idx || 'seq'; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*), md5(string_agg(r, ''|'' ORDER BY r)) FROM (SELECT row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO n, md5;
END $$;

-- the four ways agree; col_idx: the column index was read in the col mode (a bitmap scan); tup_idx: in the tup mode (an index scan or an index-only scan);
-- btree_idx: a btree was read in the btree mode
CREATE FUNCTION sc_check(q text, OUT rows bigint, OUT agree bool, OUT col_idx bool, OUT tup_idx bool, OUT btree_idx bool) LANGUAGE plpgsql AS $$
DECLARE c record; t record; h record; b record;
BEGIN
	SELECT * INTO c FROM sc_run(q, 'col');
	SELECT * INTO t FROM sc_run(q, 'tup');
	SELECT * INTO h FROM sc_run(q, 'heap');
	SELECT * INTO b FROM sc_run(q, 'btree');
	rows := h.n;
	agree := c.n = h.n AND t.n = h.n AND h.n = b.n AND c.md5 IS NOT DISTINCT FROM h.md5 AND t.md5 IS NOT DISTINCT FROM h.md5 AND h.md5 IS NOT DISTINCT FROM b.md5;
	col_idx := c.idx ~ 'col';
	tup_idx := t.idx ~ 'col';
	btree_idx := b.idx ~ 'btree';
END $$;

-- does the plan of a query read the column index (the EXPLAIN text differs between versions: a disabled node is only named from 18 on)
CREATE FUNCTION sc_uses_col(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'sc_col' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE sc_q (n serial, q text);
INSERT INTO sc_q (q) VALUES
 ('SELECT * FROM %T WHERE id = 5'),
 ('SELECT * FROM %T WHERE id = 2147483647'),
 ('SELECT * FROM %T WHERE id = -2147483648'),
 ('SELECT * FROM %T WHERE id = 99999999'),
 ('SELECT * FROM %T WHERE id IN (1, 7, 7, 29999, 40000, NULL)'),
 ('SELECT * FROM %T WHERE id = ANY (''{}'')'),
 ('SELECT * FROM %T WHERE id = ANY (ARRAY(SELECT i * 37 FROM generate_series(1, 150) i))'),
 ('SELECT * FROM %T WHERE id BETWEEN 100 AND 180'),
 ('SELECT * FROM %T WHERE id < 40'),
 ('SELECT * FROM %T WHERE id <= 40'),
 ('SELECT * FROM %T WHERE id > 29980'),
 ('SELECT * FROM %T WHERE id >= 29980'),
 ('SELECT * FROM %T WHERE id > 100 AND id < 50'),
 ('SELECT * FROM %T WHERE id = 5::int8'),
 ('SELECT * FROM %T WHERE id < 5::int2'),
 ('SELECT * FROM %T WHERE k8 = 5000000'),
 ('SELECT * FROM %T WHERE k8 > 3000000000 AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE k8 < -999999999'),
 ('SELECT * FROM %T WHERE k2 = 7::int4'),
 ('SELECT * FROM %T WHERE k2 BETWEEN -3 AND 3'),
 ('SELECT * FROM %T WHERE k2 > 32766::int8'),
 ('SELECT * FROM %T WHERE v IS NULL'),
 ('SELECT * FROM %T WHERE v IS NOT NULL AND g = 3'),
 ('SELECT * FROM %T WHERE v = 2147483647'),
 ('SELECT * FROM %T WHERE d = ''2024-03-01'''),
 ('SELECT * FROM %T WHERE d >= ''2024-02-01'' AND d < ''2024-02-10'''),
 ('SELECT * FROM %T WHERE d = ''infinity'''),
 ('SELECT * FROM %T WHERE d IS NULL'),
 ('SELECT * FROM %T WHERE ts >= ''2024-01-02'' AND ts < ''2024-01-02 02:00'''),
 ('SELECT * FROM %T WHERE ts = ''infinity'''),
 ('SELECT * FROM %T WHERE tz BETWEEN ''2024-01-10 00:00+00'' AND ''2024-01-10 06:00+00'''),
 ('SELECT * FROM %T WHERE tz < ''2024-01-01 00:30+00'''),
 ('SELECT * FROM %T WHERE s = ''k17'''),
 ('SELECT * FROM %T WHERE s = '''''),
 ('SELECT * FROM %T WHERE s = ''caf'' || chr(233)'),
 ('SELECT * FROM %T WHERE s IN (''k1'', ''k2'', ''nope'', ''caf'' || chr(233))'),
 ('SELECT * FROM %T WHERE s IS NULL'),
 ('SELECT * FROM %T WHERE s = repeat(''long'', 100) || 0'),
 ('SELECT * FROM %T WHERE u = md5(''17'')::uuid'),
 ('SELECT * FROM %T WHERE u = ANY (ARRAY[md5(''1'')::uuid, md5(''2'')::uuid, md5(''x'')::uuid])'),
 ('SELECT * FROM %T WHERE g = 7 AND v BETWEEN 100 AND 200'),
 ('SELECT * FROM %T WHERE g IN (1, 2) AND s = ''k5'''),
 ('SELECT * FROM %T WHERE g = 7 AND ts >= ''2024-01-20'' AND ts < ''2024-01-25'' AND k2 < 0'),
 ('SELECT * FROM %T WHERE g = 1 OR v = 5'),
 ('SELECT * FROM %T WHERE (g = 1 AND v < 10) OR id IN (3, 4, 5) OR s = ''k9'''),
 ('SELECT * FROM %T WHERE g = 3 AND pad <> ''x'''),
 ('SELECT * FROM %T WHERE g = 3 AND pad = repeat(''x'', 500)'),
 ('SELECT * FROM %T WHERE id > ALL (ARRAY[29990, 29995])'),
 ('SELECT * FROM %T WHERE id < ANY (ARRAY[5, 10])'),
 ('SELECT * FROM %T WHERE NOT (id <> 5)'),
 ('SELECT g, count(*) FROM %T WHERE v < 20 GROUP BY g'),
 ('SELECT count(*), min(id), max(id) FROM %T WHERE g = 4 AND v > 400'),
 ('SELECT id FROM %T WHERE id % 1000 = 0 AND g = 0'),
 ('SELECT * FROM %T WHERE f8 = 12.125'),
 ('SELECT * FROM %T WHERE f8 = 0'),
 ('SELECT * FROM %T WHERE f8 > 120'),
 ('SELECT * FROM %T WHERE f8 BETWEEN -5 AND 5'),
 ('SELECT * FROM %T WHERE f8 = ''NaN'''),
 ('SELECT * FROM %T WHERE f8 > ''NaN'''),
 ('SELECT * FROM %T WHERE f8 < ''NaN'' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE f8 = ''Infinity'''),
 ('SELECT * FROM %T WHERE f8 >= 1e300'),
 ('SELECT * FROM %T WHERE f8 <= -1e300'),
 ('SELECT * FROM %T WHERE f8 IN (1.5, ''NaN'', -3)'),
 ('SELECT * FROM %T WHERE f8 IN (1.5, 2.5, -3)'),
 ('SELECT * FROM %T WHERE f8 < ANY (ARRAY[-120, -100.5])'),
 ('SELECT * FROM %T WHERE f8 IS NULL'),
 ('SELECT * FROM %T WHERE f8 = 12.125::float4'),
 ('SELECT * FROM %T WHERE f4 = 12.25'),
 ('SELECT * FROM %T WHERE f4 > 120'),
 ('SELECT * FROM %T WHERE f4 < 3.5::float8'),
 ('SELECT * FROM %T WHERE f8 > 3::float4 AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE f4 = ''NaN''::float4'),
 ('SELECT * FROM %T WHERE f4 IS NOT NULL AND g = 3'),
 ('SELECT * FROM %T WHERE b AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE NOT b AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE b = false AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE b IS NULL'),
 ('SELECT * FROM %T WHERE b IS TRUE AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE b IS NOT TRUE AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE b AND g = 3'),
 ('SELECT * FROM %T WHERE nm = 123.45'),
 ('SELECT * FROM %T WHERE nm = 123.456'),
 ('SELECT * FROM %T WHERE nm > 999.995'),
 ('SELECT * FROM %T WHERE nm >= 999.995'),
 ('SELECT * FROM %T WHERE nm < 0.005'),
 ('SELECT * FROM %T WHERE nm <= 0.005'),
 ('SELECT * FROM %T WHERE nm BETWEEN 10 AND 11'),
 ('SELECT * FROM %T WHERE nm IN (1.5, 2.25, 7.123, 500)'),
 ('SELECT * FROM %T WHERE nm = ''NaN'''),
 ('SELECT * FROM %T WHERE nm > ''NaN'''),
 ('SELECT * FROM %T WHERE nm < ''NaN'' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE nm > 1e30'),
 ('SELECT * FROM %T WHERE nm < -1e30'),
 ('SELECT * FROM %T WHERE nm = 9999999999.99'),
 ('SELECT * FROM %T WHERE nm IS NULL'),
 ('SELECT * FROM %T WHERE nm > 5.5::float8 AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE c = ''c17'''),
 ('SELECT * FROM %T WHERE c = ''c17   '''),
 ('SELECT * FROM %T WHERE c = ''c17''::char(3)'),
 ('SELECT * FROM %T WHERE c = ''caf'' || chr(233)'),
 ('SELECT * FROM %T WHERE c = ''ab'''),
 ('SELECT * FROM %T WHERE c = ''toolongvalue'''),
 ('SELECT * FROM %T WHERE c IN (''c1'', ''c2'', ''nope'')'),
 ('SELECT * FROM %T WHERE c IS NULL'),
 ('SELECT * FROM %T WHERE vc = ''v17'''),
 ('SELECT * FROM %T WHERE vc IN (''v1'', ''v2'')'),
 ('SELECT * FROM %T WHERE vc = ''v1''::text'),
 ('SELECT * FROM %T WHERE vc IS NOT NULL AND g = 2'),
 ('SELECT * FROM %T WHERE f8 > 100 AND b AND nm < 50'),
 ('SELECT * FROM %T WHERE c = ''c5'' AND g = 3 AND vc <> ''v1'''),
 ('SELECT * FROM %T WHERE (f4 < 5 OR nm > 990) AND b IS NOT FALSE'),
 ('SELECT * FROM %T WHERE s LIKE ''k1%'' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE s LIKE ''k%'' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE s LIKE ''k_7'''),
 ('SELECT * FROM %T WHERE s LIKE ''%7'''),
 ('SELECT * FROM %T WHERE s LIKE ''caf%'''),
 ('SELECT * FROM %T WHERE s LIKE ''long%'''),
 ('SELECT * FROM %T WHERE s LIKE ''k1_'''),
 ('SELECT * FROM %T WHERE s LIKE ''k17'''),
 ('SELECT * FROM %T WHERE s LIKE ''k\\%'''),
 ('SELECT * FROM %T WHERE s LIKE ''k1\\'''),
 ('SELECT * FROM %T WHERE s LIKE '''''),
 ('SELECT * FROM %T WHERE s ^@ ''k1'' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE s ^@ '''' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE starts_with(s, ''k2'') AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE s LIKE ANY (ARRAY[''k1%'', ''k2%'']) AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE s LIKE ''k1%'' AND g = 3'),
 ('SELECT * FROM %T WHERE vc LIKE ''v1%'' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE vc LIKE ''v_5'''),
 ('SELECT * FROM %T WHERE s NOT LIKE ''k1%'' AND id % 13 = 0'),
 ('SELECT * FROM %T WHERE s ILIKE ''K1%'' AND id % 13 = 0'),
 ('SELECT id, v FROM %T WHERE id BETWEEN 100 AND 140'),
 ('SELECT id, g, s FROM %T WHERE g = 3 AND v < 5'),
 ('SELECT v, k2, k8, d, ts, tz FROM %T WHERE id < 200'),
 ('SELECT s, u, vc, c FROM %T WHERE g = 7 AND v BETWEEN 10 AND 20'),
 ('SELECT f4, f8, b, nm FROM %T WHERE id < 300'),
 ('SELECT id, nm, f8 FROM %T WHERE nm > 990 OR f8 > 120'),
 ('SELECT count(*), sum(id) FROM %T WHERE id < 5000'),
 ('SELECT id FROM %T WHERE v IS NULL'),
 ('SELECT id, s FROM %T WHERE s IN (''k1'', ''k2'', ''caf'' || chr(233))'),
 ('SELECT id, c, vc FROM %T WHERE c = ''c17''');

CREATE FUNCTION sc_all(OUT n int, OUT rows bigint, OUT agree bool, OUT col_idx bool, OUT tup_idx bool, OUT btree_idx bool) RETURNS SETOF record LANGUAGE sql AS $$
	SELECT sc_q.n, c.rows, c.agree, c.col_idx, c.tup_idx, c.btree_idx FROM sc_q, LATERAL sc_check(sc_q.q) c ORDER BY sc_q.n $$;

-- the queries the planner reads the column index for, and the ones that disagree (none)
\echo -- after the build
SELECT count(*) AS queries, count(*) FILTER (WHERE agree) AS agree, count(*) FILTER (WHERE col_idx) AS on_col, count(*) FILTER (WHERE tup_idx) AS on_tup, count(*) FILTER (WHERE btree_idx) AS on_btree FROM sc_all();
SELECT n, rows, agree, col_idx, tup_idx, btree_idx FROM sc_all();

-- a join that reads the column index once for each outer row
CREATE TABLE sc_few (id int);
INSERT INTO sc_few SELECT i * 301 FROM generate_series(1, 20) i;
ANALYZE sc_few;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SELECT * FROM sc_check('SELECT %T.id, %T.g FROM sc_few f JOIN %T ON %T.id = f.id');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- a prepared statement with runtime keys (generic plan)
PREPARE sc_p (int, text) AS SELECT * FROM sc WHERE id = $1 OR s = $2;
SET plan_cache_mode = force_generic_plan;
SET sift.enable_agg = off;
SET sift.enable_col_scan = on;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT sc_uses_col('EXECUTE sc_p(5, ''k17'')') AS generic_plan_reads_the_column_index;
SELECT count(*) FROM (SELECT * FROM sc WHERE id = 5 OR s = 'k17' EXCEPT ALL SELECT * FROM sc_b WHERE id = 5 OR s = 'k17') x;
SELECT count(*) AS rows FROM (SELECT * FROM sc WHERE id = 5 OR s = 'k17') x;
RESET plan_cache_mode;
RESET enable_seqscan;
RESET enable_indexscan;
RESET sift.enable_col_scan;
RESET sift.enable_agg;

-- off by default: the planner builds no bitmap path on the column index, even when the sequential scan is disabled
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT sc_uses_col('SELECT * FROM sc WHERE v = 5 AND g = 2') AS scan_off;
SET sift.enable_col_scan = on;
SELECT sc_uses_col('SELECT * FROM sc WHERE v = 5 AND g = 2') AS scan_on;
RESET sift.enable_col_scan;
RESET enable_seqscan;
RESET enable_indexscan;

-- the write buffer: new rows, new versions and deleted rows are read before the buffer is promoted
SELECT sc_dml($$INSERT INTO %T (id, v, k2, k8, d, ts, tz, s, u, g)
SELECT 100000 + i, i % 500, (i % 200 - 100)::int2, i::int8 * 5000000, date '2024-02-01' + i % 50, timestamp '2024-02-01' + i * interval '97 seconds',
	   timestamptz '2024-02-01+00' + i * interval '1 minute', 'k' || i % 300, md5(i::text)::uuid, i % 20
FROM generate_series(1, 3000) i$$);
SELECT sc_dml($$UPDATE %T SET v = v + 1, s = 'k' || (id % 300 + 1), g = (g + 1) % 20 WHERE id % 53 = 0$$);
SELECT sc_dml($$UPDATE %T SET pad = repeat('y', 500) WHERE id % 59 = 0$$);
SELECT sc_dml($$DELETE FROM %T WHERE id % 61 = 0$$);
SELECT sc_dml($$INSERT INTO %T (id, f4, f8, b, nm, c, vc) SELECT 110000 + i, i % 100, i / 8.0, i % 3 = 0, i / 100.0, 'c' || i % 200, 'v' || i % 150 FROM generate_series(1, 2000) i$$);
SELECT sc_dml($$UPDATE %T SET f8 = f8 + 1, nm = nm + 0.01, c = 'c9', b = NOT b, vc = 'v' || (id % 150 + 1) WHERE id % 47 = 3$$);
\echo -- write buffer
SELECT buffers > 0 AS rows_in_the_buffer FROM sift.col_index_info('sc_col'::regclass);
SELECT count(*) AS queries, count(*) FILTER (WHERE agree) AS agree, count(*) FILTER (WHERE col_idx) AS on_col, count(*) FILTER (WHERE tup_idx) AS on_tup FROM sc_all();
SELECT n, rows, agree FROM sc_all() WHERE NOT agree;

-- an open transaction reads its own rows, a rollback leaves none
BEGIN;
SELECT sc_dml($$INSERT INTO %T (id, v, g, s) SELECT 200000 + i, 7, 7, 'k7' FROM generate_series(1, 50) i$$);
SELECT sc_dml($$UPDATE %T SET v = 7, g = 7 WHERE id BETWEEN 1000 AND 1100$$);
\echo -- own rows
SELECT n, rows, agree, col_idx, tup_idx FROM sc_all() WHERE n IN (1, 22, 23, 41, 43);
ROLLBACK;
\echo -- after the rollback
SELECT n, rows, agree, col_idx, tup_idx FROM sc_all() WHERE n IN (1, 22, 23, 41, 43);

VACUUM sc;
VACUUM sc_b;
\echo -- vacuumed, buffer promoted
SELECT count(*) AS queries, count(*) FILTER (WHERE agree) AS agree, count(*) FILTER (WHERE col_idx) AS on_col, count(*) FILTER (WHERE tup_idx) AS on_tup FROM sc_all();

SELECT sc_dml($$UPDATE %T SET v = (v * 3) % 500 WHERE id % 7 = 0$$);
SELECT sc_dml($$DELETE FROM %T WHERE id % 71 = 0$$);
SELECT sc_dml($$INSERT INTO %T (id, v, g, s) SELECT 300000 + i, i % 500, i % 20, 'k' || i % 300 FROM generate_series(1, 500) i$$);
VACUUM sc;
VACUUM sc_b;
SELECT sift.col_merge('sc_col'::regclass) >= 0 AS merged;
\echo -- merged
SELECT count(*) AS queries, count(*) FILTER (WHERE agree) AS agree, count(*) FILTER (WHERE col_idx) AS on_col, count(*) FILTER (WHERE tup_idx) AS on_tup FROM sc_all();

-- an update that leaves the indexed columns alone (HOT where the page has room), and one that changes them in the same page
SELECT sc_dml($$UPDATE %T SET pad = repeat('z', 500) WHERE id BETWEEN 2000 AND 2100$$);
SELECT sc_dml($$UPDATE %T SET v = 499 WHERE id BETWEEN 2000 AND 2020$$);
SELECT n, rows, agree, col_idx, tup_idx FROM sc_all() WHERE n IN (8, 9, 41, 42);

-- the bitmap scan compares bytes: a nondeterministic collation has no such index
CREATE COLLATION sc_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE INDEX sc_col_ci ON sc USING sift_col (s COLLATE sc_ci sift.col_text_ops);
CREATE INDEX sc_col_ci ON sc USING sift_col (s COLLATE sc_ci);
DROP INDEX sc_col_ci;
-- a char without a length and a numeric the leaf holds in another form are not scanned
CREATE TABLE sc_bad (w bpchar, y numeric, z numeric(30, 5));
CREATE INDEX ON sc_bad USING sift_col (w sift.col_bpchar_ops);
CREATE INDEX ON sc_bad USING sift_col (y sift.col_numeric_ops);
CREATE INDEX ON sc_bad USING sift_col (z sift.col_numeric_ops);
DROP TABLE sc_bad;

-- the scans are counted
SELECT pg_stat_force_next_flush();
SELECT idx_scan > 0 AS scans_counted, idx_tup_read > 0 AS tuples_counted FROM pg_stat_user_indexes WHERE indexrelname = 'sc_col';

-- the index is the one of sift.col_ops: the aggregate scans still read it, and it verifies
SELECT count(*) FROM sc WHERE id < 100;
SELECT sift.col_verify('sc_col'::regclass, true) IS NOT NULL AS verified;

DROP COLLATION sc_ci;
DROP TABLE sc, sc_b, sc_q, sc_few;
DROP FUNCTION sc_dml(text), sc_mode(text), sc_run(text, text), sc_check(text), sc_uses_col(text), sc_all();
DROP EXTENSION pg_sift;
