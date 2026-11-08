-- count(DISTINCT x) with a great deal of hash memory: a hash set that grows past the cache hands its pairs to the sort (which a set of many pairs does
-- faster), whose array is stripped of repeats when it first reaches a million pairs (a column of repeats) or left to grow (a column of unique values).
-- The counts are the plain plan's for both.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int, grp int NOT NULL, rep int NOT NULL, uniq int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 4, (i % 400000) * 1009, i * 251 FROM generate_series(1, 1500000) i;
CREATE INDEX ev_col ON ev USING sift_col (grp, rep, uniq);
VACUUM (FREEZE, ANALYZE) ev;
SET work_mem = '1GB';
SET sift.agg_force = on;
CREATE FUNCTION sets_spilled(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l ~ '(\d+) distinct sets spilled' THEN n := n + substring(l FROM '(\d+) distinct sets spilled')::int; END IF;
  END LOOP;
  RETURN n;
END $$;
SELECT sets_spilled($q$ SELECT grp, count(DISTINCT rep), count(DISTINCT uniq) FROM ev GROUP BY grp $q$) > 0 AS spilled;
CREATE TABLE got AS SELECT grp, count(DISTINCT rep) AS r, count(DISTINCT uniq) AS u FROM ev GROUP BY grp;
SET sift.enable_agg = off;
CREATE TABLE want AS SELECT grp, count(DISTINCT rep) AS r, count(DISTINCT uniq) AS u FROM ev GROUP BY grp;
SELECT (SELECT count(*) FROM got) AS groups, (SELECT count(*) FROM (SELECT * FROM got EXCEPT SELECT * FROM want) x) AS missing,
       (SELECT count(*) FROM (SELECT * FROM want EXCEPT SELECT * FROM got) x) AS extra, (SELECT max(r) FROM got) AS repeats, (SELECT max(u) FROM got) AS uniques;
DROP FUNCTION sets_spilled(text);
DROP TABLE got, want, ev;
DROP EXTENSION pg_sift;
