-- The sift_col index: DDL and refusals, build equivalence with the heap for every type and operator, the SIMD levels
-- and the visibility rule. Differential: the column source API (sift._test_col_*) against plain SQL on the same table.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- The rows of an index scan (root TIDs) against the visible heap tuples; HOT chains are avoided by changing indexed columns
CREATE FUNCTION col_cmp(idx regclass, preds text, heapwhere text, flags int DEFAULT 0) RETURNS text LANGUAGE plpgsql AS $$
DECLARE tbl regclass; a bigint; b bigint; ex bigint; mi bigint;
BEGIN
	SELECT indrelid::regclass INTO tbl FROM pg_index WHERE indexrelid = idx;
	SELECT count(*) INTO a FROM sift._test_col_tids(idx, preds, flags);
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', tbl, heapwhere) INTO b;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L, %s) EXCEPT SELECT ctid FROM %s WHERE %s) z',
				   idx, preds, flags, tbl, heapwhere) INTO ex;
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM %s WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L, %s)) z',
				   tbl, heapwhere, idx, preds, flags) INTO mi;
	RETURN CASE WHEN a = b AND ex = 0 AND mi = 0 THEN 'ok' ELSE format('MISMATCH index=%s heap=%s extra=%s missing=%s', a, b, ex, mi) END;
END $$;

-- ---------------------------------------------------------------------------------------------------------
-- DDL
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE orders (
	id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	tenant_id int, qty bigint, status text, amount float8, discount float4, placed date, paid_at timestamptz, express bool,
	total numeric, payload jsonb, note text);

CREATE INDEX o_numeric ON orders USING sift_col (total);
CREATE INDEX o_jsonb ON orders USING sift_col (payload);
CREATE INDEX o_include ON orders USING sift_col (tenant_id) INCLUDE (qty);
CREATE UNIQUE INDEX o_unique ON orders USING sift_col (tenant_id);
CREATE INDEX o_paths ON orders USING sift_col (tenant_id) WITH (paths = 'tenant_id int4');
CREATE INDEX o_paths2 ON orders USING sift_col (payload) WITH (paths = 'other.x int4');
CREATE INDEX o_paths3 ON orders USING sift_col (payload) WITH (paths = 'payload.a[].b[].c int4');
CREATE INDEX o_limit ON orders USING sift_col (tenant_id) WITH (mutable_limit = 1);
CREATE INDEX o_empty ON orders USING sift_col (tenant_id, status, amount, discount, placed, paid_at, express, qty);
SELECT relation_blocks, high_water, free_blocks, segments, max_segments, rows, bytes, units, mutable_segments, leaves, maintenance_busy
FROM sift.col_index_info('o_empty') \gx
SELECT count(*) FROM sift._test_col_tids('o_empty', '');
SELECT sift.col_verify('o_empty', true);
SELECT sift.col_index_info('orders_pkey');
DROP INDEX o_empty;

-- ---------------------------------------------------------------------------------------------------------
-- Build equivalence
-- ---------------------------------------------------------------------------------------------------------
INSERT INTO orders (tenant_id, qty, status, amount, discount, placed, paid_at, express, note)
SELECT CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 1000 - 500 END,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE (i::bigint * 104729) % 100000 - 50000 END,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE (ARRAY['new', 'paid', 'shipped', '', 'retourné', 'x' || (i % 50)])[1 + i % 6] END,
	   CASE WHEN i % 19 = 0 THEN NULL WHEN i % 23 = 0 THEN 'NaN'::float8 WHEN i % 29 = 0 THEN 'Infinity'
			WHEN i % 31 = 0 THEN '-Infinity' WHEN i % 37 = 0 THEN -0.0 ELSE (i % 1000) / 8.0 - 60 END,
	   CASE WHEN i % 19 = 0 THEN NULL ELSE ((i % 500) / 4.0)::float4 END,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE date '2020-01-01' + i % 900 END,
	   CASE WHEN i % 43 = 0 THEN NULL ELSE timestamptz '2021-03-04 05:06:07+00' + (i || ' minutes')::interval END,
	   CASE WHEN i % 7 = 0 THEN NULL ELSE i % 2 = 0 END,
	   repeat('n', i % 300)
FROM generate_series(1, 20000) i;
ANALYZE orders;

-- 255 bytes are the limit of a text value
CREATE INDEX o_note ON orders USING sift_col (note);
DELETE FROM orders WHERE length(note) > 255;
CREATE INDEX o_note ON orders USING sift_col (note);
SELECT sift.col_verify('o_note', true);
DROP INDEX o_note;

CREATE INDEX o_col ON orders USING sift_col (tenant_id, status, amount, discount, placed, paid_at, express, qty);
SELECT leaves, segments, mutable_segments, rows = (SELECT count(*) FROM orders) AS rows_match FROM sift.col_index_info('o_col');
SELECT sift.col_verify('o_col', true);

CREATE TABLE col_checks (n serial, preds text, heapwhere text);
INSERT INTO col_checks (preds, heapwhere) VALUES
	('', 'true'),
	('0:eq:7', 'tenant_id = 7'),
	('0:ne:7', 'tenant_id <> 7'),
	('0:lt:-100;7:ge:100', 'tenant_id < -100 AND qty >= 100'),
	('0:in:1|2|3|4|-5;6:eq:0', 'tenant_id IN (1, 2, 3, 4, -5) AND NOT express'),
	('0:notin:1|2|3', 'tenant_id NOT IN (1, 2, 3)'),
	('0:null', 'tenant_id IS NULL'),
	('0:notnull', 'tenant_id IS NOT NULL'),
	('1:eq:paid', 'status = ''paid'''),
	('1:eq:', 'status = '''''),
	('1:eq:retourné', 'status = ''retourné'''),
	('1:eq:nothere', 'status = ''nothere'''),
	('1:ne:paid', 'status <> ''paid'''),
	('1:in:new|paid|zzz', 'status IN (''new'', ''paid'', ''zzz'')'),
	('1:notin:new|paid', 'status NOT IN (''new'', ''paid'')'),
	('1:prefix:x', 'status LIKE ''x%'' COLLATE "C"'),
	('1:prefix:x4', 'status LIKE ''x4%'' COLLATE "C"'),
	('1:ge:paid;1:lt:shipped', 'status >= ''paid'' COLLATE "C" AND status < ''shipped'' COLLATE "C"'),
	('1:null', 'status IS NULL'),
	('1:eq:new;0:eq:7', 'status = ''new'' AND tenant_id = 7'),
	('2:gt:10', 'amount > 10'),
	('2:eq:0', 'amount = 0'),
	('2:ge:1e308', 'amount >= 1e308'),
	('2:eq:nan', 'amount = ''NaN'''),
	('2:ge:inf', 'amount >= ''Infinity'''),
	('2:le:-inf', 'amount <= ''-Infinity'''),
	('2:lt:-1e308', 'amount < -1e308'),
	('2:null', 'amount IS NULL'),
	('2:in:0|12.5|nan', 'amount IN (0, 12.5, ''NaN'')'),
	('3:le:20.5', 'discount <= 20.5'),
	('3:gt:20.3', 'discount > 20.3::float8'),
	('3:eq:0.25', 'discount = 0.25'),
	('4:ge:18500', 'placed >= date ''2000-01-01'' + 18500'),
	('4:lt:18500;4:gt:18000', 'placed < date ''2000-01-01'' + 18500 AND placed > date ''2000-01-01'' + 18000'),
	('5:ge:1000000000000', 'paid_at >= timestamptz ''2000-01-01 00:00:00+00'' + interval ''1000000 seconds'''),
	('5:null', 'paid_at IS NULL'),
	('6:eq:1', 'express'),
	('6:eq:0', 'NOT express'),
	('6:null', 'express IS NULL'),
	('7:lt:0', 'qty < 0'),
	('7:in:-50000|0|49999', 'qty IN (-50000, 0, 49999)'),
	('7:ge:9223372036854775807', 'qty >= 9223372036854775807'),
	('0:eq:7;1:eq:paid;2:gt:0;3:lt:100;6:eq:1;7:gt:0', 'tenant_id = 7 AND status = ''paid'' AND amount > 0 AND discount < 100 AND express AND qty > 0');
SELECT n, preds, col_cmp('o_col', preds, heapwhere) FROM col_checks ORDER BY n;

-- Without the mutable segments the result is the same (nothing is there yet), with the visibility check skipped it can only grow
SELECT bool_and(col_cmp('o_col', preds, heapwhere, 1) = 'ok') AS no_mutable_same FROM col_checks;

-- compression policies: other policies give other bytes and the same answers
CREATE INDEX o_bad ON orders USING sift_col (tenant_id) WITH (compression = 'bogus');
CREATE INDEX o_none ON orders USING sift_col (tenant_id, status, amount, discount, placed, paid_at, express, qty) WITH (compression = 'none');
CREATE INDEX o_mixed ON orders USING sift_col (tenant_id, status, amount, discount, placed, paid_at, express, qty)
	WITH (compression = 'zstd:3; status=lz4; amount=for+zstd; express=rle');
SELECT sift.col_verify('o_none', true), sift.col_verify('o_mixed', true);
SELECT i AS index, bool_and(col_cmp(i, preds, heapwhere) = 'ok') AS same_as_heap
FROM (VALUES ('o_none'::regclass), ('o_mixed'::regclass)) v(i), col_checks GROUP BY i ORDER BY i::text;
SELECT (SELECT sum(chunk_bytes) FROM sift.col_leaves('o_none')) > (SELECT sum(chunk_bytes) FROM sift.col_leaves('o_col')) AS auto_smaller_than_none,
	(SELECT sum(codec_chunks) FROM sift.col_leaves('o_col')) > 0 AS codec_used;
DROP INDEX o_none, o_mixed;
SELECT bool_and((SELECT count(*) FROM sift._test_col_tids('o_col', preds, 2)) >= (SELECT count(*) FROM sift._test_col_tids('o_col', preds))) AS novet_superset FROM col_checks;

-- ---------------------------------------------------------------------------------------------------------
-- Values read back
-- ---------------------------------------------------------------------------------------------------------
SELECT count(*) AS differing FROM (
	SELECT r.tid, r.val FROM sift._test_col_read('o_col', 0, '') r
	EXCEPT SELECT ctid, tenant_id::text FROM orders) z;
SELECT count(*) AS differing FROM (
	SELECT ctid, tenant_id::text FROM orders
	EXCEPT SELECT r.tid, r.val FROM sift._test_col_read('o_col', 0, '') r) z;
SELECT count(*) AS differing FROM (SELECT tid, val FROM sift._test_col_read('o_col', 1, '') EXCEPT SELECT ctid, status FROM orders) z;
SELECT count(*) AS differing FROM (SELECT tid, val FROM sift._test_col_read('o_col', 2, '') EXCEPT SELECT ctid, amount::text FROM orders) z;
SELECT count(*) AS differing FROM (SELECT tid, val FROM sift._test_col_read('o_col', 3, '') EXCEPT SELECT ctid, discount::float8::text FROM orders) z;
SELECT count(*) AS differing FROM (SELECT tid, val FROM sift._test_col_read('o_col', 4, '') EXCEPT SELECT ctid, (placed - date '2000-01-01')::text FROM orders) z;
SELECT count(*) AS differing FROM (SELECT tid, val FROM sift._test_col_read('o_col', 6, '') EXCEPT SELECT ctid, express::int::text FROM orders) z;
SELECT count(*) AS differing FROM (SELECT tid, val FROM sift._test_col_read('o_col', 7, '') EXCEPT SELECT ctid, qty::text FROM orders) z;

-- Zones answer count/min/max/sum of clean all-visible units without reading a chunk
VACUUM orders;
SELECT sum(nvals - nnull) = (SELECT count(tenant_id) FROM orders) AS zone_count, min(min) = (SELECT min(tenant_id) FROM orders) AS zone_min,
	   max(max) = (SELECT max(tenant_id) FROM orders) AS zone_max, sum(sum_i) = (SELECT sum(tenant_id) FROM orders) AS zone_sum,
	   bool_and(exact) AS exact
FROM sift._test_col_zones('o_col', 0);

-- ---------------------------------------------------------------------------------------------------------
-- Visibility: all-visible pages are read from the index, the others are checked against the heap
-- ---------------------------------------------------------------------------------------------------------
SELECT rows_vetted, heap_pages FROM sift._test_col_counters('o_col', '0:eq:7');
UPDATE orders SET tenant_id = coalesce(tenant_id, 0) + 1000 WHERE id % 50 = 0;
SELECT rows_vetted > 0 AS vetted, heap_pages > 0 AS heap_read FROM sift._test_col_counters('o_col', '0:ge:0');
SELECT col_cmp('o_col', '0:ge:700', 'tenant_id >= 700');
SELECT col_cmp('o_col', '', 'true');
VACUUM orders;
SELECT rows_vetted, heap_pages FROM sift._test_col_counters('o_col', '0:ge:0');
SELECT col_cmp('o_col', '0:ge:700', 'tenant_id >= 700');

-- ---------------------------------------------------------------------------------------------------------
-- SIMD levels: the same answers (scalar is the specification)
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE col_digest AS SELECT 'default' AS level, md5(string_agg(tid::text, ',' ORDER BY tid)) AS d
FROM (SELECT tid FROM col_checks, LATERAL sift._test_col_tids('o_col', preds)) s;
SET sift.simd = scalar;
SELECT (sift.col_index_info('o_col')).simd AS simd;
SELECT md5(string_agg(tid::text, ',' ORDER BY tid)) = (SELECT d FROM col_digest) AS scalar_same
FROM (SELECT tid FROM col_checks, LATERAL sift._test_col_tids('o_col', preds)) s;
SET sift.simd = avx2;
SELECT md5(string_agg(tid::text, ',' ORDER BY tid)) = (SELECT d FROM col_digest) AS avx2_same
FROM (SELECT tid FROM col_checks, LATERAL sift._test_col_tids('o_col', preds)) s;
RESET sift.simd;

-- ---------------------------------------------------------------------------------------------------------
-- Privileges and row-level security
-- ---------------------------------------------------------------------------------------------------------
CREATE ROLE col_reader;
GRANT USAGE ON SCHEMA sift TO col_reader;
GRANT EXECUTE ON FUNCTION sift._test_col_counters(regclass, text, int) TO col_reader;
SET ROLE col_reader;
SELECT rows FROM sift._test_col_counters('o_col', '');
RESET ROLE;
GRANT SELECT ON orders TO col_reader;
SET ROLE col_reader;
SELECT rows > 0 AS readable FROM sift._test_col_counters('o_col', '');
RESET ROLE;
ALTER TABLE orders ENABLE ROW LEVEL SECURITY;
CREATE POLICY mine ON orders FOR SELECT USING (tenant_id > 0);
SET ROLE col_reader;
SELECT rows FROM sift._test_col_counters('o_col', '');
RESET ROLE;
SELECT rows > 0 AS owner_bypasses FROM sift._test_col_counters('o_col', '');
ALTER TABLE orders DISABLE ROW LEVEL SECURITY;
REVOKE ALL ON orders FROM col_reader;
DROP OWNED BY col_reader;
DROP ROLE col_reader;

-- ---------------------------------------------------------------------------------------------------------
-- SERIALIZABLE takes the relation predicate lock
-- ---------------------------------------------------------------------------------------------------------
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT count(*) AS siread_before FROM pg_locks WHERE mode = 'SIReadLock' AND relation = 'orders'::regclass;
SELECT count(*) > 0 AS scanned FROM sift._test_col_tids('o_col', '0:eq:7');
SELECT count(*) AS siread_after FROM pg_locks WHERE mode = 'SIReadLock' AND relation = 'orders'::regclass;
COMMIT;

-- ---------------------------------------------------------------------------------------------------------
-- Partial and expression indexes
-- ---------------------------------------------------------------------------------------------------------
CREATE INDEX o_partial ON orders USING sift_col (tenant_id, status) WHERE express;
SELECT sift.col_verify('o_partial', true);
SELECT col_cmp('o_partial', '1:eq:paid', 'express AND status = ''paid''');
CREATE INDEX o_expr ON orders USING sift_col ((tenant_id % 10), (amount * 2), (status || '!'));
SELECT sift.col_verify('o_expr', true);
SELECT col_cmp('o_expr', '0:eq:3;1:gt:20', 'tenant_id % 10 = 3 AND amount * 2 > 20');
SELECT col_cmp('o_expr', '2:eq:paid!', 'status || ''!'' = ''paid!''');

-- ---------------------------------------------------------------------------------------------------------
-- numeric(p <= 18, s): the number times 10^s as an exact integer; NaN is a candidate of every predicate (the heap decides)
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE nums (id int, n numeric(12, 2), w numeric(18, 0));
INSERT INTO nums SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 19 = 0 THEN 'NaN' ELSE (i * 7919 % 200001 - 100000) / 100.0 END,
	999999999999999999 - i FROM generate_series(1, 3000) i;
CREATE INDEX nums_wide ON nums USING sift_col ((n::numeric(19, 2)));
CREATE INDEX nums_idx ON nums USING sift_col (n, w);
SELECT sift.col_verify('nums_idx', true);
SELECT col_cmp('nums_idx', '0:ge:-250;0:lt:12345;0:ne:0', '(n >= -2.50 AND n < 123.45 AND n <> 0) OR n = ''NaN''');
SELECT col_cmp('nums_idx', '0:eq:700', 'n = 7.00 OR n = ''NaN''');
SELECT col_cmp('nums_idx', '1:gt:999999999999998000', 'w > 999999999999998000');
SELECT col_cmp('nums_idx', '0:null', 'n IS NULL OR n = ''NaN''');
SELECT (SELECT sum(val::bigint) FROM sift._test_col_read('nums_idx', 0, '')) = (SELECT (sum(n) * 100)::bigint FROM nums WHERE n <> 'NaN') AS scaled_sum;
DROP TABLE nums;

DROP TABLE orders CASCADE;
DROP TABLE col_checks;
DROP TABLE col_digest;
DROP FUNCTION col_cmp(regclass, text, text, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
