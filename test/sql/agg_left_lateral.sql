-- LEFT JOIN LATERAL over the list of a row (jsonb_to_recordset, jsonb_array_elements), ON true or with conditions of its own: a row none of whose
-- elements passes is one tuple with NULL element columns (sift.agg_left_lateral). Every query runs with and without the scan: the rows must be the
-- same, and the scan answers the query. Rows with no list, an empty list, a list whose elements all fail the lateral's conditions, NULL members,
-- and rows the snapshot does not see (deleted, updated) are in the table; groups by the row id and by a column several rows share.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.agg_left_lateral = on;

CREATE TABLE items (id int PRIMARY KEY, parent_id int, grp text, weight float8, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO items
SELECT i, i % 40, CASE WHEN i % 11 = 0 THEN NULL ELSE 'g' || i % 5 END, CASE WHEN i % 13 = 0 THEN NULL ELSE (i % 10)::float8 END,
       CASE WHEN i % 17 = 0 THEN NULL                                  -- no document
            WHEN i % 7 = 0 THEN '{"parts": []}'::jsonb                   -- an empty list
            WHEN i % 19 = 0 THEN '{"other": 1}'::jsonb                   -- no list
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3],
                                                                                  'n', CASE WHEN (i + j) % 6 = 0 THEN NULL ELSE (i * j) % 10 END,
                                                                                  'tag', CASE WHEN (i + j) % 5 = 0 THEN NULL ELSE 't' || (i + j) % 4 END))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 4000) i;
CREATE INDEX items_col ON items USING sift_col (id, parent_id, grp, weight, doc) WITH (paths = 'doc.parts[]{k text, n int4, tag text}');
VACUUM (FREEZE, ANALYZE) items;
-- rows the snapshot of the scan does not see: deleted and updated after the vacuum
DELETE FROM items WHERE id % 53 = 0;
UPDATE items SET weight = weight + 1 WHERE id % 59 = 0;

-- whether the scan answers the query, and the rows it differs by from the plan without it
CREATE FUNCTION check_same(q text) RETURNS TABLE (pushed bool, rows bigint, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  SELECT count(*) INTO rows FROM r_on;
  pushed := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;

-- per row: the weight of the row, the average and count of its elements (a row with none keeps its group)
SELECT * FROM check_same($q$SELECT i.id, max(i.weight) AS weight, avg(p.n) AS n, count(*) AS tuples, count(p.n) AS counted
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true GROUP BY i.id$q$);
-- the lateral's own conditions say which elements exist: a row whose elements all fail them is one NULL tuple
SELECT * FROM check_same($q$SELECT i.id, max(i.weight) AS weight, avg(p.n) AS n, count(*) AS tuples
  FROM items i LEFT JOIN LATERAL (SELECT * FROM jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) WHERE p.n IS NOT NULL AND p.n > 5) p ON true GROUP BY i.id$q$);
SELECT * FROM check_same($q$SELECT i.id, count(*) AS tuples, count(p.k) AS ks
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON p.k = 'a' AND p.n IS NOT NULL GROUP BY i.id$q$);
-- a group of several rows: NULL tuples and elements mix in a group
SELECT * FROM check_same($q$SELECT i.parent_id, count(*) AS tuples, count(DISTINCT i.id) AS ids, sum(p.n) AS total, max(p.tag) AS tag
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true GROUP BY i.parent_id$q$);
SELECT * FROM check_same($q$SELECT i.grp, p.k, count(*) AS tuples, sum(p.n) AS total, count(DISTINCT i.id) AS ids
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true GROUP BY i.grp, p.k$q$);
-- a condition on the rows, an aggregate FILTER, no group
SELECT * FROM check_same($q$SELECT count(*) AS tuples, avg(p.n) AS n, count(*) FILTER (WHERE p.k = 'b') AS bs, count(*) FILTER (WHERE p.k IS NULL) AS none
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true WHERE i.parent_id < 20 AND i.weight IS NOT NULL$q$);
-- jsonb_array_elements
SELECT * FROM check_same($q$SELECT i.id, count(*) AS tuples, count(e.value ->> 'k') AS elems
  FROM items i LEFT JOIN LATERAL jsonb_array_elements(i.doc -> 'parts') AS e(value) ON true GROUP BY i.id$q$);
-- the outer aggregate over the per-row groups
SELECT * FROM check_same($q$SELECT s.parent_id, count(s.id) AS n, avg(s.weight) AS weight, avg(s.score) AS score FROM
  (SELECT i.id, i.parent_id, max(i.weight) AS weight, avg(p.n) AS score
   FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true GROUP BY i.id, i.parent_id) s GROUP BY s.parent_id$q$);

-- the elements joined to a lookup inside the lateral: a row with no element is one NULL tuple, a placeholder that defaults a missing match reads NULL there
CREATE TABLE kinds (k text PRIMARY KEY, flag_a text);
INSERT INTO kinds VALUES ('a', 'x'), ('b', 'y');
SELECT * FROM check_same($q$SELECT i.id, count(*) AS tuples, avg(m.n) AS n, avg(m.n) FILTER (WHERE m.c = 'x') AS xs, count(m.c) AS cs
  FROM items i LEFT JOIN LATERAL (SELECT coalesce(kk.flag_a, 'none') AS c, p.n
    FROM jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) LEFT JOIN LATERAL (SELECT flag_a FROM kinds WHERE kinds.k = p.k LIMIT 1) kk ON true
    WHERE p.n IS NOT NULL) m ON true GROUP BY i.id$q$);
SELECT * FROM check_same($q$SELECT s.parent_id, count(s.id) AS n, round(avg(s.score)::numeric, 6) AS score, round(avg(s.xs)::numeric, 6) AS xs FROM
  (SELECT i.id, i.parent_id, avg(m.n) AS score, avg(m.n) FILTER (WHERE m.c IN ('x', 'none')) AS xs
   FROM items i LEFT JOIN LATERAL (SELECT coalesce(kk.flag_a, 'none') AS c, p.n
     FROM jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) LEFT JOIN LATERAL (SELECT flag_a FROM kinds WHERE kinds.k = p.k LIMIT 1) kk ON true
     WHERE p.n IS NOT NULL) m ON true GROUP BY i.id, i.parent_id) s GROUP BY s.parent_id$q$);
SELECT * FROM check_same($q$SELECT m.c, count(*) AS tuples, count(DISTINCT i.id) AS ids
  FROM items i LEFT JOIN LATERAL (SELECT coalesce(kk.flag_a, 'none') AS c
    FROM jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) LEFT JOIN LATERAL (SELECT flag_a FROM kinds WHERE kinds.k = p.k LIMIT 1) kk ON true) m ON true GROUP BY m.c$q$);
DROP TABLE kinds;

-- a condition above the join on an element column that is not strict sees the NULL tuple: the normal plan (equal rows)
SELECT * FROM check_same($q$SELECT i.parent_id, count(*) AS tuples
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true WHERE p.n IS NULL GROUP BY i.parent_id$q$);
-- a strict one makes it an inner join, which was pushed down before: rows without elements are gone
SELECT * FROM check_same($q$SELECT i.parent_id, count(*) AS tuples
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true WHERE p.n > 3 GROUP BY i.parent_id$q$);

-- the setting off keeps the normal plan
SET sift.agg_left_lateral = off;
SELECT * FROM check_same($q$SELECT i.id, max(i.weight) AS weight, avg(p.n) AS n
  FROM items i LEFT JOIN LATERAL jsonb_to_recordset(i.doc -> 'parts') AS p(k text, n int, tag text) ON true GROUP BY i.id$q$);

DROP TABLE items;
DROP FUNCTION check_same(text);
DROP EXTENSION pg_sift;
