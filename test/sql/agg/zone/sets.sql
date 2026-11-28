-- Two sets of keys on one leaf (an IN list or a dimension filter, and the keys of a lookup join): a unit is read only when its zone holds a value of
-- both. The zone test used to keep the first set and the range of the two, so a wide list in front of a narrow set read the units of the wide one.
-- Every query runs with and without the scan: the rows must be the same.
CREATE EXTENSION pg_sift;
SET enable_indexscan = off; SET enable_indexonlyscan = off; SET enable_bitmapscan = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

-- events stored in entity order, 20 per entity: a unit of about 12000 rows holds 600 entities (units: 1-600, 601-1200, 1201-1800, 1801-2400, 2401-3000)
CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + (i - 1) / 20 END, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) || md5((i + 3)::text)
FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, amount, pad);
CREATE TABLE ents (id int PRIMARY KEY, grp int);
INSERT INTO ents SELECT i, i % 3 FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) ev, ents;

-- the granules a query reads (Granules: scanned=N; none read when the line is missing), whether the scan answered it, and the rows it differs by from the plan without the scan
CREATE FUNCTION granules(q text, pushed OUT bool, n OUT int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  pushed := false;
  n := 0;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg%';
    n := n + coalesce(substring(l from 'Granules: scanned=(\d+)')::int, 0);
  END LOOP;
END $$;
-- `units` compares the granules of q with those of `narrow`, the list of the values the sets have in common: the same when the zone test intersects the sets
CREATE FUNCTION check_units(q text, narrow text) RETURNS TABLE (pushed bool, units text, differences bigint) LANGUAGE plpgsql AS $$
DECLARE g int; ng int;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  SELECT r.pushed, r.n INTO pushed, g FROM granules(q) r;
  ng := CASE WHEN narrow IS NULL THEN 0 ELSE (SELECT r.n FROM granules('SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (' || narrow || ')') r) END;
  units := CASE WHEN g = ng THEN 'narrow' WHEN g > ng THEN 'more' ELSE 'fewer' END;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;

-- a list with gaps alone: the units of 10 and 2900. A set with a value in every unit (10, 710, 1410, 2110, 2810) in front of it or behind it: only 10 is in both
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (10, 2900)$q$, '10, 2900');
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (SELECT g FROM generate_series(10, 2810, 700) g) AND dim_a_id IN (10, 2900)$q$, '10');
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (10, 2900) AND dim_a_id IN (SELECT g FROM generate_series(10, 2810, 700) g)$q$, '10');
SELECT * FROM check_units($q$SELECT dim_a_id, count(*), sum(amount) FROM ev WHERE dim_a_id IN (SELECT g FROM generate_series(10, 2810, 700) g) AND dim_a_id IN (10, 2900) GROUP BY dim_a_id$q$, '10');
-- a list and the keys of a lookup join (300, 600, .. 1800): 300 and 1200 are in both, in the units 1 and 2 of the 5
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (300, 1200, 2700, 2901)$q$, '300, 1200, 2700, 2901');
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (300, 1200, 2700, 2901) AND dim_a_id IN (SELECT id FROM ents WHERE grp = 0 AND id % 100 = 0 AND id <= 1800)$q$, '300, 1200');
-- a range and two sets
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (SELECT g FROM generate_series(10, 2810, 700) g) AND dim_a_id > 1000 AND dim_a_id IN (10, 900, 1410, 2900)$q$, '1410');
-- sets with no value in common: no unit is read
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (1, 2, 3) AND dim_a_id IN (1500, 1600)$q$, NULL);
SELECT * FROM check_units($q$SELECT count(*), sum(amount) FROM ev WHERE dim_a_id IN (SELECT id FROM ents WHERE grp = 1 AND id < 100) AND dim_a_id IN (2, 5, 8)$q$, NULL);

DROP TABLE ev, ents;
DROP FUNCTION check_units(text, text), granules(text);
DROP EXTENSION pg_sift;
