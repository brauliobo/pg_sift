-- An index entry of a version that a committed transaction deleted or updated long ago is marked dead by the scan that finds it (kill_prior_tuple):
-- the table AM says the version is dead for every snapshot (all_dead), so the b-tree does not fetch it again
CREATE EXTENSION pg_sift;
CREATE EXTENSION pageinspect;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int PRIMARY KEY, v int, note text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO t SELECT g, 0, 'n' || g FROM generate_series(1, 200) g;
SELECT sift.table_maintain('t') IS NOT NULL AS maintained;
-- three updates of every row, one transaction each statement: 400 versions are dead, the live one of every row is the last
UPDATE t SET v = v + 1;
UPDATE t SET v = v + 1;
UPDATE t SET v = v + 1;
CREATE FUNCTION dead_entries() RETURNS bigint LANGUAGE sql AS
$$ SELECT coalesce(sum((SELECT count(*) FROM bt_page_items('t_pkey', p) WHERE dead)), 0)
   FROM generate_series(1, (pg_relation_size('t_pkey') / 8192)::int - 1) p $$;
SELECT dead_entries() AS dead_before_scan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS rows_found, sum(v) AS sum_v FROM t WHERE id BETWEEN 1 AND 200;
SELECT dead_entries() > 0 AS dead_marked, dead_entries() <= 600 AS no_more_than_the_dead_versions;
SELECT (SELECT count(*) FROM t WHERE id = 77) AS one_row, (SELECT v FROM t WHERE id = 77) AS its_value;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE t;
DROP FUNCTION dead_entries();
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift;
