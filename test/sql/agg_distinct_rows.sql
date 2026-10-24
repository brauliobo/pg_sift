-- count(DISTINCT x) over the elements of a list, where x is the primary key of the scanned table (a NOT NULL column under a unique single-column
-- index), counts the rows of each group that have an element in it with a marker per group (agg/update.c, upd_count_rows): the elements of a row are
-- contiguous and the rows of a unit are its own, so no set of values is kept and the counts add up across units, workers and spilled tables. A grouping
-- set over a preserved join hands the column as a placeholder (NULL where the join nulls it, which DISTINCT does not count): the same path. A column
-- that is not proven unique (a duplicated column, a nullable unique column, a column of a two-column unique index) keeps the set, and
-- sift.explain_why says so (distinct_set).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, alt_id int, day date NOT NULL, metrics_a jsonb, parts jsonb, pad text);
INSERT INTO facts
SELECT i, i % 37, CASE WHEN i % 5 = 0 THEN NULL ELSE i END, date '2026-01-01' + i % 400,
       CASE WHEN i % 23 = 0 THEN NULL
            ELSE jsonb_build_object('mhead', i % 10 + 1, 'service_staff', (i * 7) % 10 + 1) || CASE WHEN i % 3 = 0 THEN jsonb_build_object('value', (i * 3) % 10 + 1) ELSE '{}' END END,
       CASE WHEN i % 29 = 0 THEN NULL
            ELSE jsonb_build_array(jsonb_build_object('cat', 'c' || i % 4, 'score', i % 10), jsonb_build_object('cat', 'c' || i % 4, 'score', (i * 3) % 10),
                                   jsonb_build_object('cat', 'c' || (i + 1) % 4, 'score', CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 7) % 10 END)) END,
       repeat('x', 40)
FROM generate_series(1, 20000) i;
CREATE UNIQUE INDEX facts_alt ON facts (alt_id);
CREATE UNIQUE INDEX facts_pair ON facts (dim_a_id, id);
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, alt_id, day, metrics_a, parts) WITH (paths = 'metrics_a.* float8, parts[]{cat text, score float8}');
VACUUM (FREEZE, ANALYZE) facts;
\ir test/agg_check_query.sql

-- the aggregates the scan computes (a rows count shows as `count`, a set as `count(DISTINCT col)`), and the counters of what the group table did
CREATE FUNCTION pushed(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || q LOOP
    IF l LIKE '%Pushed Aggregates:%' THEN RETURN btrim(l); END IF;
  END LOOP;
  RETURN 'no SiftAgg';
END $$;
CREATE FUNCTION counters(q text) RETURNS TABLE (spills int, sets_spilled int, workers int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  spills := 0; sets_spilled := 0; workers := 0;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l LIKE '%Group Table Spills:%' THEN spills := spills + substring(l FROM 'Spills: (\d+)')::int; END IF;
    IF l LIKE '%Distinct Sets Spilled:%' THEN sets_spilled := sets_spilled + substring(l FROM 'Spilled: (\d+)')::int; END IF;
    IF l LIKE '%Workers Launched:%' THEN workers := workers + substring(l FROM 'Launched: (\d+)')::int; END IF;
  END LOOP;
  RETURN NEXT;
END $$;

-- the keys of a map: a row holds each key once, so the rows of a key are its elements
SELECT pushed($q$ SELECT r.key, count(DISTINCT f.id) AS rows_, count(*) AS elements FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 $q$);
SELECT * FROM check_query($q$ SELECT r.key, count(DISTINCT f.id) AS rows_, count(*) AS elements FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 ORDER BY 1 $q$);
SELECT r.key, count(DISTINCT f.id) AS rows_, count(*) AS elements FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 ORDER BY 1;

-- the elements of an array: two elements of a row in one category count the row once, a row no element of which passes the FILTER is not counted
SELECT pushed($q$ SELECT p.cat, count(DISTINCT f.id) AS rows_, count(DISTINCT f.id) FILTER (WHERE p.score >= 8) AS high, count(*) AS elements
                  FROM facts f, jsonb_to_recordset(f.parts) p(cat text, score float8) GROUP BY 1 $q$);
SELECT * FROM check_query($q$ SELECT p.cat, count(DISTINCT f.id) AS rows_, count(DISTINCT f.id) FILTER (WHERE p.score >= 8) AS high, count(*) AS elements
                              FROM facts f, jsonb_to_recordset(f.parts) p(cat text, score float8) GROUP BY 1 ORDER BY 1 $q$);
SELECT p.cat, count(DISTINCT f.id) AS rows_, count(DISTINCT f.id) FILTER (WHERE p.score >= 8) AS high, count(*) AS elements
FROM facts f, jsonb_to_recordset(f.parts) p(cat text, score float8) GROUP BY 1 ORDER BY 1;

-- the report shape: periods as a spine (one matches nothing), the parsed map joined from the left, grouping sets, the buckets as filtered distinct
-- counts, a distinct count of a duplicated column beside them; the planner hands the key as a placeholder under the grouping sets
SELECT pushed($q$
 WITH periods(period, range) AS (VALUES ('current', daterange('2026-07-01', '2027-02-04', '[]')), ('previous', daterange('2026-01-01', '2026-06-30', '[]')), ('none', daterange('2030-01-01', '2030-12-31', '[]'))),
      parsed AS (SELECT f.id, f.dim_a_id, f.day, split_part(r.key, '_', 1) AS category, nullif(split_part(r.key, '_', 2), '') AS sub, (r.value #>> '{}')::float8 AS value
                 FROM facts f, jsonb_each(f.metrics_a) r)
 SELECT p.period, category, sub, count(DISTINCT id) AS n, count(DISTINCT id) FILTER (WHERE value >= 8) AS high, count(DISTINCT id) FILTER (WHERE value <= 6) AS low,
        count(DISTINCT dim_a_id) AS dim_a, avg(value) AS value
 FROM periods p LEFT JOIN parsed x ON x.day <@ p.range
 GROUP BY GROUPING SETS ((p.period, category, sub), (p.period, category), (p.period)) $q$);
SELECT * FROM check_query($q$
 WITH periods(period, range) AS (VALUES ('current', daterange('2026-07-01', '2027-02-04', '[]')), ('previous', daterange('2026-01-01', '2026-06-30', '[]')), ('none', daterange('2030-01-01', '2030-12-31', '[]'))),
      parsed AS (SELECT f.id, f.dim_a_id, f.day, split_part(r.key, '_', 1) AS category, nullif(split_part(r.key, '_', 2), '') AS sub, (r.value #>> '{}')::float8 AS value
                 FROM facts f, jsonb_each(f.metrics_a) r)
 SELECT p.period, category, sub, count(DISTINCT id) AS n, count(DISTINCT id) FILTER (WHERE value >= 8) AS high, count(DISTINCT id) FILTER (WHERE value <= 6) AS low,
        count(DISTINCT dim_a_id) AS dim_a, round(avg(value)::numeric, 6) AS value
 FROM periods p LEFT JOIN parsed x ON x.day <@ p.range
 GROUP BY GROUPING SETS ((p.period, category, sub), (p.period, category), (p.period)) ORDER BY 1, 2, 3 $q$);
WITH periods(period, range) AS (VALUES ('current', daterange('2026-07-01', '2027-02-04', '[]')), ('previous', daterange('2026-01-01', '2026-06-30', '[]')), ('none', daterange('2030-01-01', '2030-12-31', '[]'))),
     parsed AS (SELECT f.id, f.dim_a_id, f.day, split_part(r.key, '_', 1) AS category, nullif(split_part(r.key, '_', 2), '') AS sub, (r.value #>> '{}')::float8 AS value
                FROM facts f, jsonb_each(f.metrics_a) r)
SELECT p.period, category, sub, count(DISTINCT id) AS n, count(DISTINCT id) FILTER (WHERE value >= 8) AS high, count(DISTINCT id) FILTER (WHERE value <= 6) AS low,
       count(DISTINCT dim_a_id) AS dim_a
FROM periods p LEFT JOIN parsed x ON x.day <@ p.range
GROUP BY GROUPING SETS ((p.period, category, sub), (p.period, category), (p.period)) ORDER BY 1, 2, 3;

-- not proven unique per row: a duplicated column, a nullable unique column, a column of a two-column unique index keep the set, and say why
SELECT pushed($q$ SELECT r.key, count(DISTINCT f.dim_a_id) AS d FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 $q$);
SELECT pushed($q$ SELECT r.key, count(DISTINCT f.alt_id) AS d FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 $q$);
SELECT DISTINCT slug, reason FROM sift.explain_why($q$ SELECT r.key, count(DISTINCT f.dim_a_id) AS d FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 $q$) WHERE slug = 'distinct_set';
SELECT * FROM check_query($q$ SELECT r.key, count(DISTINCT f.dim_a_id) AS d, count(DISTINCT f.alt_id) AS a FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 ORDER BY 1 $q$);
SELECT r.key, count(DISTINCT f.dim_a_id) AS d, count(DISTINCT f.alt_id) AS a FROM facts f, jsonb_each(f.metrics_a) r GROUP BY 1 ORDER BY 1;

-- the group table spills (hash memory at its smallest): the counts of the spilled tables add up, and no set of values is written
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT * FROM counters($q$ SELECT f.id % 5000 AS g, p.cat, count(DISTINCT f.id) AS rows_, count(*) AS elements FROM facts f, jsonb_to_recordset(f.parts) p(cat text, score float8) GROUP BY 1, 2 $q$);
SELECT * FROM check_query($q$ SELECT f.id % 5000 AS g, p.cat, count(DISTINCT f.id) AS rows_, count(*) AS elements FROM facts f, jsonb_to_recordset(f.parts) p(cat text, score float8) GROUP BY 1, 2 ORDER BY 1, 2 $q$);
RESET work_mem;
RESET hash_mem_multiplier;

-- parallel workers: each counts the rows of its units, the leader adds the counts
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT spills, sets_spilled, workers > 0 AS parallel FROM counters($q$ SELECT p.cat, count(DISTINCT f.id) AS rows_, count(DISTINCT f.id) FILTER (WHERE p.score >= 8) AS high, count(*) AS elements
                                                              FROM facts f, jsonb_to_recordset(f.parts) p(cat text, score float8) GROUP BY 1 $q$);
SELECT * FROM check_query($q$ SELECT p.cat, count(DISTINCT f.id) AS rows_, count(DISTINCT f.id) FILTER (WHERE p.score >= 8) AS high, count(*) AS elements
                              FROM facts f, jsonb_to_recordset(f.parts) p(cat text, score float8) GROUP BY 1 ORDER BY 1 $q$);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;

DROP FUNCTION pushed(text);
DROP FUNCTION counters(text);
DROP FUNCTION check_query(text);
DROP TABLE facts;
DROP EXTENSION pg_sift;
