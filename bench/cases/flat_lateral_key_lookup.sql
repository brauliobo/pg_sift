-- the unnested scores of the entries of the members one subscription reaches, two periods against each other: a lateral sub-select the planner flattens, and a CTE of keys that reads another table with a column index
-- label: `entries` of 100 members of 3,000 (a CTE of keys from `members`, 30,000 rows), two months against two months, 500,000 rows; the scores unnested and labelled
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('fk_entries') IS NULL THEN
		CREATE TABLE fk_sources AS SELECT i AS id, i <> 7 AS enabled FROM generate_series(1, 20) i;
		ALTER TABLE fk_sources ADD PRIMARY KEY (id);
		CREATE TABLE fk_labels AS SELECT s AS foreign_id, k AS source_key, upper(k) AS label FROM generate_series(1, 20) s, unnest(ARRAY['alpha', 'beta', 'gamma']) k WHERE (s + length(k)) % 3 <> 0;
		CREATE TABLE fk_members AS SELECT i AS id, CASE WHEN i % 7 = 0 THEN 'x' ELSE 'a' END AS kind FROM generate_series(1, 30000) i;
		ALTER TABLE fk_members ADD PRIMARY KEY (id);
		CREATE INDEX fk_members_col ON fk_members USING sift_col (id, kind);
		CREATE TABLE fk_links AS SELECT 1 AS grp, i AS dim_b_id FROM generate_series(1, 100) i UNION ALL SELECT 2, i FROM generate_series(301, 380) i;
		CREATE UNIQUE INDEX fk_links_idx ON fk_links (grp, dim_b_id);
		CREATE TABLE fk_assigns AS SELECT 9 AS sub, 1 AS grp, NULL::int AS dim_b_id UNION ALL SELECT 9, NULL, i FROM generate_series(101, 400) i;
		CREATE TABLE fk_entries AS SELECT row_number() OVER ()::int AS id, m.id AS dim_b_id, 1 + (m.id * 7 + g) % 20 AS foreign_id, date '2024-01-01' + (g * 37 + m.id * 11) % 900 AS day,
			jsonb_build_object('alpha', (g % 10)::float, 'beta_x', ((g * 3) % 10)::float) || CASE WHEN g % 3 = 0 THEN jsonb_build_object('gamma', (g % 7)::float) ELSE '{}' END AS metrics_a
			FROM fk_members m, LATERAL generate_series(1, CASE WHEN m.id <= 100 THEN 4800 ELSE 8 END) g WHERE m.id <= 3000;
		CREATE INDEX fk_entries_col ON fk_entries USING sift_col (id, dim_b_id, foreign_id, day, metrics_a) WITH (key_cut = 'dim_b_id', paths = 'metrics_a.* float8');
		CREATE INDEX fk_entries_member ON fk_entries (dim_b_id);
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) fk_entries, fk_members, fk_links, fk_labels, fk_sources, fk_assigns
--@ pg
WITH picked AS MATERIALIZED (SELECT DISTINCT m.id FROM fk_members m
  WHERE m.id IN (SELECT coalesce(a.dim_b_id, l.dim_b_id) FROM fk_assigns a LEFT JOIN fk_links l ON l.grp = a.grp WHERE a.sub = 9 AND coalesce(a.dim_b_id, l.dim_b_id) IS NOT NULL)
    AND m.id IN (SELECT dim_b_id FROM fk_links WHERE grp IN (1)) AND m.kind = 'a'),
 periods AS (SELECT * FROM (VALUES ('current', daterange('2025-05-01', '2025-06-30', '[]')), ('previous', daterange('2024-05-01', '2024-06-30', '[]'))) p(period, range)),
 parsed AS (
  SELECT e.id, p.period, s.category, CAST(s.value AS float) AS score
  FROM fk_entries e JOIN periods p ON e.day <@ p.range JOIN picked ON picked.id = e.dim_b_id
  JOIN fk_sources src ON src.id = e.foreign_id AND src.enabled IS TRUE
  INNER JOIN LATERAL (SELECT coalesce(l.label, split_part(r.key, '_', 1)) AS category, r.value
                      FROM jsonb_each(e.metrics_a) r LEFT JOIN fk_labels l ON l.foreign_id = e.foreign_id AND l.source_key = r.key) s ON true)
SELECT d.period, x.category, count(DISTINCT x.id) AS entries, round(avg(x.score)::numeric, 6) AS score
FROM periods d LEFT JOIN parsed x ON x.period = d.period
GROUP BY d.period, x.category ORDER BY d.period, x.category
