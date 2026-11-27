-- A boolean column of a sift_col index in a WHERE clause (NOT flag, flag, flag = true, true = flag) is a scan key of the aggregate scan: its comparison carries the function
-- of its operator, which the scan key of the index is built with (an Assert-enabled server checks it). Every statement gives the rows of the plan without the extension.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

CREATE TABLE bk (id int, grp int, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO bk SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 3 = 0 END FROM generate_series(1, 20000) i;
CREATE INDEX bk_col ON bk USING sift_col (id, grp, flag);
VACUUM (FREEZE, ANALYZE) bk;

SET sift.agg_force = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM bk WHERE NOT flag;
SELECT count(*) FROM bk WHERE NOT flag;
EXPLAIN (COSTS OFF) SELECT grp, count(*), sum(id) FROM bk WHERE flag GROUP BY grp ORDER BY grp;
SELECT grp, count(*), sum(id) FROM bk WHERE flag GROUP BY grp ORDER BY grp;
SELECT count(*) FROM bk WHERE flag = true;
SELECT count(*) FROM bk WHERE true = flag;
SET sift.enable_agg = off;
SELECT count(*) FROM bk WHERE NOT flag;
SELECT grp, count(*), sum(id) FROM bk WHERE flag GROUP BY grp ORDER BY grp;
SELECT count(*) FROM bk WHERE flag = true;
DROP TABLE bk;
DROP EXTENSION pg_sift;
