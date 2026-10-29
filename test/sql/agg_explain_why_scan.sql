-- sift.explain_why for a statement with no aggregate: the row "index" says whether the plan reads a sift_col index (index, index-only or bitmap scan) and, if not, why:
-- no_source (the table has none), where (no condition of the statement is one the index answers) or cost (the normal plan is estimated cheaper, with both costs).
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ew (id int, k int, grp int, note text);
INSERT INTO ew SELECT i, (hashint4(i) & 1023), i % 10, 'n' || i FROM generate_series(1, 100000) i;
CREATE TABLE ew_plain AS SELECT * FROM ew;
CREATE INDEX ew_col ON ew USING sift_col (id, k, grp);
VACUUM (FREEZE, ANALYZE) ew;
VACUUM (FREEZE, ANALYZE) ew_plain;
CREATE FUNCTION why(q text) RETURNS TABLE (arm text, used bool, slug text, has_scan_cost bool, has_normal_cost bool, scan_below_normal bool) LANGUAGE sql AS
$$ SELECT arm, used, slug, scan_cost IS NOT NULL, normal_cost IS NOT NULL, scan_cost < normal_cost FROM sift.explain_why(q) $$;
-- an equality on a key of the index: the plan reads it
SELECT * FROM why($q$ SELECT * FROM ew WHERE id = 5 $q$);
-- a table with no sift_col index
SELECT * FROM why($q$ SELECT * FROM ew_plain WHERE id = 5 $q$);
-- a condition on a column the index does not hold, and one the index cannot answer
SELECT * FROM why($q$ SELECT * FROM ew WHERE note = 'n5' $q$);
SELECT * FROM why($q$ SELECT * FROM ew WHERE id + k = 5 $q$);
-- a condition that keeps most of the table: the sequential scan is cheaper
SELECT * FROM why($q$ SELECT * FROM ew WHERE k >= 0 $q$);
-- an aggregate keeps its arms and gets no "index" row
SELECT * FROM why($q$ SELECT count(*) FROM ew WHERE grp = 3 $q$);
DROP FUNCTION why(text);
DROP TABLE ew, ew_plain;
DROP EXTENSION pg_sift;
