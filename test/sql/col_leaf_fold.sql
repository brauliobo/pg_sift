-- The statistics a plan reads of a sift_col index (the sections of its segments, folded once for the server in a record of shared memory) are the same whichever
-- segments a backend folds: when the record covers the first segments of the directory (a promotion added one), only the others are opened, and the statistics are
-- those of folding all of them. sift._test_leaf_fold folds both ways and says how many segments each opened and whether the bytes are the same.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
CREATE TABLE lf (id int, dim_a_id int, kind int, label text, amount float8, doc jsonb) WITH (autovacuum_enabled = off);
CREATE FUNCTION lf_rows(a int, b int) RETURNS void LANGUAGE sql AS $$
  INSERT INTO lf SELECT g, g % 40, g % 7, CASE WHEN g % 9 = 0 THEN NULL ELSE 'l' || (g % 50) END, g * 0.5,
         CASE WHEN g % 3 = 0 THEN jsonb_build_object('items', jsonb_build_array(jsonb_build_object('t', 'x' || g % 11, 'v', g % 5))) END
  FROM generate_series(a, b) g $$;
SELECT lf_rows(1, 6000);
CREATE INDEX lf_idx ON lf USING sift_col (id, dim_a_id, kind, label, amount, doc) WITH (key_cut = dim_a_id, paths = 'doc.items[]{t text, v float8}');
-- one segment, nothing in the ring yet
SELECT folded_shared, folded_full, same FROM sift._test_leaf_fold('lf_idx');
-- a promotion adds a segment: the record covers the first, the second is the only one opened
SELECT lf_rows(6001, 7500);
SELECT sift.col_promote('lf_idx') > 0 AS promoted;
SELECT count(*) AS segments FROM sift._test_col_formats('lf_idx');
SELECT folded_shared, folded_full, same FROM sift._test_leaf_fold('lf_idx');
-- and another one
SELECT lf_rows(7501, 8200);
SELECT sift.col_promote('lf_idx') > 0 AS promoted;
SELECT folded_shared, folded_full, same FROM sift._test_leaf_fold('lf_idx');
-- the same directory again: the record is the whole
SELECT folded_shared, folded_full, same FROM sift._test_leaf_fold('lf_idx');
-- rows in the write buffer do not move the record
SELECT lf_rows(8201, 8300);
SELECT folded_shared, folded_full, same FROM sift._test_leaf_fold('lf_idx');
-- a merge replaces the segments: no prefix, every segment is folded, and the statistics are the same
SELECT sift.col_merge('lf_idx', true) >= 0 AS merged;
SELECT folded_shared = folded_full AS all_folded, same FROM sift._test_leaf_fold('lf_idx');
-- a promotion after the merge goes back to adding one
SELECT lf_rows(8301, 9000);
SELECT sift.col_promote('lf_idx') > 0 AS promoted;
SELECT folded_shared, folded_full - 1 AS others, same FROM sift._test_leaf_fold('lf_idx');
DROP FUNCTION lf_rows(int, int);
DROP TABLE lf;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
