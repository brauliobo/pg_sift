-- Format major 2: what a snapshot trusts of a part and of its deletion vector. A writer older than the snapshot has ended, not committed: only a part sealed for its inserters (all of
-- them committed) and a deletion vector settled for its deleters are read without the version vector and the log; a part of a transaction that aborted is never sealed and is never
-- seen. The heap's visibility map answers for the rows of a granule only while the part is current. A manifest commit takes no transaction id of its own, a VACUUM's deletes are in
-- the manifest before the map says its pages are all-visible, and a delete the partition lost its row to does not stop a fold.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

-- Inserters: committed, aborted, and the seal (flag 512)
CREATE TABLE v (v int);
CREATE INDEX v_idx ON v USING sift_store_test (v);
SELECT sift._test_f2_init('v_idx');
BEGIN;
SELECT pg_current_xact_id()::text::int8 AS aborted \gset
ROLLBACK;
SELECT pg_current_xact_id()::text::int8 AS committed \gset
SELECT sift._test_f2_add_part('v_idx', 1, NULL, 1000, 0, 100000, :aborted) AS ga \gset
SELECT sift._test_f2_add_part('v_idx', 1, NULL, 1000, 5000, 200000, :committed) AS gc \gset
SELECT sift._test_f2_add_part('v_idx', 1, NULL, 1000, 9000, 300000, 0) AS gf \gset
SELECT pg_current_xact_id() IS NOT NULL AS later_xid;
SELECT g, flags & 512 <> 0 AS sealed FROM (VALUES (:ga), (:gc), (:gf)) v(g), LATERAL (SELECT flags FROM sift._test_f2_parts('v_idx', 1, NULL) WHERE part_gen = g) p ORDER BY g;
SELECT visible, judged FROM sift._test_f2_visible('v_idx', 1, NULL, :ga);
SELECT visible, judged FROM sift._test_f2_visible('v_idx', 1, NULL, :gc);
SELECT visible, judged FROM sift._test_f2_visible('v_idx', 1, NULL, :gf);
SELECT sealed, flags & 512 <> 0 AS has_flag FROM sift._test_f2_seal('v_idx', 1, NULL, :ga);
SELECT sealed, flags & 512 <> 0 AS has_flag FROM sift._test_f2_seal('v_idx', 1, NULL, :gc);
SELECT sealed, flags & 512 <> 0 AS has_flag FROM sift._test_f2_seal('v_idx', 1, NULL, :gc);
SELECT visible, judged FROM sift._test_f2_visible('v_idx', 1, NULL, :ga);
SELECT visible, judged FROM sift._test_f2_visible('v_idx', 1, NULL, :gc);
-- sealing keeps the bounds of the writers: a snapshot older than the committer does not see its rows as there
SELECT min_xid = max_xid AND max_xid = :committed AS bounds_kept FROM sift._test_f2_part_xids('v_idx', 1, NULL, :gc);
-- the compaction drops the rows of the writer that aborted and freezes those of the one that committed
SELECT done, rows_in, rows_out, aborted_dropped, frozen FROM sift._test_f2_compact('v_idx', 1, NULL, ARRAY[:ga, :gc]);

-- Deleters: a delete that rolled back never hides a row, and its vector is not settled; one that committed does, and it is
CREATE TABLE w (v int);
CREATE INDEX w_idx ON w USING sift_store_test (v);
SELECT sift._test_f2_init('w_idx');
SELECT sift._test_f2_add_part('w_idx', 1, NULL, 20000, 0, 100000, 0) AS gw \gset
BEGIN;
SELECT pg_current_xact_id()::text::int8 AS aborted_del \gset
SELECT sift._test_f2_delete('w_idx', 1, NULL, 100000, 0, 200, 1, :aborted_del);
ROLLBACK;
SELECT pg_current_xact_id() IS NOT NULL AS later_xid;
SELECT flags & 1024 <> 0 AS dv_settled FROM sift._test_f2_parts('w_idx', 1, NULL);
SELECT visible, judged FROM sift._test_f2_visible('w_idx', 1, NULL, :gw);
SELECT sift._test_f2_delete('w_idx', 1, NULL, 100000, 300, 200, 1, :committed);
SELECT flags & 1024 <> 0 AS dv_settled FROM sift._test_f2_parts('w_idx', 1, NULL);
SELECT visible, judged FROM sift._test_f2_visible('w_idx', 1, NULL, :gw);
-- the deleters that committed are gone from the vector by the first rewrite, and the dead rows with them; the one that aborted never counted
SELECT done, rows_in, rows_out, dead_dropped FROM sift._test_f2_compact('w_idx', 1, NULL, ARRAY[:gw]);

-- a vector whose deleters all committed is settled: its dead rows are dead outright
CREATE TABLE x (v int);
CREATE INDEX x_idx ON x USING sift_store_test (v);
SELECT sift._test_f2_init('x_idx');
SELECT sift._test_f2_add_part('x_idx', 1, NULL, 1000, 0, 100000, 0) AS gz \gset
SELECT sift._test_f2_delete('x_idx', 1, NULL, 100000, 0, 100, 1, :committed);
SELECT flags & 1024 <> 0 AS dv_settled FROM sift._test_f2_parts('x_idx', 1, NULL);
SELECT visible, judged FROM sift._test_f2_visible('x_idx', 1, NULL, :gz);

-- A delete that names no row of any part of its partition (its part was compacted away) is dropped, and the fold that holds it finishes
SELECT sift._test_f2_delete('w_idx', 1, NULL, 999999, 0, 100, 1);
SELECT sift._test_f2_delete('w_idx', 1, NULL, 999999, 0, 10, 1);
SELECT sift._test_f2_pending('w_idx', 1, NULL);
SELECT sift._test_f2_delete('w_idx', 1, NULL, 999999, 20, 70, 1);
SELECT sift._test_f2_pending('w_idx', 1, NULL);
SELECT sift._test_f2_check('w_idx') ~ '^parts=' AS consistent;

-- A manifest commit and a reclaim take no transaction id of their own
SELECT sift._test_f2_add_part('w_idx', 1, NULL, 100, 70000, 700000, 0) AS gx \gset
BEGIN;
SELECT done FROM sift._test_f2_compact('w_idx', 1, NULL, ARRAY[:gx]);
SELECT sift._test_f2_reclaim('w_idx') >= 0 AS reclaimed;
SELECT txid_current_if_assigned() IS NULL AS no_xid;
COMMIT;

-- Subtransactions: the rows a transaction commits are written under its own transaction id, those of its savepoints included, and an aborted savepoint writes none
CREATE TABLE s (v int);
CREATE INDEX s_idx ON s USING sift_store_test (v);
SELECT sift._test_f2_init('s_idx');
BEGIN;
SELECT sift._test_f2_stage('s_idx', 100, 0, 1, -1);
SAVEPOINT a;
SELECT sift._test_f2_stage('s_idx', 100, 10000, 100000, -1);
ROLLBACK TO a;
SAVEPOINT b;
SELECT sift._test_f2_stage('s_idx', 100, 20000, 200000, -1);
RELEASE b;
SELECT pg_current_xact_id()::text::int8 AS top \gset
COMMIT;
SELECT count(*) AS parts, bool_and(min_xid = :top AND max_xid = :top) AS under_top_xid, sum(nrows) AS rows
	FROM sift._test_f2_parts('s_idx', 1, NULL) p, LATERAL sift._test_f2_part_xids('s_idx', 1, NULL, p.part_gen);

-- The heap's visibility map: a granule of a table that VACUUM made all-visible is counted without a heap page, rows on pages that are not all-visible are asked of the heap, a part
-- read before VACUUM removed its dead rows does not trust the map
CREATE TABLE m (id int, v int);
INSERT INTO m SELECT g, g % 100 FROM generate_series(1, 40000) g;
CREATE INDEX m_idx ON m USING sift_col (id, v) WITH (key_order = 'id');
VACUUM m;
SELECT count(*) AS heap_rows FROM m;
SELECT visible, heap_pages FROM sift._test_f2_vis_count('m_idx', 'm');
INSERT INTO m SELECT g, g % 100 FROM generate_series(40001, 40500) g;
SELECT count(*) AS heap_rows FROM m;
SELECT visible, heap_pages > 0 AS heap_asked, heap_pages < pg_relation_size('m') / 8192 / 4 AS few_pages FROM sift._test_f2_vis_count('m_idx', 'm');
SET work_mem = '64kB';
BEGIN;
INSERT INTO m SELECT g, g % 100 FROM generate_series(50001, 56000) g;
ROLLBACK;
RESET work_mem;
SELECT count(*) AS heap_rows FROM m;
SELECT visible FROM sift._test_f2_vis_count('m_idx', 'm');
DELETE FROM m WHERE id <= 500;
SELECT count(*) AS heap_rows FROM m;
SELECT visible FROM sift._test_f2_vis_count('m_idx', 'm');
VACUUM m;
SELECT count(*) AS heap_rows FROM m;
SELECT visible, heap_pages FROM sift._test_f2_vis_count('m_idx', 'm');
SELECT visible, heap_pages > 0 AS heap_asked FROM sift._test_f2_vis_count('m_idx', 'm', true);

DROP TABLE v;
DROP TABLE w;
DROP TABLE x;
DROP TABLE s;
DROP TABLE m;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
