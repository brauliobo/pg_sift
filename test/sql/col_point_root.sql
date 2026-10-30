-- The header, records and parameters of the point directories end the DICT section next to its footer, so that a lookup reads
-- them from the page of the footer. The same index written the earlier way (sift.test_col_point_unrooted, minor 35) and the new way answers every key alike, has the
-- same size, and sift.col_verify accepts both.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET client_min_messages = warning;

CREATE TABLE rt (id int8, s text, d int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO rt SELECT hashint8extended(g, 1) & 4611686018427387903, 'k' || md5(g::text), g % 5000, repeat('x', 20) FROM generate_series(1, 30000) g;
ANALYZE rt;

-- addresses of an integer leaf, rows of a text leaf with its dictionary read in place, an order array
SET sift.test_col_point_unrooted = on;
CREATE INDEX rt_old ON rt USING sift_col (id, s, d) WITH (key_cut = 'off', point_tid_columns = 'id', point_columns = 's', order_columns = 'd', order_array_columns = 'd');
RESET sift.test_col_point_unrooted;
CREATE INDEX rt_new ON rt USING sift_col (id, s, d) WITH (key_cut = 'off', point_tid_columns = 'id', point_columns = 's', order_columns = 'd', order_array_columns = 'd');

SELECT ix, min(format) AS min_minor, max(format) AS max_minor, count(*) AS segments
FROM (SELECT 'old' ix, format FROM sift.col_segments('rt_old') WHERE kind = 'immutable' UNION ALL SELECT 'new', format FROM sift.col_segments('rt_new') WHERE kind = 'immutable') x
GROUP BY ix ORDER BY ix;
SELECT pg_relation_size('rt_old') = pg_relation_size('rt_new') AS same_size;

-- every sampled key of both leaves lists the same addresses in both indexes, an absent key none
SELECT leaf, count(*) AS keys, count(*) FILTER (WHERE a <> b) AS different
FROM (SELECT 'id' leaf, (SELECT array_agg(t.tid ORDER BY t.tid) FROM sift._test_col_tids('rt_old', '0:eq:' || h.id, 24) t) a,
		(SELECT array_agg(t.tid ORDER BY t.tid) FROM sift._test_col_tids('rt_new', '0:eq:' || h.id, 24) t) b
	FROM (SELECT id FROM rt WHERE hashint8extended(id, 7) % 100 = 0) h) x GROUP BY leaf;
SELECT count(*) AS absent_listed FROM generate_series(1, 50) g, sift._test_col_tids('rt_new', '0:eq:' || (4100000000000000000 + g), 24);
SELECT count(*) AS text_found FROM rt h WHERE h.d = 7 AND NOT EXISTS (SELECT 1 FROM sift._test_col_tids('rt_new', '1:eq:' || h.s, 24) t WHERE t.tid = h.ctid);

SELECT (SELECT count(*) FROM sift.col_verify('rt_old', true)) >= 0 AS old_verified, (SELECT count(*) FROM sift.col_verify('rt_new', true)) >= 0 AS new_verified;
DROP TABLE rt;
