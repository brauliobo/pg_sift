-- A joined UNION ALL of sub-selects is a plan child: its rows are PostgreSQL's plan of the arms, read once when the scan starts.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dates (day date PRIMARY KEY);
INSERT INTO dates SELECT d::date FROM generate_series('2026-01-01'::date, '2026-01-20', '1 day') d;
CREATE TABLE facts (id int, entity_id int, day date, amount float8, payload text);
INSERT INTO facts
SELECT i, i % 40, '2026-01-01'::date + (i % 15), CASE WHEN i % 9 = 0 THEN NULL ELSE i % 11 END, repeat('payload', 50)
FROM generate_series(1, 60000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, entity_id, day, amount);
CREATE TABLE left_names (id int, name text);
INSERT INTO left_names SELECT i, 'n' || (i % 6) FROM generate_series(0, 19) i;
CREATE TABLE right_names (id int, name text);
INSERT INTO right_names SELECT i, 'm' || (i % 4) FROM generate_series(15, 39) i;
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dates;
ANALYZE left_names;
ANALYZE right_names;
\ir test/agg_check_query.sql
-- The names of both tables for an entity: the same id may be in both arms, in one, or in none.
SELECT * FROM check_query($q$
 SELECT n.name, count(*) AS n, sum(f.amount) AS total
 FROM facts f JOIN (SELECT id, name FROM left_names UNION ALL SELECT id, name FROM right_names) n ON n.id = f.entity_id
 GROUP BY 1
$q$);
-- Rows without a name stay with the outer join.
SELECT * FROM check_query($q$
 SELECT n.name, count(*) AS n, count(f.amount) AS amounts
 FROM facts f LEFT JOIN (SELECT id, name FROM left_names UNION ALL SELECT id, name FROM right_names) n ON n.id = f.entity_id
 GROUP BY 1
$q$);
-- A UNION removes the arms' duplicate rows before the join.
SELECT * FROM check_query($q$
 SELECT n.name, count(*) AS n
 FROM facts f JOIN (SELECT id, name FROM left_names UNION SELECT id, name FROM right_names) n ON n.id = f.entity_id
 GROUP BY 1
$q$);
-- Stays native: the calendar's dates are preserved by a join before the names, and the scan keeps its preserved rows for the last join only.
SELECT * FROM check_query($q$
 SELECT d.day, n.name, count(f.id) AS n
 FROM dates d LEFT JOIN facts f ON f.day = d.day
 LEFT JOIN (SELECT id, name FROM left_names UNION ALL SELECT id, name FROM right_names) n ON n.id = f.entity_id
 GROUP BY 1, 2
$q$);
-- A column that only a condition read stays out of the UNION ALL's plan (an Append outputs the columns the joins and the groups need): the normal plan stays.
SELECT * FROM check_query($q$
 SELECT count(*) AS n, sum(f.amount) AS total
 FROM facts f JOIN (SELECT id, name FROM left_names UNION ALL SELECT id, name FROM right_names) n ON n.id = f.entity_id
 WHERE n.name <> 'n1'
$q$);
SELECT * FROM check_query($q$
 SELECT count(*) AS n, count(f.amount) AS amounts
 FROM facts f LEFT JOIN (SELECT id, name FROM left_names UNION ALL SELECT id, name FROM right_names) n ON n.id = f.entity_id AND n.name = 'n1'
$q$);
SELECT * FROM check_query($q$
 SELECT n.id, count(*) AS n
 FROM facts f LEFT JOIN (SELECT id, name FROM left_names UNION ALL SELECT id, name FROM right_names) n ON n.id = f.entity_id AND n.name = 'm1'
 GROUP BY 1
$q$);
-- The condition's column is a group key: the Append outputs it, the scan runs.
SELECT * FROM check_query($q$
 SELECT n.name, count(*) AS n
 FROM facts f JOIN (SELECT id, name FROM left_names UNION ALL SELECT id, name FROM right_names) n ON n.id = f.entity_id
 WHERE n.name <> 'n1'
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts, dates, left_names, right_names;
DROP EXTENSION pg_sift;
