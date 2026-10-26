-- The arms of a UNION ALL that scan the same rows and join the same correlated LIMIT 1 lookup run in one pass (agg_fuse_arms) also when the lookup is read as
-- a table (a key that may repeat, a condition on the parameters alone): the arms read different columns of it and the planner numbers their Params apart, which
-- is not a difference of the join. Arms whose conditions on the parameters differ are two joins and run apart. The rows are the normal plan's either way.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET enable_nestloop = off;
CREATE TABLE ff_facts (id int, k int, g int, h int, skip bool);
INSERT INTO ff_facts SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE i % 2600 END, i % 5, i % 7, i % 13 = 0 FROM generate_series(1, 9000) i;
CREATE INDEX ff_facts_col ON ff_facts USING sift_col (id, k, g, h, skip);
CREATE TABLE ff_map (k int, v int, w text);
INSERT INTO ff_map SELECT k, k * 2, 'w' || k FROM generate_series(1, 2400) k;
INSERT INTO ff_map SELECT k, k * 2, 'w' || k FROM generate_series(1, 2400) k WHERE k % 3 = 1;
CREATE INDEX ff_map_k ON ff_map (k);
VACUUM (FREEZE, ANALYZE) ff_facts, ff_map;

CREATE FUNCTION ff_check(q text) RETURNS TABLE (arms_in_one_pass int, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  arms_in_one_pass := 0;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l LIKE '%Arms In One Pass: %' THEN arms_in_one_pass := regexp_replace(l, '.*: ', '')::int; END IF;
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;

CREATE TABLE ff_cases (name text, q text);
INSERT INTO ff_cases VALUES
 ('three arms, a key that repeats: each reads other columns', $q$
  SELECT 'g' AS arm, f.g AS k1, NULL::int AS k2, count(*) AS n, sum(m.v) AS sv FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k LIMIT 1) m ON true GROUP BY 2
  UNION ALL
  SELECT 'h', f.h, NULL, count(*), count(m.w) FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k LIMIT 1) m ON true GROUP BY 2
  UNION ALL
  SELECT 'gh', f.g, f.h, count(*), sum(m.v) FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k LIMIT 1) m ON true GROUP BY 2, 3 $q$),
 ('the same with a condition on the parameters alone in every arm', $q$
  SELECT 'g' AS arm, f.g AS k1, NULL::int AS k2, count(*) AS n, sum(m.v) AS sv FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k AND f.skip IS NOT TRUE LIMIT 1) m ON true GROUP BY 2
  UNION ALL
  SELECT 'h', f.h, NULL, count(*), count(m.w) FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k AND f.skip IS NOT TRUE LIMIT 1) m ON true GROUP BY 2
  UNION ALL
  SELECT 'gh', f.g, f.h, count(*), count(m.w) + count(m.v) FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k AND f.skip IS NOT TRUE LIMIT 1) m ON true GROUP BY 2, 3 $q$),
 ('arms with another condition on the parameters are two joins', $q$
  SELECT 'g' AS arm, f.g AS k1, count(*) AS n, sum(m.v) AS sv FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k AND f.g <> 2 LIMIT 1) m ON true GROUP BY 2
  UNION ALL
  SELECT 'h', f.h, count(*), sum(m.v) FROM ff_facts f LEFT JOIN LATERAL (SELECT v, w FROM ff_map WHERE ff_map.k = f.k AND f.g <> 3 LIMIT 1) m ON true GROUP BY 2 $q$);
SELECT name, (ff_check(q)).* FROM ff_cases ORDER BY name;
SELECT name, (ff_check(q)).* FROM ff_cases ORDER BY name;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SELECT name, (ff_check(q)).* FROM ff_cases ORDER BY name;
DROP FUNCTION ff_check(text);
DROP TABLE ff_cases, ff_facts, ff_map;
DROP EXTENSION pg_sift;
