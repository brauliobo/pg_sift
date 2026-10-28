-- A correlated LIMIT 1 lookup of a table that may hold a key more than once (no unique index on it) is read as a lookup table where the rows of the key
-- agree on the columns the query reads; the child runs for a key whose rows differ, whichever of them its plan meets first. A condition the child tests
-- of its parameters alone is tested for the input. The table is read once the child has run for many keys (or a cache holds it).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET enable_nestloop = off;
CREATE TABLE fd_facts (id int, k int, g int, skip bool);
INSERT INTO fd_facts SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE i % 2600 END, i % 5, i % 13 = 0 FROM generate_series(1, 9000) i;
CREATE INDEX fd_facts_col ON fd_facts USING sift_col (id, k, g, skip);
-- keys 1..2400 by their residue modulo 3: 0 one row, 1 two rows that agree on v and w (a NULL w in some) and not on z, 2 two rows that differ
CREATE TABLE fd_map (k int, v int, w text, z text);
INSERT INTO fd_map SELECT k, k * 2, CASE WHEN k % 6 = 4 THEN NULL ELSE 'w' || k END, 'z' || k FROM generate_series(1, 2400) k;
INSERT INTO fd_map SELECT k, k * 2, CASE WHEN k % 6 = 4 THEN NULL ELSE 'w' || k END, 'another' FROM generate_series(1, 2400) k WHERE k % 3 = 1;
INSERT INTO fd_map SELECT k, k * 2 + 1, 'other', 'z' || k FROM generate_series(1, 2400) k WHERE k % 3 = 2;
CREATE INDEX fd_map_k ON fd_map (k);
VACUUM (FREEZE, ANALYZE) fd_facts, fd_map;
-- what each case asks: a statement, whether the scan runs it, the rows it differs by from the plan without the scan, and whether the lookup table gave
-- rows and met keys whose rows differ
CREATE FUNCTION fd_check(q text) RETURNS TABLE (pushed bool, differences bigint, hits bool, differ bool) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false; hits := false; differ := false;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
    IF l ~ '[1-9]\d* first row lookups' THEN hits := true; END IF;
    IF l ~ '[1-9]\d* first row keys differ' THEN differ := true; END IF;
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
CREATE TABLE fd_cases (name text, q text);
INSERT INTO fd_cases VALUES
 ('agree, one row, absent and NULL keys: the values', $q$
  SELECT f.g, count(*) AS n, count(m.v) AS nv, sum(m.v) AS sv, count(m.w) AS nw, max(m.w) AS mw
  FROM fd_facts f LEFT JOIN LATERAL (SELECT v, w FROM fd_map WHERE fd_map.k = f.k LIMIT 1) m ON true WHERE f.k IS NULL OR f.k % 3 <> 2 GROUP BY 1 $q$),
 ('inner join: the keys without a row are dropped', $q$
  SELECT f.g, count(*) AS n, sum(m.v) AS sv
  FROM fd_facts f JOIN LATERAL (SELECT v FROM fd_map WHERE fd_map.k = f.k LIMIT 1) m ON true WHERE f.k % 3 <> 2 GROUP BY 1 $q$),
 ('rows that differ: a row is there, which one is the plan''s', $q$
  SELECT f.g, count(*) AS n, count(m.v) AS nv
  FROM fd_facts f LEFT JOIN LATERAL (SELECT v, w FROM fd_map WHERE fd_map.k = f.k LIMIT 1) m ON true GROUP BY 1 $q$),
 ('rows that differ in a column the query reads too', $q$
  SELECT f.g, count(*) AS n, count(m.z) AS nz
  FROM fd_facts f LEFT JOIN LATERAL (SELECT z FROM fd_map WHERE fd_map.k = f.k LIMIT 1) m ON true WHERE f.k % 3 <> 2 GROUP BY 1 $q$),
 ('a condition on the parameters alone', $q$
  SELECT f.g, count(*) AS n, count(m.v) AS nv, sum(m.v) AS sv
  FROM fd_facts f LEFT JOIN LATERAL (SELECT v FROM fd_map WHERE fd_map.k = f.k AND f.skip IS NOT TRUE LIMIT 1) m ON true WHERE f.k IS NULL OR f.k % 3 <> 2 GROUP BY 1 $q$),
 ('a condition on the parameters alone, a condition on the row', $q$
  SELECT f.g, count(*) AS n, count(m.v) AS nv, sum(m.v) AS sv
  FROM fd_facts f LEFT JOIN LATERAL (SELECT v FROM fd_map WHERE fd_map.k = f.k AND f.g <> 2 AND fd_map.v > 1000 LIMIT 1) m ON true WHERE f.k IS NULL OR f.k % 3 <> 2 GROUP BY 1 $q$);
-- the first statements read the table when the child has run enough, the later ones from the cache of the backend
SELECT name, (fd_check(q)).* FROM fd_cases ORDER BY name;
SELECT name, (fd_check(q)).* FROM fd_cases ORDER BY name;
-- the same with workers: each reads or asks for the table by itself
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT name, (fd_check(q)).differences FROM fd_cases ORDER BY name;
-- without the cache of the backend the table is read when the child has run for enough keys, and not before
RESET max_parallel_workers_per_gather;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SELECT name, (fd_check(q)).* FROM fd_cases ORDER BY name;
DROP FUNCTION fd_check(text);
DROP TABLE fd_cases, fd_facts, fd_map;
DROP EXTENSION pg_sift;
