-- A lateral sub-select that PostgreSQL leaves in place on the nullable side of an outer join (a CTE read by `dates LEFT JOIN cte`) is pulled up
-- inside its own query level, so that the scan reads the unnested document and the mapping table instead of a correlated child.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE sources (id int PRIMARY KEY, enabled bool);
INSERT INTO sources VALUES (1, true), (2, true), (3, false);
CREATE TABLE labels (foreign_id int, source_key text, label text, sublabel text);
INSERT INTO labels VALUES (1, 'catga_score', 'catg_b', NULL), (1, 'catgm_score', 'catg_f', 'sub_a'), (2, 'catga_score', 'tidy', NULL), (2, 'catga_score', 'tidy2', NULL);
CREATE TABLE entries (id int, foreign_id int, day date, metrics_a jsonb);
INSERT INTO entries
SELECT i, 1 + i % 4, date '2026-01-01' + i % 20,
       CASE i % 7 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN NULL WHEN 2 THEN '{"catga_score": 9, "catgm_score": 4}'::jsonb
                  WHEN 3 THEN '{"catga_score": 6.5, "other_score": 2, "catgj": 5}'::jsonb
                  ELSE jsonb_build_object('catga_score', i % 10, 'catgk_score', i % 7) END
FROM generate_series(1, 20000) i;
CREATE INDEX entries_col ON entries USING sift_col (id, foreign_id, day, metrics_a) WITH (paths = 'metrics_a.* float8');
VACUUM (FREEZE, ANALYZE) entries;
ANALYZE sources; ANALYZE labels;
-- The result, or the SQLSTATE and message, with the scan off and on. on: the table scans are off, so a Custom Scan (SiftAgg) runs when the query is one it can run.
\ir test/agg_helpers.sql
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), left(outcome(q, true), 100);
END $$;
-- the mapping table joined to the unnested members, below a date spine's outer join (no condition of the left join reads a table outside it)
SELECT * FROM check_query('mapping below an outer join', $q$
 WITH parsed AS (
   SELECT e.id, e.day, s.category, s.subcategory, CAST(s.value AS float) AS score
   FROM entries e
   INNER JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, coalesce(l.sublabel, nullif(split_part(r.key, '_', 2), '')) AS subcategory, r.value
                       FROM jsonb_each(e.metrics_a) r LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true)
 SELECT d.day, p.category, count(DISTINCT p.id) AS entries, avg(p.score) AS score
 FROM (SELECT DISTINCT day FROM entries) d LEFT JOIN parsed p ON p.day = d.day
 GROUP BY d.day, p.category
$q$);
-- the same with an equality of the source with a joined table (an equivalence class that reaches into the mapping join)
SELECT * FROM check_query('equality with a joined table', $q$
 WITH parsed AS (
   SELECT e.id, e.day, s.category, CAST(s.value AS float) AS score
   FROM entries e INNER JOIN sources src ON src.id = e.foreign_id AND src.enabled IS TRUE
   INNER JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, r.value
                       FROM jsonb_each(e.metrics_a) r LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true)
 SELECT d.day, p.category, count(DISTINCT p.id) AS entries, avg(p.score) AS score
 FROM (SELECT DISTINCT day FROM entries) d LEFT JOIN parsed p ON p.day = d.day
 GROUP BY d.day, p.category
$q$);
-- a sub-select with no join of its own, and one that reads two columns of the row
SELECT * FROM check_query('plain lateral', $q$
 WITH parsed AS (
   SELECT e.id, e.day, s.category, CAST(s.value AS float) AS score
   FROM entries e INNER JOIN LATERAL (SELECT split_part(key, '_', 1) AS category, value FROM jsonb_each(e.metrics_a) WHERE key <> 'catgj') s ON true)
 SELECT d.day, p.category, count(*) AS n, sum(p.score) AS total
 FROM (SELECT DISTINCT day FROM entries) d LEFT JOIN parsed p ON p.day = d.day
 GROUP BY d.day, p.category
$q$);
-- without an enclosing outer join PostgreSQL plans the same query itself
SELECT * FROM check_query('no outer join', $q$
 SELECT s.category, count(*) AS n, sum(CAST(s.value AS float)) AS total
 FROM entries e INNER JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, r.value
                                     FROM jsonb_each(e.metrics_a) r LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true
 GROUP BY 1
$q$);
-- on the nullable side of a right join the columns of a sub-select are NULL for the rows the join does not match: not pulled up
SELECT * FROM check_query('right join', $q$
 SELECT s.category, z.id, count(e.id) AS n
 FROM (entries e INNER JOIN LATERAL (SELECT 'c'::text AS category FROM jsonb_each(e.metrics_a) r) s ON true)
 RIGHT JOIN sources z ON z.id = e.foreign_id + 10
 GROUP BY 1, 2
$q$);
-- a sub-select that reads the columns of an earlier one
SELECT * FROM check_query('chained', $q$
 WITH parsed AS (
   SELECT e.id, e.day, s.category, s2.k2
   FROM entries e
   JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, r.value
                 FROM jsonb_each(e.metrics_a) r LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true
   JOIN LATERAL (SELECT s.category || 'x' AS k2 FROM sources z WHERE z.id = e.foreign_id) s2 ON true)
 SELECT d.day, p.category, p.k2, count(DISTINCT p.id) AS n
 FROM (SELECT DISTINCT day FROM entries) d LEFT JOIN parsed p ON p.day = d.day
 GROUP BY 1, 2, 3
$q$);
-- A sub-select with a LIMIT is not pulled up: it stays a correlated child, with the same results.
SELECT * FROM check_query('limit', $q$
 WITH parsed AS (
   SELECT e.id, e.day, s.label FROM entries e
   INNER JOIN LATERAL (SELECT l.label FROM labels l WHERE l.foreign_id = e.foreign_id ORDER BY l.label LIMIT 1) s ON true)
 SELECT d.day, p.label, count(*) FROM (SELECT DISTINCT day FROM entries) d LEFT JOIN parsed p ON p.day = d.day GROUP BY 1, 2
$q$);
-- Error timing: the cast of a member runs for the members of the rows that reach it, with PostgreSQL's SQLSTATE and message.
INSERT INTO entries VALUES (20001, 1, '2026-02-01', '{"catga_score": 3000000000}'), (20002, 2, '2026-02-02', '{"catga_score": 1}');
VACUUM (FREEZE, ANALYZE) entries;
SELECT * FROM check_query('cast of an excluded row', $q$
 WITH parsed AS (
   SELECT e.id, e.day, s.category, CAST(s.value AS integer) AS score FROM entries e
   INNER JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, r.value
                       FROM jsonb_each(e.metrics_a) r LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true
   WHERE e.id <> 20001)
 SELECT d.day, p.category, count(*) AS n, sum(p.score) AS total FROM (SELECT DISTINCT day FROM entries) d LEFT JOIN parsed p ON p.day = d.day GROUP BY 1, 2
$q$);
SELECT * FROM check_query('cast out of range', $q$
 WITH parsed AS (
   SELECT e.id, e.day, s.category, CAST(s.value AS integer) AS score FROM entries e
   INNER JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, r.value
                       FROM jsonb_each(e.metrics_a) r LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true)
 SELECT d.day, p.category, count(*) AS n, sum(p.score) AS total FROM (SELECT DISTINCT day FROM entries) d LEFT JOIN parsed p ON p.day = d.day GROUP BY 1, 2
$q$);
DROP FUNCTION check_query(text, text), outcome(text, bool), runs_in_scan(text);
DROP TABLE entries, labels, sources;
DROP EXTENSION pg_sift;
