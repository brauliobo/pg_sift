-- count(DISTINCT x) of a nullable bigint column whose sets outgrow the hash memory: the pairs of group and value go through a sort of int8 datums, and the
-- counts are the plain plan's over NULL values, groups of NULLs only and values that repeat inside a group.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
CREATE TABLE ev (id int PRIMARY KEY, grp int NOT NULL, uid bigint, pad text);
INSERT INTO ev
SELECT i, i % 61, CASE WHEN i % 61 IN (5, 6) OR i % 9 = 0 THEN NULL ELSE (hashint4(i) & 32767)::bigint * 1000003 END, 'p'
FROM generate_series(1, 30000) i;
CREATE INDEX ev_col ON ev USING sift_col (grp, uid);
VACUUM (FREEZE, ANALYZE) ev;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
CREATE FUNCTION sets_spilled(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l LIKE '%Distinct Sets Spilled:%' THEN n := n + substring(l FROM 'Spilled: (\d+)')::int; END IF;
  END LOOP;
  RETURN n;
END $$;
SELECT sets_spilled($q$ SELECT grp, count(DISTINCT uid), count(*) FROM ev GROUP BY grp $q$) > 0 AS spilled;
CREATE TABLE got AS SELECT grp, count(DISTINCT uid) AS d, count(*) AS n FROM ev GROUP BY grp;
SET sift.enable_agg = off;
CREATE TABLE want AS SELECT grp, count(DISTINCT uid) AS d, count(*) AS n FROM ev GROUP BY grp;
SELECT (SELECT count(*) FROM got) AS groups, (SELECT count(*) FROM (SELECT * FROM got EXCEPT SELECT * FROM want) x) AS missing,
       (SELECT count(*) FROM (SELECT * FROM want EXCEPT SELECT * FROM got) x) AS extra, (SELECT count(*) FROM got WHERE d = 0) AS null_only_groups;
DROP FUNCTION sets_spilled(text);
DROP TABLE got, want, ev;
DROP EXTENSION pg_sift;
