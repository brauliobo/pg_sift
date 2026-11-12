-- the members of one tenant as = ANY (ARRAY(SELECT member ..)) over the pairs of a scope of two paths: an InitPlan PostgreSQL runs before the scan, planned also as IN
-- label: `f.dim_a_id = ANY (ARRAY(SELECT member FROM su_pairs WHERE tenant_id = 37))`, {rows} facts, about 180 members of the tenant, a fact counted once
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS su_facts, su_grants, su_memberships CASCADE;
CREATE TABLE su_grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX su_grants_direct ON su_grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX su_grants_group ON su_grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE su_memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX su_memberships_active ON su_memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
INSERT INTO su_memberships (group_id, dim_a_id) SELECT g, 1 + (g * 50 + m * 7) % 10000 FROM generate_series(1, 200) g, generate_series(0, 49) m ON CONFLICT DO NOTHING;
INSERT INTO su_grants (id, tenant_id, dim_a_id) SELECT t * 1000 + m, t, 1 + (t * 97 + m * 13) % 10000 FROM generate_series(1, 100) t, generate_series(1, 30) m ON CONFLICT DO NOTHING;
INSERT INTO su_grants (id, tenant_id, group_id) SELECT 200000 + t * 10 + g, t, 1 + (t * 3 + g) % 200 FROM generate_series(1, 100) t, generate_series(0, 2) g ON CONFLICT DO NOTHING;
CREATE TABLE su_facts (id int PRIMARY KEY, dim_a_id int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO su_facts SELECT g, 1 + (g * 7) % 10000, g % 100 FROM generate_series(1, {rows}) g;
CREATE INDEX su_grants_by_member ON su_grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX su_grants_by_group ON su_grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX su_memberships_by_member ON su_memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE INDEX su_facts_col ON su_facts USING sift_col (id, dim_a_id, amount)
  WITH (scope = 'tenant_id: dim_a_id -> su_grants(dim_a_id, tenant_id) | su_grants(group_id, tenant_id) via su_memberships(group_id, dim_a_id)');
-- built before the journals of the pairs existed: the stamps are the next build's
REINDEX INDEX su_facts_col;
CREATE VIEW su_pairs AS
  SELECT g.dim_a_id AS member, g.tenant_id FROM su_grants g WHERE g.deleted_at IS NULL
  UNION
  SELECT m.dim_a_id, g.tenant_id FROM su_grants g JOIN su_memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
VACUUM (FREEZE, ANALYZE) su_facts, su_grants, su_memberships
--@ pg
SELECT count(*) AS n, sum(f.amount) AS total FROM su_facts f WHERE f.dim_a_id = ANY (ARRAY(SELECT member FROM su_pairs WHERE tenant_id = 37))
