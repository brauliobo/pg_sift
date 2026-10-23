-- A serial scan of a table partitioned by (part of) the group key finishes the groups of a partition when its units end: no other partition holds a
-- row of them. Every query runs with and without the scan: the rows must be the same; "finished" says whether the groups were finished per partition.
-- Partitions that hold nothing, a default partition, NULL keys, a pruned range, a key that lacks (part of) the partition key, a float sum (added in lane
-- order: rounded, as the sum is not the normal plan's to the last digit), a partition of a partition and an expression as the partition key keep the
-- single table, and the answers do not change.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE pr (id int, k int, g int, u int, d date, v float8) PARTITION BY RANGE (k);
CREATE TABLE pr_1 PARTITION OF pr FOR VALUES FROM (0) TO (100);
CREATE TABLE pr_2 PARTITION OF pr FOR VALUES FROM (100) TO (200);
CREATE TABLE pr_3 PARTITION OF pr FOR VALUES FROM (200) TO (300);        -- no row
CREATE TABLE pr_4 PARTITION OF pr FOR VALUES FROM (300) TO (400);
CREATE TABLE pr_x PARTITION OF pr DEFAULT;                               -- NULL keys, keys out of the ranges
INSERT INTO pr SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 500 + i % 7 WHEN i % 5 = 0 THEN i % 100 + 300 ELSE i % 200 END, i % 7,
                      CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 31) % 211 END, date '2024-01-01' + i % 90, (i % 50)::float8 / 3
FROM generate_series(1, 6000) i;
CREATE INDEX pr_col ON pr USING sift_col (id, k, g, u, d, v);

CREATE TABLE ph (id int, k int, u int) PARTITION BY HASH (k);
CREATE TABLE ph_0 PARTITION OF ph FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE ph_1 PARTITION OF ph FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE ph_2 PARTITION OF ph FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO ph SELECT i, i % 150, (i * 17) % 301 FROM generate_series(1, 4000) i;
CREATE INDEX ph_col ON ph USING sift_col (id, k, u);

CREATE TABLE pl (id int, k text, u int) PARTITION BY LIST (k);
CREATE TABLE pl_a PARTITION OF pl FOR VALUES IN ('a', 'b');
CREATE TABLE pl_b PARTITION OF pl FOR VALUES IN ('c', NULL);
CREATE TABLE pl_c PARTITION OF pl FOR VALUES IN ('d');
INSERT INTO pl SELECT i, (ARRAY['a', 'b', 'c', 'd', NULL])[1 + i % 5], (i * 7) % 101 FROM generate_series(1, 3000) i;
CREATE INDEX pl_col ON pl USING sift_col (id, k, u);

-- a partition of a partition: the groups of a partition go on in the next leaf
CREATE TABLE pm (id int, k int, s int, u int) PARTITION BY RANGE (k);
CREATE TABLE pm_1 PARTITION OF pm FOR VALUES FROM (0) TO (100) PARTITION BY HASH (s);
CREATE TABLE pm_1a PARTITION OF pm_1 FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE pm_1b PARTITION OF pm_1 FOR VALUES WITH (MODULUS 2, REMAINDER 1);
CREATE TABLE pm_2 PARTITION OF pm FOR VALUES FROM (100) TO (200);
INSERT INTO pm SELECT i, i % 200, i % 9, (i * 3) % 77 FROM generate_series(1, 3000) i;
CREATE INDEX pm_col ON pm USING sift_col (id, k, s, u);

-- an expression as the partition key
CREATE TABLE pe (id int, k int, u int) PARTITION BY RANGE ((k % 100));
CREATE TABLE pe_1 PARTITION OF pe FOR VALUES FROM (0) TO (50);
CREATE TABLE pe_2 PARTITION OF pe FOR VALUES FROM (50) TO (100);
INSERT INTO pe SELECT i, i % 300, (i * 5) % 61 FROM generate_series(1, 2000) i;
CREATE INDEX pe_col ON pe USING sift_col (id, k, u);
VACUUM (FREEZE, ANALYZE) pr, ph, pl, pm, pe;

-- whether the scan answers the query, finishes the groups per partition, and the rows it differs by from the plan without it
CREATE FUNCTION check_same(q text) RETURNS TABLE (pushed bool, finished bool, rows bigint, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  SELECT count(*) INTO rows FROM r_on;
  pushed := false;
  finished := false;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
    finished := finished OR l LIKE '%Groups Finished: per partition%';
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;

-- no count(DISTINCT): the single table (the sets are what make the table of all partitions large); the NULL key and the default partition are groups like the others
SELECT * FROM check_same($q$SELECT k, count(*) AS n, sum(u) AS su, min(d) AS d0, max(g) AS g1 FROM pr GROUP BY k$q$);
-- count(DISTINCT): the sets of a partition are over when it ends; counts, integer sums, min and max are finished with them
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du, count(DISTINCT g) AS dg FROM pr GROUP BY k$q$);
-- HAVING reads an aggregate the select list does not
SELECT * FROM check_same($q$SELECT k, count(DISTINCT u) AS du FROM pr GROUP BY k HAVING count(*) > 20$q$);
-- the key holds the partition key and more
SELECT * FROM check_same($q$SELECT k, g, count(*) AS n, count(DISTINCT u) AS du FROM pr GROUP BY k, g$q$);
SELECT * FROM check_same($q$SELECT g, k, date_trunc('month', d)::date AS m, count(*) AS n, max(u) AS mu FROM pr GROUP BY g, k, 3$q$);
-- a pruned range: the partitions left
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM pr WHERE k BETWEEN 120 AND 330 GROUP BY k$q$);
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM pr WHERE k < 0 GROUP BY k$q$);
-- a condition on the rows
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM pr WHERE u IS NOT NULL AND g <> 3 GROUP BY k$q$);
-- hash and list partitions: the same value is always in the same partition
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du, sum(id) AS si FROM ph GROUP BY k$q$);
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM pl GROUP BY k$q$);
-- workers: the plan may have them, the answers are the same
SET max_parallel_workers_per_gather = 2;
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM pr GROUP BY k$q$);
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM ph GROUP BY k$q$);
SET max_parallel_workers_per_gather = 0;

-- not per partition: a key without the partition key, a float sum, a partition of a partition, an expression as the partition key
SELECT * FROM check_same($q$SELECT g, count(*) AS n, count(DISTINCT u) AS du FROM pr GROUP BY g$q$);
SELECT * FROM check_same($q$SELECT k, count(*) AS n, round(sum(v)::numeric, 6) AS sv, round(avg(v)::numeric, 6) AS av FROM pr GROUP BY k$q$);
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM pm GROUP BY k$q$);
SELECT * FROM check_same($q$SELECT k, count(*) AS n, count(DISTINCT u) AS du FROM pe GROUP BY k$q$);
SELECT * FROM check_same($q$SELECT k % 100 AS kk, count(*) AS n, count(DISTINCT u) AS du FROM pe GROUP BY k % 100$q$);

DROP TABLE pr, ph, pl, pm, pe;
DROP FUNCTION check_same(text);
DROP EXTENSION pg_sift;
