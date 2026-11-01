-- the file of a sift_col index after a stream of small batches: the merge cuts the free end it leaves
-- label: the rows of the run inserted in batches of 1,000 (one transaction each) into a table with a `sift_col` index and `sift.col_auto_merge` on, then `VACUUM`; the metric is the index size at the end; the other lane keeps a b-tree
--@ prepare
CREATE TABLE w (id int, k int, s text, f float8);
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_ix ON w USING sift_col (id, k, s, f); ELSE CREATE INDEX w_ix ON w (id, k, s, f); END IF; END $$
--@ load
DO $$ BEGIN FOR i IN 0..({rows} / 1000 - 1) LOOP INSERT INTO w SELECT g, g % 1013, 'x' || (g % 97), g::float8 FROM generate_series(i * 1000 + 1, (i + 1) * 1000) g; COMMIT; END LOOP; END $$;
VACUUM w
--@ check
SELECT count(*) FROM w
