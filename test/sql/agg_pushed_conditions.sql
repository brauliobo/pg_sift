-- A condition over the columns of a joined sub-select that PostgreSQL pushed down into it (the run condition of a ranking window) is enforced by the plan
-- of the sub-select, which then does not output the column: the scan joins its rows without testing the condition again.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE entities (id int PRIMARY KEY, kind text);
INSERT INTO entities SELECT i, (ARRAY['a', 'b', 'c'])[1 + i % 3] FROM generate_series(1, 90) i;
CREATE TABLE facts (id int, day date, entity_id int, deleted_at timestamp);
INSERT INTO facts SELECT i, '2026-01-01'::date + (i % 15), 1 + (i * i) % 97, CASE WHEN i % 17 = 0 THEN now() END FROM generate_series(1, 40000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, day, entity_id, deleted_at);
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE entities;
\ir test/agg_check_query.sql
-- The entities of the top three of their kind: the semi join to the ranked sub-select, whose `rn <= 3` the window evaluates itself.
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n
 FROM facts f JOIN entities e ON e.id = f.entity_id
 WHERE f.entity_id IN (SELECT entity_id FROM (
   SELECT entity_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, entity_id) AS rn
   FROM (SELECT f2.entity_id, e2.kind, count(*) AS n FROM facts f2 JOIN entities e2 ON e2.id = f2.entity_id GROUP BY 1, 2) c) ranked
   WHERE rn <= 3)
 GROUP BY 1
$q$);
-- The same as an inner join, and as a LEFT JOIN whose ON clause holds the condition.
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n
 FROM facts f JOIN entities e ON e.id = f.entity_id
 JOIN (SELECT entity_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, entity_id) AS rn
       FROM (SELECT f2.entity_id, e2.kind, count(*) AS n FROM facts f2 JOIN entities e2 ON e2.id = f2.entity_id GROUP BY 1, 2) c) ranked
   ON ranked.entity_id = f.entity_id AND ranked.rn <= 2
 GROUP BY 1
$q$);
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n, count(ranked.entity_id) AS ranked_rows
 FROM facts f JOIN entities e ON e.id = f.entity_id
 LEFT JOIN (SELECT entity_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, entity_id) AS rn
       FROM (SELECT f2.entity_id, e2.kind, count(*) AS n FROM facts f2 JOIN entities e2 ON e2.id = f2.entity_id GROUP BY 1, 2) c) ranked
   ON ranked.entity_id = f.entity_id AND ranked.rn <= 2
 GROUP BY 1
$q$);
-- The rank is read by the query too, so its column stays in the plan's output.
SELECT * FROM check_query($q$
 SELECT ranked.rn, count(*) AS n
 FROM facts f JOIN (SELECT entity_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, entity_id) AS rn
       FROM (SELECT f2.entity_id, e2.kind, count(*) AS n FROM facts f2 JOIN entities e2 ON e2.id = f2.entity_id GROUP BY 1, 2) c) ranked
   ON ranked.entity_id = f.entity_id
 WHERE ranked.rn <= 3
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts, entities;
DROP EXTENSION pg_sift;
