-- A flattened lateral sub-select (agg/flat.c) runs in the scan although the query also looks up the keys of its table in a CTE that reads another
-- table with a column index: those few probes are not an ordinary plan the flat plan must be set against, and the estimates of the plan as written
-- (the CTE's rows and the rows of each key) are low by two orders of magnitude. The same when the planner reads the table of that CTE as an input of a merge join: the join
-- is priced for the few rows it reads of it, not for its index scan (30,000 members are priced 997 for the scan, over a quarter of the flat plan, and the join 22).
-- Use default planner costs: the choice between the two forms is the point.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE sources (id int PRIMARY KEY, enabled bool);
INSERT INTO sources SELECT i, i <> 7 FROM generate_series(1, 20) i;
CREATE TABLE labels (foreign_id int, source_key text, label text);
INSERT INTO labels SELECT s, k, upper(k) FROM generate_series(1, 20) s, unnest(ARRAY['alpha', 'beta', 'gamma']) k WHERE (s + length(k)) % 3 <> 0;
-- the table the CTE of keys picks from: it has a column index, so the plan that reads it is an ordinary read of an indexed table (30,000 rows: only 3,000 of them have entries)
CREATE TABLE dim_b (id int PRIMARY KEY, kind text);
INSERT INTO dim_b SELECT i, CASE WHEN i % 7 = 0 THEN 'x' ELSE 'a' END FROM generate_series(1, 30000) i;
CREATE INDEX members_col ON dim_b USING sift_col (id, kind);
CREATE TABLE links (grp int, dim_b_id int);
INSERT INTO links SELECT 1, i FROM generate_series(1, 100) i;
INSERT INTO links SELECT 2, i FROM generate_series(301, 380) i;
CREATE UNIQUE INDEX links_idx ON links (grp, dim_b_id);
CREATE TABLE assigns (sub int, grp int, dim_b_id int);
INSERT INTO assigns SELECT 9, 1, NULL;
INSERT INTO assigns SELECT 9, NULL, i FROM generate_series(101, 400) i;
-- the first 100 members hold 1,200 rows each, the others 20
CREATE TABLE entries (id serial, dim_b_id int, foreign_id int, day date, metrics_a jsonb);
INSERT INTO entries (dim_b_id, foreign_id, day, metrics_a)
SELECT m.id, 1 + (m.id * 7 + g) % 20, date '2024-01-01' + (g * 37 + m.id * 11) % 900,
       jsonb_build_object('alpha', (g % 10)::float, 'beta_x', ((g * 3) % 10)::float) || CASE WHEN g % 3 = 0 THEN jsonb_build_object('gamma', (g % 7)::float) ELSE '{}' END
FROM dim_b m, LATERAL generate_series(1, CASE WHEN m.id <= 100 THEN 1200 ELSE 20 END) g WHERE m.id <= 3000;
CREATE INDEX entries_col ON entries USING sift_col (id, dim_b_id, foreign_id, day, metrics_a) WITH (key_cut = 'dim_b_id', paths = 'metrics_a.* float8');
CREATE INDEX entries_member ON entries (dim_b_id);
VACUUM (FREEZE, ANALYZE) entries;
ANALYZE dim_b; ANALYZE links; ANALYZE labels; ANALYZE sources; ANALYZE assigns;
\ir test/agg_check_pushed.sql
-- the members of a group that a subscription reaches, the entries of two periods with the labels of the unnested scores, and a spine of the periods
SELECT * FROM check_query($q$
 WITH picked AS MATERIALIZED (SELECT DISTINCT m.id FROM dim_b m
   WHERE m.id IN (SELECT coalesce(a.dim_b_id, l.dim_b_id) FROM assigns a LEFT JOIN links l ON l.grp = a.grp WHERE a.sub = 9 AND coalesce(a.dim_b_id, l.dim_b_id) IS NOT NULL)
     AND m.id IN (SELECT dim_b_id FROM links WHERE grp IN (1)) AND m.kind = 'a'),
 periods AS (SELECT * FROM (VALUES ('current', daterange('2025-06-01', '2025-06-30', '[]')), ('previous', daterange('2024-06-01', '2024-06-30', '[]'))) p(period, range)),
 parsed AS (
   SELECT e.id, p.period, s.category, CAST(s.value AS float) AS score
   FROM entries e JOIN periods p ON e.day <@ p.range JOIN picked ON picked.id = e.dim_b_id
   JOIN sources src ON src.id = e.foreign_id AND src.enabled IS TRUE
   INNER JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, r.value
                       FROM jsonb_each(e.metrics_a) r LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true)
 SELECT d.period, x.category, count(DISTINCT x.id) AS entries, round(avg(x.score)::numeric, 6) AS score
 FROM periods d LEFT JOIN parsed x ON x.period = d.period
 GROUP BY d.period, x.category
$q$);
-- the same keys without the spine, and a sum
SELECT * FROM check_query($q$
 WITH picked AS MATERIALIZED (SELECT DISTINCT m.id FROM dim_b m
   WHERE m.id IN (SELECT coalesce(a.dim_b_id, l.dim_b_id) FROM assigns a LEFT JOIN links l ON l.grp = a.grp WHERE a.sub = 9 AND coalesce(a.dim_b_id, l.dim_b_id) IS NOT NULL)
     AND m.id IN (SELECT dim_b_id FROM links WHERE grp IN (1)) AND m.kind = 'a')
 SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, count(*) AS n, sum(CAST(r.value AS float)) AS total
 FROM entries e JOIN picked ON picked.id = e.dim_b_id
 INNER JOIN LATERAL (SELECT key, value FROM jsonb_each(e.metrics_a)) r ON true
 LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key
 WHERE e.day >= '2025-06-01' AND e.day < '2025-07-01'
 GROUP BY 1
$q$);
-- the other side of the quarter (PLAIN_READER_SHARE, cte/hook.c): the same statement over so few entries that the ordinary read of the members is over a quarter of the flat plan. The two plans are then
-- weighed by their total costs, and the normal plan, cheaper for a few hundred rows, wins (not pushed); the rows are the same either way
CREATE TABLE entries_few (LIKE entries INCLUDING DEFAULTS);
INSERT INTO entries_few SELECT * FROM entries WHERE dim_b_id <= 100 AND id % 200 = 0;
CREATE INDEX entries_few_col ON entries_few USING sift_col (id, dim_b_id, foreign_id, day, metrics_a) WITH (key_cut = 'dim_b_id', paths = 'metrics_a.* float8');
CREATE INDEX entries_few_member ON entries_few (dim_b_id);
VACUUM (FREEZE, ANALYZE) entries_few;
SELECT * FROM check_query($q$
 WITH picked AS MATERIALIZED (SELECT DISTINCT m.id FROM dim_b m
   WHERE m.id IN (SELECT coalesce(a.dim_b_id, l.dim_b_id) FROM assigns a LEFT JOIN links l ON l.grp = a.grp WHERE a.sub = 9 AND coalesce(a.dim_b_id, l.dim_b_id) IS NOT NULL)
     AND m.id IN (SELECT dim_b_id FROM links WHERE grp IN (1)) AND m.kind = 'a')
 SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, count(*) AS n, sum(CAST(r.value AS float)) AS total
 FROM entries_few e JOIN picked ON picked.id = e.dim_b_id
 INNER JOIN LATERAL (SELECT key, value FROM jsonb_each(e.metrics_a)) r ON true
 LEFT JOIN labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE entries_few, entries, assigns, links, dim_b, labels, sources;
DROP EXTENSION pg_sift;
