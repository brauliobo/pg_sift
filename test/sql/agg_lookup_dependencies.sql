-- A lookup can read columns of earlier lookups, including NULL-extended rows.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, dim_a_id int, amount int, payload text);
INSERT INTO events SELECT i, i % 100, i % 11, repeat('payload', 50) FROM generate_series(1, 50000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, amount);
CREATE TABLE dim_a (id int, code text, kind text);
INSERT INTO dim_a SELECT i, CASE WHEN i % 9 = 0 THEN NULL ELSE 'c' || i % 5 END, 'k' || i % 3 FROM generate_series(1, 95) i;
INSERT INTO dim_a VALUES (1, 'c1', 'duplicate');
CREATE TABLE labels (code text, tag text);
INSERT INTO labels VALUES ('c0', 'zero'), ('c1', 'one'), ('c1', 'duplicate'), ('c2', NULL), (NULL, 'null');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE dim_a; ANALYZE labels;
CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM
    ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
SELECT * FROM check_query($q$
 SELECT l.tag, count(*) AS n, sum(f.amount) AS total
 FROM events f JOIN dim_a e ON e.id = f.dim_a_id JOIN labels l ON l.code = e.code
 GROUP BY l.tag
$q$);
SELECT * FROM check_query($q$
 SELECT coalesce(l.tag, e.kind) AS label, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f LEFT JOIN dim_a e ON e.id = f.dim_a_id LEFT JOIN labels l ON l.code = e.code
 GROUP BY 1
$q$);
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n
 FROM events f JOIN dim_a e ON e.id = f.dim_a_id JOIN labels l ON l.code = e.code
 WHERE l.tag <> e.kind GROUP BY e.kind
$q$);
-- The deduplicated membership CTE stays materialized and is evaluated by PostgreSQL.
SELECT * FROM check_query($q$
 WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM dim_a WHERE id < 90)
 SELECT coalesce(l.tag, e.kind) AS label, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f JOIN ids p ON p.id = f.dim_a_id
 JOIN dim_a e ON e.id = p.id LEFT JOIN labels l ON l.code = e.code
 GROUP BY 1
$q$);
-- Reusing a materialized CTE must share the same statement result.
SELECT * FROM check_query($q$
 WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM dim_a WHERE id < 90)
 SELECT count(*) AS n FROM events f JOIN ids p ON p.id = f.dim_a_id
 JOIN ids q ON q.id = p.id
$q$);
-- Three lookup stages retain duplicate fanout and NULL extensions from earlier stages.
SELECT * FROM check_query($q$
 SELECT coalesce(b.tag, e.kind) AS label, count(*) AS n, count(DISTINCT f.id) AS ids,
        count(a.tag) AS labelled
 FROM events f LEFT JOIN dim_a e ON e.id = f.dim_a_id
 LEFT JOIN labels a ON a.code = e.code
 LEFT JOIN labels b ON b.tag = a.tag AND b.code = e.code
 GROUP BY GROUPING SETS ((coalesce(b.tag, e.kind)), ())
$q$);
DROP FUNCTION check_query(text);
DROP TABLE events, dim_a, labels;
DROP EXTENSION pg_sift;
