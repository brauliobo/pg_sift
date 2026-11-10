-- the groups of each row's entity as a lateral sub-select over a link table (many to many), grouped by group, over the entities of one set
-- label: `LEFT JOIN LATERAL (SELECT gs_groups.* FROM gs_groups WHERE EXISTS (SELECT 1 FROM gs_links x WHERE x.deleted_at IS NULL AND x.group_id = gs_groups.id AND x.dim_a_id = f.dim_a_id)) g ON true` grouped by `g.id` over the entities of one set (a quarter of the 1,000 entities of the {rows}-row fact table, 30 groups, 1.13 links an entity); the sub-select runs once for each distinct entity in the scan
-- ordered: yes
--@ setup
CREATE TABLE gs_groups (id int PRIMARY KEY);
INSERT INTO gs_groups SELECT g FROM generate_series(1, 30) g;
CREATE TABLE gs_links (group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz);
INSERT INTO gs_links SELECT i % 30 + 1, i, NULL FROM generate_series(1, 1000) i;
INSERT INTO gs_links SELECT (i + 7) % 30 + 1, i, NULL FROM generate_series(1, 1000) i WHERE i % 100 < 13;
CREATE INDEX gs_links_dim ON gs_links (dim_a_id);
CREATE TABLE gs_set (set_id int NOT NULL, dim_a_id int NOT NULL);
INSERT INTO gs_set SELECT 5, i FROM generate_series(1, 1000) i WHERE i % 4 = 0;
CREATE TABLE gs_facts (id int PRIMARY KEY, dim_a_id int, created_at date, kind int, value_num numeric(8,2));
INSERT INTO gs_facts SELECT g, 1 + (g::bigint * 7919) % 1000, date '2025-01-01' + g % 365, g % 5, g % 97 FROM generate_series(1, {rows}) g;
CREATE INDEX gs_facts_col ON gs_facts USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (key_columns = 'dim_a_id', key_order = 'dim_a_id, created_at');
VACUUM (FREEZE, ANALYZE) gs_groups, gs_links, gs_set, gs_facts
--@ pg
SELECT g.id, count(*) AS n, sum(f.value_num) AS total FROM gs_facts f
LEFT JOIN LATERAL (SELECT gs_groups.* FROM gs_groups WHERE EXISTS (SELECT 1 FROM gs_links x WHERE x.deleted_at IS NULL AND x.group_id = gs_groups.id AND x.dim_a_id = f.dim_a_id)) g ON true
WHERE f.dim_a_id IN (SELECT dim_a_id FROM gs_set WHERE set_id = 5) GROUP BY g.id ORDER BY 1
