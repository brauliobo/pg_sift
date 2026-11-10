-- the entities of one set with their groups as a CTE PostgreSQL inlines, the facts joined to it and grouped by group
-- label: `WITH ents AS (SELECT d.id, g.id AS group_id FROM gse_dim d LEFT JOIN LATERAL (SELECT gse_groups.* FROM gse_groups WHERE EXISTS (..)) g ON true WHERE d.id IN (set)) SELECT group_id, count(*) FROM gse_facts f JOIN ents ON ents.id = f.dim_a_id GROUP BY group_id` over the {rows}-row fact table (the entities of one set are a quarter of the 1,000; the scan is priced for the entities of the set, not the table's)
-- ordered: yes
--@ setup
CREATE TABLE gse_dim (id int PRIMARY KEY);
INSERT INTO gse_dim SELECT g FROM generate_series(1, 1000) g;
CREATE TABLE gse_groups (id int PRIMARY KEY);
INSERT INTO gse_groups SELECT g FROM generate_series(1, 30) g;
CREATE TABLE gse_links (group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz);
INSERT INTO gse_links SELECT i % 30 + 1, i, NULL FROM generate_series(1, 1000) i;
INSERT INTO gse_links SELECT (i + 7) % 30 + 1, i, NULL FROM generate_series(1, 1000) i WHERE i % 100 < 13;
CREATE INDEX gse_links_dim ON gse_links (dim_a_id);
CREATE TABLE gse_set (set_id int NOT NULL, dim_a_id int NOT NULL);
INSERT INTO gse_set SELECT 5, i FROM generate_series(1, 1000) i WHERE i % 4 = 0;
CREATE TABLE gse_facts (id int PRIMARY KEY, dim_a_id int, created_at date, kind int, value_num numeric(8,2));
INSERT INTO gse_facts SELECT g, 1 + (g::bigint * 7919) % 1000, date '2025-01-01' + g % 365, g % 5, g % 97 FROM generate_series(1, {rows}) g;
CREATE INDEX gse_facts_col ON gse_facts USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (key_columns = 'dim_a_id', key_order = 'dim_a_id, created_at');
VACUUM (FREEZE, ANALYZE) gse_dim, gse_groups, gse_links, gse_set, gse_facts
--@ pg
WITH ents AS (SELECT d.id, g.id AS group_id FROM gse_dim d
  LEFT JOIN LATERAL (SELECT gse_groups.* FROM gse_groups WHERE EXISTS (SELECT 1 FROM gse_links x WHERE x.deleted_at IS NULL AND x.group_id = gse_groups.id AND x.dim_a_id = d.id)) g ON true
  WHERE d.id IN (SELECT dim_a_id FROM gse_set WHERE set_id = 5))
SELECT ents.group_id, count(*) AS n, sum(f.value_num) AS total FROM gse_facts f JOIN ents ON ents.id = f.dim_a_id GROUP BY ents.group_id ORDER BY 1
