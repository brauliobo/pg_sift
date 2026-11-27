-- count(DISTINCT x) with a great deal of hash memory: the exact set holds every pair of a column of repeats and of a column of unique values in memory,
-- with no spill. The counts are the plain plan's for both.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
-- the superblocks a parallel build gives each segment depend on which participant reads which blocks, so the share of order chunks that come out compact (the advice) varies from run to run
SET max_parallel_maintenance_workers = 0;
CREATE TABLE ev (id int, grp int NOT NULL, rep int NOT NULL, uniq int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 4, (i % 400000) * 1009, i * 251 FROM generate_series(1, 1500000) i;
CREATE INDEX ev_col ON ev USING sift_col (grp, rep, uniq);
VACUUM (FREEZE, ANALYZE) ev;
SET work_mem = '1GB';
SET sift.agg_force = on;
CREATE FUNCTION spills_of(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l ~ 'spills=\d+' THEN n := n + substring(l FROM 'spills=(\d+)')::int; END IF;
  END LOOP;
  RETURN n;
END $$;
SELECT spills_of($q$ SELECT grp, count(DISTINCT rep), count(DISTINCT uniq) FROM ev GROUP BY grp $q$) AS spills;
CREATE TABLE got AS SELECT grp, count(DISTINCT rep) AS r, count(DISTINCT uniq) AS u FROM ev GROUP BY grp;
SET sift.enable_agg = off;
CREATE TABLE want AS SELECT grp, count(DISTINCT rep) AS r, count(DISTINCT uniq) AS u FROM ev GROUP BY grp;
SELECT (SELECT count(*) FROM got) AS groups, (SELECT count(*) FROM (SELECT * FROM got EXCEPT SELECT * FROM want) x) AS missing,
       (SELECT count(*) FROM (SELECT * FROM want EXCEPT SELECT * FROM got) x) AS extra, (SELECT max(r) FROM got) AS repeats, (SELECT max(u) FROM got) AS uniques;
DROP FUNCTION spills_of(text);
DROP TABLE got, want, ev;
DROP EXTENSION pg_sift;
