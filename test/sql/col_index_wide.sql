-- Wide leaf types of the sift_col index: numeric(p <= 38, s) as two lanes, numeric of any other shape as an image and
-- its display scale, money and interval. Differential: the column source API (sift._test_col_*) against plain SQL on the same table, the
-- values read back against the heap's text, through the write buffer, promotion, merge, VACUUM and REINDEX.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET lc_monetary = 'C';

-- The rows an index scan keeps (root TIDs) against the visible heap tuples
CREATE FUNCTION col_cmp(idx regclass, preds text, heapwhere text, flags int DEFAULT 0) RETURNS text LANGUAGE plpgsql SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_agg = off AS $$
DECLARE tbl regclass; a bigint; b bigint; ex bigint; mi bigint;
BEGIN
	SELECT indrelid::regclass INTO tbl FROM pg_index WHERE indexrelid = idx;
	SELECT count(*) INTO a FROM sift._test_col_tids(idx, preds, flags);
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', tbl, heapwhere) INTO b;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L, %s) EXCEPT SELECT ctid FROM %s WHERE %s) z',
				   idx, preds, flags, tbl, heapwhere) INTO ex;
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM %s WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L, %s)) z',
				   tbl, heapwhere, idx, preds, flags) INTO mi;
	RETURN CASE WHEN a = b AND ex = 0 AND mi = 0 THEN format('ok %s', a) ELSE format('MISMATCH index=%s heap=%s extra=%s missing=%s', a, b, ex, mi) END;
END $$;

-- The leaf's values read back (leaf: a head, its lanes are read with it) against what the heap says: expr is a SQL expression of the row, a text
CREATE FUNCTION col_back(idx regclass, leaf int, expr text, flags int DEFAULT 0) RETURNS text LANGUAGE plpgsql
SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_agg = off AS $$
DECLARE tbl regclass; bad bigint; n bigint;
BEGIN
	SELECT indrelid::regclass INTO tbl FROM pg_index WHERE indexrelid = idx;
	EXECUTE format('SELECT count(*) FROM sift._test_col_read(%L, %s, '''', %s) r JOIN %s t ON t.ctid = r.tid WHERE r.val IS DISTINCT FROM (%s)',
				   idx, leaf, flags, tbl, expr) INTO bad;
	EXECUTE format('SELECT count(*) FROM sift._test_col_read(%L, %s, '''', %s)', idx, leaf, flags) INTO n;
	RETURN format('%s rows, %s differ', n, bad);
END $$;

-- ---------------------------------------------------------------------------------------------------------
-- DDL: the types that are accepted and the ones that are not
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE w (id int, n38 numeric(38, 10), n19 numeric(19, 2), n30 numeric(30, 0), nsc numeric(5, 30), n3 numeric(3, 1),
				img numeric, neg numeric(5, -2), huge numeric(40, 5), m money, iv interval, tm time, u uuid, b bytea);
CREATE INDEX w_n38 ON w USING sift_col (n38);
CREATE INDEX w_n19 ON w USING sift_col (n19);
CREATE INDEX w_n30 ON w USING sift_col (n30);
CREATE INDEX w_nsc ON w USING sift_col (nsc);
CREATE INDEX w_n3 ON w USING sift_col (n3);
CREATE INDEX w_img ON w USING sift_col (img);
CREATE INDEX w_neg ON w USING sift_col (neg);
CREATE INDEX w_huge ON w USING sift_col (huge);
CREATE INDEX w_m ON w USING sift_col (m);
CREATE INDEX w_iv ON w USING sift_col (iv);
CREATE INDEX w_expr ON w USING sift_col ((n3::numeric(25, 5)), (id::numeric));
CREATE INDEX w_time ON w USING sift_col (tm);
CREATE INDEX w_uuid ON w USING sift_col (u);
CREATE INDEX w_bytea ON w USING sift_col (b);
SELECT indexrelid::regclass AS idx, (SELECT count(*) FROM sift.col_leaves(indexrelid)) AS leaves
FROM pg_index WHERE indrelid = 'w'::regclass AND indexrelid::regclass::text <> 'w_expr' ORDER BY 1;
SELECT leaf, path, kind, type FROM sift.col_leaves('w_iv') ORDER BY leaf;
SELECT leaf, path, type FROM sift.col_leaves('w_n38') ORDER BY leaf;
SELECT leaf, path, type FROM sift.col_leaves('w_img') ORDER BY leaf;
DROP INDEX w_n3, w_n30, w_nsc;
DROP INDEX w_n38, w_n19, w_img, w_neg, w_huge, w_m, w_iv, w_expr;

-- ---------------------------------------------------------------------------------------------------------
-- Data: the traps of every type
-- ---------------------------------------------------------------------------------------------------------
INSERT INTO w (id, n38, n19, n30, img, m, iv, n3, neg, huge)
SELECT i,
	   CASE WHEN i % 13 = 0 THEN NULL
			WHEN i % 17 = 0 THEN 'NaN'
			WHEN i % 19 = 0 THEN 9999999999999999999999999999.9999999999
			WHEN i % 29 = 0 THEN -9999999999999999999999999999.9999999999
			WHEN i % 23 = 0 THEN 0
			WHEN i % 2 = 0 THEN round(((i * 7919) % 2000001 - 1000000) / 100.0, 10)
			ELSE round(mod(i::numeric * 1234567890123456789012345.6789012345, 9999999999999999999999999999) * CASE WHEN i % 3 = 0 THEN -1 ELSE 1 END, 10) END,
	   CASE WHEN i % 11 = 0 THEN NULL WHEN i % 17 = 0 THEN 'NaN' WHEN i % 31 = 0 THEN 99999999999999999.99
			WHEN i % 37 = 0 THEN -99999999999999999.99 ELSE ((i * 104729) % 2000001 - 1000000) / 100.0 END,
	   CASE WHEN i % 7 = 0 THEN NULL WHEN i % 41 = 0 THEN 'NaN' ELSE (i::numeric * 1000000007) * (i % 5 - 2) END,
	   CASE WHEN i % 10 = 0 THEN NULL
			WHEN i % 41 = 0 THEN 'NaN' WHEN i % 43 = 0 THEN 'Infinity' WHEN i % 47 = 0 THEN '-Infinity'
			WHEN i % 53 = 0 THEN 10::numeric ^ 400 + i WHEN i % 59 = 0 THEN -(10::numeric ^ 400)
			WHEN i % 61 = 0 THEN 1 / 10::numeric ^ 300 WHEN i % 5 = 0 THEN (ARRAY[1.0, 1.00, 1, 1.000, 1.5, 1.50])[1 + i % 6]
			ELSE round(((i * 7919) % 200001 - 100000) / 7.0, 1 + i % 5) END,
	   CASE WHEN i % 9 = 0 THEN NULL WHEN i % 101 = 0 THEN '92233720368547758.07'::money WHEN i % 103 = 0 THEN '-92233720368547758.08'::money
			ELSE (((i * 7919) % 200001 - 100000)::numeric / 100)::money END,
	   CASE WHEN i % 12 = 0 THEN NULL
			WHEN i % 71 = 0 THEN 'infinity' WHEN i % 73 = 0 THEN '-infinity'
			WHEN i % 79 = 0 THEN '2147483647 months 2147483647 days 9223372036854775807 us'::interval
			WHEN i % 83 = 0 THEN '-2147483648 months -2147483648 days -9223372036854775808 us'::interval
			ELSE (ARRAY['1 month', '30 days', '720 hours', '1 mon 1 day', '31 days', '1 day 1 hour', '25 hours', '-1 month', '-30 days',
						'0', '00:00:00.000001', '-00:00:00.000001', '1 year', '12 mons', '365 days', '2 mons', '59 days', '60 days'])[1 + i % 18]::interval
				 + make_interval(secs => (i % 7) * 0.5) END,
	   CASE WHEN i % 5 = 0 THEN NULL WHEN i % 29 = 0 THEN 'NaN' ELSE (i % 200 - 100) / 10.0 END,
	   CASE WHEN i % 6 = 0 THEN NULL ELSE ((i % 300) - 150) * 100 END,
	   CASE WHEN i % 8 = 0 THEN NULL WHEN i % 16 = 1 THEN 'NaN' ELSE (i::numeric * 10::numeric ^ 30) + i / 1000.0 END
FROM generate_series(1, 20000) i;
ANALYZE w;

CREATE INDEX w_n38 ON w USING sift_col (n38);
CREATE INDEX w_n19 ON w USING sift_col (n19);
CREATE INDEX w_img ON w USING sift_col (img);
CREATE INDEX w_m ON w USING sift_col (m);
CREATE INDEX w_iv ON w USING sift_col (iv);
CREATE INDEX w_n3 ON w USING sift_col (n3);
CREATE INDEX w_neg ON w USING sift_col (neg);
CREATE INDEX w_huge ON w USING sift_col (huge);
CREATE INDEX w_all ON w USING sift_col (id, n38, img, m, iv);
SELECT sift.col_verify(indexrelid, true) FROM pg_index WHERE indrelid = 'w'::regclass AND indexrelid::regclass::text IN ('w_n38', 'w_n19', 'w_img', 'w_m', 'w_iv', 'w_n3', 'w_neg', 'w_huge', 'w_all') ORDER BY indexrelid::regclass::text;
SELECT idx, string_agg(DISTINCT format::text, ',') AS format FROM (SELECT 'w_all' AS idx, format FROM sift._test_col_formats('w_all')
	UNION ALL SELECT 'w_iv', format FROM sift._test_col_formats('w_iv') UNION ALL SELECT 'w_m', format FROM sift._test_col_formats('w_m')) f GROUP BY idx ORDER BY idx;

-- ---------------------------------------------------------------------------------------------------------
-- The values come back exactly: a lane pair, an image and its display scale, an interval's month, day and time
-- ---------------------------------------------------------------------------------------------------------
SELECT col_back('w_n38', 0, 'CASE WHEN n38 = ''NaN'' THEN NULL ELSE n38::text END'), col_back('w_n19', 0, 'CASE WHEN n19 = ''NaN'' THEN NULL ELSE n19::text END'), col_back('w_huge', 0, 'huge::text');
SELECT col_back('w_neg', 0, 'neg::text');
SELECT col_back('w_img', 0, 'img::text'), col_back('w_iv', 0, 'iv::text'), col_back('w_m', 0, '(m::numeric * 100)::bigint::text');
SELECT col_back('w_all', 3, 'img::text'), col_back('w_all', 6, 'iv::text'), col_back('w_all', 1, 'CASE WHEN n38 = ''NaN'' THEN NULL ELSE n38::text END');
-- lanes are plain integers (the head of an interval is the high word of its total, lane 2 month << 32 | day)
SELECT count(*) AS lane_rows, count(DISTINCT val) > 5 AS varied FROM sift._test_col_read('w_iv', 2);

-- ---------------------------------------------------------------------------------------------------------
-- numeric(38, 10): the pair of lanes in turn, a NaN a candidate of everything
-- ---------------------------------------------------------------------------------------------------------
SELECT col_cmp('w_n38', '0:gt:1234.5', 'n38 > 1234.5 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:ge:1234.5', 'n38 >= 1234.5 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:lt:-1234.5', 'n38 < -1234.5 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:le:-1234.5', 'n38 <= -1234.5 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:eq:0', 'n38 = 0 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:ne:0', 'n38 <> 0 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:eq:9999999999999999999999999999.9999999999', 'n38 = 9999999999999999999999999999.9999999999 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:gt:9999999999999999999999999999.9999999998', 'n38 > 9999999999999999999999999999.9999999998 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:lt:-9999999999999999999999999999.9999999999', 'n38 < -9999999999999999999999999999.9999999999 OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:ge:-9999999999999999999999999999.9999999999', 'n38 >= -9999999999999999999999999999.9999999999 OR n38 = ''NaN''');
-- constants beyond 128 bits: below or above every value
SELECT col_cmp('w_n38', '0:lt:99999999999999999999999999999999999999999999', 'n38 IS NOT NULL OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:gt:99999999999999999999999999999999999999999999', 'n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:gt:-99999999999999999999999999999999999999999999', 'n38 IS NOT NULL OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:le:-99999999999999999999999999999999999999999999', 'n38 = ''NaN''');
-- the hi lane is the same for a whole unit when the values are small (|v * 10^10| < 2^63): the order is the low lane's
SELECT col_cmp('w_n38', '0:gt:12.34;0:lt:5000.5', '(n38 > 12.34 AND n38 < 5000.5) OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:ge:-0.0000000001;0:le:0.0000000001', '(n38 >= -0.0000000001 AND n38 <= 0.0000000001) OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:in:0|1234.56|9999999999999999999999999999.9999999999|-1', 'n38 IN (0, 1234.56, 9999999999999999999999999999.9999999999, -1) OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:notin:0|1234.56|-1', 'n38 NOT IN (0, 1234.56, -1) OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:null', 'n38 IS NULL OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:notnull', 'n38 IS NOT NULL OR n38 = ''NaN''');
SELECT col_cmp('w_n38', '0:gt:100;0:lt:200;0:ne:150', '(n38 > 100 AND n38 < 200 AND n38 <> 150) OR n38 = ''NaN''');
-- numeric(19, 2)
SELECT col_cmp('w_n19', '0:gt:-5000.5;0:le:99999999999999999.99', '(n19 > -5000.5 AND n19 <= 99999999999999999.99) OR n19 = ''NaN''');
SELECT col_cmp('w_n19', '0:eq:99999999999999999.99', 'n19 = 99999999999999999.99 OR n19 = ''NaN''');
SELECT col_cmp('w_n19', '0:lt:-99999999999999999.99', 'n19 < -99999999999999999.99 OR n19 = ''NaN''');
-- numeric(3, 1) is narrow (one leaf), numeric(5, -2) and numeric(40, 5) are images
SELECT leaf, path FROM sift.col_leaves('w_n3') ORDER BY leaf;
SELECT col_cmp('w_n3', '0:ge:25', 'n3 >= 2.5 OR n3 = ''NaN''');
SELECT col_cmp('w_neg', '0:gt:-5000', 'neg > -5000');
SELECT col_cmp('w_huge', '0:ge:100000000000000000000000000000000.5', 'huge >= 100000000000000000000000000000000.5');

-- ---------------------------------------------------------------------------------------------------------
-- numeric image: equal numbers of other scales, NaN greatest and equal to itself, the infinities, 400 digits
-- ---------------------------------------------------------------------------------------------------------
SELECT col_cmp('w_img', '0:eq:1', 'img = 1');
SELECT col_cmp('w_img', '0:eq:1.0000', 'img = 1.0000');
SELECT col_cmp('w_img', '0:eq:1.5', 'img = 1.5');
SELECT col_cmp('w_img', '0:ne:1', 'img <> 1');
SELECT col_cmp('w_img', '0:gt:1', 'img > 1');
SELECT col_cmp('w_img', '0:ge:1.5', 'img >= 1.5');
SELECT col_cmp('w_img', '0:lt:-0.0001', 'img < -0.0001');
SELECT col_cmp('w_img', '0:le:0', 'img <= 0');
SELECT col_cmp('w_img', '0:eq:NaN', 'img = ''NaN''');
SELECT col_cmp('w_img', '0:gt:Infinity', 'img > ''Infinity''');
SELECT col_cmp('w_img', '0:lt:-Infinity', 'img < ''-Infinity''');
SELECT col_cmp('w_img', '0:le:-Infinity', 'img <= ''-Infinity''');
SELECT col_cmp('w_img', '0:in:1|1.5|NaN|Infinity|0.0', 'img IN (1, 1.5, ''NaN'', ''Infinity'', 0.0)');
SELECT col_cmp('w_img', '0:notin:1|1.5|NaN', 'img NOT IN (1, 1.5, ''NaN'')');
SELECT col_cmp('w_img', '0:gt:1' || repeat('0', 399), 'img > 1' || repeat('0', 399));
SELECT col_cmp('w_img', '0:lt:-1' || repeat('0', 399), 'img < -1' || repeat('0', 399));
SELECT col_cmp('w_img', '0:gt:0.' || repeat('0', 299) || '1', 'img > 0.' || repeat('0', 299) || '1');
SELECT col_cmp('w_img', '0:null', 'img IS NULL');
SELECT col_cmp('w_img', '0:gt:-1000;0:lt:1000', 'img > -1000 AND img < 1000');
SELECT col_cmp('w_img', '0:gt:12345678901234567890.123456789;0:lt:1' || repeat('0', 40), 'img > 12345678901234567890.123456789 AND img < 1' || repeat('0', 40));
-- one image per number: 1.0, 1.00 and 1 are the same dictionary entry, the display scales are lane 1
SELECT count(DISTINCT val) AS distinct_lane1 FROM sift._test_col_read('w_img', 1, '0:eq:1');
SELECT (SELECT count(*) FROM sift._test_col_read('w_img', 0, '0:eq:1')) = (SELECT count(*) FROM w WHERE img = 1) AS equal_scales_found;

-- ---------------------------------------------------------------------------------------------------------
-- interval: the total orders and groups (1 mon = 30 days = 720 hours), month and day tell them apart
-- ---------------------------------------------------------------------------------------------------------
SELECT col_cmp('w_iv', '0:eq:1 month', 'iv = interval ''1 month''');
SELECT col_cmp('w_iv', '0:eq:30 days', 'iv = interval ''30 days''');
SELECT col_cmp('w_iv', '0:eq:1 mon 1 day', 'iv = interval ''31 days''');
SELECT col_cmp('w_iv', '0:gt:1 month', 'iv > interval ''1 month''');
SELECT col_cmp('w_iv', '0:ge:1 month', 'iv >= interval ''1 month''');
SELECT col_cmp('w_iv', '0:lt:-1 month', 'iv < interval ''-1 month''');
SELECT col_cmp('w_iv', '0:le:0', 'iv <= interval ''0''');
SELECT col_cmp('w_iv', '0:ne:0', 'iv <> interval ''0''');
SELECT col_cmp('w_iv', '0:gt:59 days;0:le:2 mons', 'iv > interval ''59 days'' AND iv <= interval ''2 mons''');
SELECT col_cmp('w_iv', '0:in:1 month|1 year|-00:00:00.000001', 'iv IN (interval ''1 month'', interval ''1 year'', interval ''-00:00:00.000001'')');
SELECT col_cmp('w_iv', '0:notin:0|1 day', 'iv NOT IN (interval ''0'', interval ''1 day'')');
SELECT col_cmp('w_iv', '0:eq:infinity', 'iv = interval ''infinity''');
SELECT col_cmp('w_iv', '0:gt:2147483647 months 2147483647 days 9223372036854775806 us', 'iv > interval ''2147483647 months 2147483647 days 9223372036854775806 us''');
SELECT col_cmp('w_iv', '0:lt:-infinity', 'iv < interval ''-infinity''');
SELECT col_cmp('w_iv', '0:null', 'iv IS NULL');
SELECT (SELECT count(*) FROM sift._test_col_read('w_iv', 0, '0:eq:1 month')) = (SELECT count(*) FROM w WHERE iv = '720 hours') AS month_is_720_hours;

-- ---------------------------------------------------------------------------------------------------------
-- money: the int64 of the smallest unit
-- ---------------------------------------------------------------------------------------------------------
SELECT col_cmp('w_m', '0:gt:12345', 'm > ''123.45''::money');
SELECT col_cmp('w_m', '0:le:-12345', 'm <= ''-123.45''::money');
SELECT col_cmp('w_m', '0:eq:9223372036854775807', 'm = ''92233720368547758.07''::money');
SELECT col_cmp('w_m', '0:eq:-9223372036854775808', 'm = ''-92233720368547758.08''::money');
SELECT col_cmp('w_m', '0:in:0|100|-100', 'm IN (''0'', ''1.00'', ''-1.00'')');
SELECT col_cmp('w_m', '0:null', 'm IS NULL');

-- ---------------------------------------------------------------------------------------------------------
-- Zones: a unit is skipped when the pair cannot be inside its two lane zones
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE wz (id int, v numeric(30, 4));
INSERT INTO wz SELECT i, (i / 1000)::numeric * 1000000000000000000000 + (i % 1000) FROM generate_series(0, 99999) i;
CREATE INDEX wz_idx ON wz USING sift_col (v);
SELECT sift.col_verify('wz_idx', true);
SELECT col_cmp('wz_idx', '0:eq:42000000000000000000999', 'v = 42000000000000000000999');
SELECT col_cmp('wz_idx', '0:gt:42000000000000000000999', 'v > 42000000000000000000999');
SELECT col_cmp('wz_idx', '0:le:3000000000000000000000', 'v <= 3000000000000000000000');
SELECT col_cmp('wz_idx', '0:ge:99000000000000000000999.0001', 'v >= 99000000000000000000999.0001');
SELECT col_cmp('wz_idx', '0:in:7|99000000000000000000999|50000000000000000000500', 'v IN (7, 99000000000000000000999, 50000000000000000000500)');
-- a unit whose lane zones cannot hold the pair is not read: the pair (hi, lo) of the constant is out of its box
SELECT units, units_pruned FROM sift._test_col_counters('wz_idx', '0:eq:42000000000000000000999', 0);
SELECT units, units_pruned FROM sift._test_col_counters('wz_idx', '0:ne:42000000000000000000999', 0);
SELECT units, units_pruned FROM sift._test_col_counters('wz_idx', '0:gt:100000000000000000000000000', 0);
SELECT units, units_pruned FROM sift._test_col_counters('wz_idx', '0:lt:-100000000000000000000000000', 0);
SELECT units, units_pruned FROM sift._test_col_counters('wz_idx', '0:ge:60000000000000000000000;0:le:60000000000000000000999', 0);
DROP TABLE wz;

-- ---------------------------------------------------------------------------------------------------------
-- The write buffer, promotion, merge, VACUUM and REINDEX keep the values
-- ---------------------------------------------------------------------------------------------------------
INSERT INTO w (id, n38, n19, img, m, iv)
SELECT 30000 + i, CASE WHEN i % 7 = 0 THEN 'NaN' ELSE i * 1234567.1234567891 END, i / 3.0, CASE WHEN i % 5 = 0 THEN 'NaN' ELSE round(i / 7.0, i % 9) END, (i * 17 || '.' || i % 100)::money,
	   make_interval(months => i % 14, days => i % 40, secs => i * 0.25)
FROM generate_series(1, 3000) i;
SELECT col_cmp('w_n38', '0:gt:1234.5', 'n38 > 1234.5 OR n38 = ''NaN''');
SELECT col_cmp('w_img', '0:ge:1.5', 'img >= 1.5');
SELECT col_cmp('w_iv', '0:gt:1 month', 'iv > interval ''1 month''');
SELECT col_cmp('w_m', '0:gt:12345', 'm > ''123.45''::money');
SELECT col_back('w_n38', 0, 'CASE WHEN n38 = ''NaN'' THEN NULL ELSE n38::text END'), col_back('w_img', 0, 'img::text'), col_back('w_iv', 0, 'iv::text'), col_back('w_m', 0, '(m::numeric * 100)::bigint::text');
SELECT kind, format FROM sift.col_segments('w_iv') WHERE kind <> 'immutable' ORDER BY 1;
SELECT sift.col_promote('w_n38'), sift.col_promote('w_img'), sift.col_promote('w_iv'), sift.col_promote('w_m'), sift.col_promote('w_all');
SELECT col_cmp('w_n38', '0:gt:1234.5', 'n38 > 1234.5 OR n38 = ''NaN''');
SELECT col_cmp('w_img', '0:ge:1.5', 'img >= 1.5');
SELECT col_cmp('w_iv', '0:gt:1 month', 'iv > interval ''1 month''');
SELECT col_cmp('w_m', '0:gt:12345', 'm > ''123.45''::money');
SELECT col_back('w_n38', 0, 'CASE WHEN n38 = ''NaN'' THEN NULL ELSE n38::text END', 1), col_back('w_img', 0, 'img::text', 1), col_back('w_iv', 0, 'iv::text', 1), col_back('w_m', 0, '(m::numeric * 100)::bigint::text', 1);
SELECT sift.col_verify(idx, true) FROM (VALUES ('w_n38'), ('w_img'), ('w_iv'), ('w_m'), ('w_all')) v(idx);
SELECT idx, count(*) AS segments, string_agg(DISTINCT format::text, ',') AS format
FROM (SELECT 'w_n38' AS idx, format FROM sift.col_segments('w_n38') WHERE kind = 'immutable'
	  UNION ALL SELECT 'w_iv', format FROM sift.col_segments('w_iv') WHERE kind = 'immutable') f GROUP BY idx ORDER BY idx;
SELECT sift.col_merge('w_n38', true), sift.col_merge('w_img', true), sift.col_merge('w_iv', true), sift.col_merge('w_m', true), sift.col_merge('w_all', true);
SELECT sift.col_verify(idx, true) FROM (VALUES ('w_n38'), ('w_img'), ('w_iv'), ('w_m'), ('w_all')) v(idx);
SELECT col_cmp('w_n38', '0:gt:1234.5;0:lt:99999999999', '(n38 > 1234.5 AND n38 < 99999999999) OR n38 = ''NaN''', 1);
SELECT col_cmp('w_img', '0:lt:0.5', 'img < 0.5', 1);
SELECT col_back('w_n38', 0, 'CASE WHEN n38 = ''NaN'' THEN NULL ELSE n38::text END', 1), col_back('w_img', 0, 'img::text', 1), col_back('w_iv', 0, 'iv::text', 1), col_back('w_m', 0, '(m::numeric * 100)::bigint::text', 1);
DELETE FROM w WHERE id % 5 = 1;
VACUUM w;
SELECT sift.col_verify(idx, true) FROM (VALUES ('w_n38'), ('w_img'), ('w_iv'), ('w_m'), ('w_all')) v(idx);
SELECT col_cmp('w_n38', '0:gt:1234.5', 'n38 > 1234.5 OR n38 = ''NaN''');
SELECT col_cmp('w_iv', '0:le:1 year', 'iv <= interval ''1 year''');
SELECT col_back('w_n38', 0, 'CASE WHEN n38 = ''NaN'' THEN NULL ELSE n38::text END'), col_back('w_img', 0, 'img::text'), col_back('w_iv', 0, 'iv::text'), col_back('w_m', 0, '(m::numeric * 100)::bigint::text');
REINDEX INDEX w_all;
REINDEX INDEX w_iv;
SELECT sift.col_verify('w_all', true), sift.col_verify('w_iv', true);
SELECT col_cmp('w_iv', '0:le:1 year', 'iv <= interval ''1 year''');
SELECT col_back('w_all', 1, 'CASE WHEN n38 = ''NaN'' THEN NULL ELSE n38::text END'), col_back('w_all', 6, 'iv::text'), col_back('w_all', 3, 'img::text'), col_back('w_all', 5, '(m::numeric * 100)::bigint::text');

-- the sums of the lanes of a unit are the pre-aggregates of plain integers (no lane is a measure or a dimension of a cube)
SELECT leaf, path FROM sift.col_leaves('w_all') ORDER BY leaf;

-- sift_table keeps its own mapping: every one of these types is stored exactly as bytes (or the 3.1 integer for the narrow numeric)
CREATE TABLE wt (id int, a numeric(38, 6), b numeric(5, 30), c numeric, d money, e interval, f numeric(10, 2)) USING sift_table;
INSERT INTO wt SELECT i, i * 123456789012345678901234.123456, i / 7.0 / 1e30, CASE WHEN i % 5 = 0 THEN 'NaN' ELSE i / 3.0 END, (i || '.25')::money,
	make_interval(months => i % 5, days => i % 31, secs => i), i / 8.0 FROM generate_series(1, 5000) i;
VACUUM wt;
SELECT count(*), sum(a), sum(f), max(d)::numeric AS max_d, max(e), count(*) FILTER (WHERE c = 'NaN') FROM wt;
SELECT count(*) FROM wt WHERE a > 1e28 AND e >= interval '1 month' AND d < '100.00';
DROP TABLE wt;

DROP TABLE w;
DROP FUNCTION col_cmp(regclass, text, text, int), col_back(regclass, int, text, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
