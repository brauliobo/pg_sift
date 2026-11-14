-- key_cut by default: of the keys that gain, the one whose lookups gain most is cut by, not the first in the index and not a column of three values; the leading column
-- of a CLUSTER of the table wins a tie; a leaf with no values (all NULL) is not a leaf that follows the table; a table stored by its entity is left as it is
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

-- kg: 20 tenants of 3000 rows each stored together, the tenants in no order; kind_id has three values, empty_id none
CREATE TABLE kg (id int PRIMARY KEY, tenant_id int, kind_id int, empty_id int, a1 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kg SELECT row_number() OVER (), t.pos, CASE WHEN random() < 0.6 THEN 15 WHEN random() < 0.5 THEN 16 ELSE 9 END, NULL, i % 1000, repeat('x', 100)
FROM (SELECT row_number() OVER (ORDER BY random()) AS pos FROM generate_series(1, 20)) t, generate_series(1, 3000) i ORDER BY t.pos, i;
-- kx: 100 tenants and 103 parents scattered over the heap; kind_id has three values, empty_id none
CREATE TABLE kx (id int PRIMARY KEY, tenant_id int, parent_id int, kind_id int, empty_id int, a1 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO kx SELECT i, i % 101, i % 103, CASE WHEN random() < 0.6 THEN 15 WHEN random() < 0.5 THEN 16 ELSE 9 END, NULL, i % 1000, repeat('x', 100)
FROM generate_series(1, 60000) i;
VACUUM (FREEZE, ANALYZE) kg, kx;

CREATE INDEX kg_i ON kg USING sift_col (id, kind_id, tenant_id, empty_id, a1);
CREATE INDEX kx_kind_first ON kx USING sift_col (id, kind_id, tenant_id, parent_id, empty_id, a1);
CREATE INDEX kx_tenant_first ON kx USING sift_col (id, tenant_id, parent_id, kind_id, empty_id, a1);
CREATE INDEX kx_parent_first ON kx USING sift_col (id, parent_id, tenant_id, kind_id, empty_id, a1);
CREATE INDEX kx_kind_last ON kx USING sift_col (id, parent_id, kind_id, tenant_id, empty_id, a1);
CREATE INDEX kx_kind_plain ON kx USING sift_col (id, kind_id, tenant_id, a1);
CREATE FUNCTION kdecide(i regclass) RETURNS TABLE (leaf text, decision text, reads_more_than_ideal boolean, reason text) LANGUAGE sql AS
$$ SELECT d.leaf, d.decision, d.reads > d.ideal, d.reason FROM sift.key_cut_decision(i) d $$;

-- the tenants are stored together: nothing to cut, and the numbers are those of the leaf the verdict names
SELECT * FROM kdecide('kg_i');

-- no cluster: a column of three values listed first does not win, whatever the order of the index
SELECT 'kind first' AS index, leaf, decision FROM kdecide('kx_kind_first');
SELECT 'kind last' AS index, leaf, decision FROM kdecide('kx_kind_last');
SELECT 'kind first, no empty leaf' AS index, leaf, decision FROM kdecide('kx_kind_plain');

-- the leading column of a CLUSTER (marked, the rows stay where they are) wins the tie of two keys, listed first or last; a column that gains much less does not
CREATE INDEX kx_tenant_bt ON kx (tenant_id);
CREATE INDEX kx_parent_bt ON kx (parent_id);
CREATE INDEX kx_kind_bt ON kx (kind_id);
ALTER TABLE kx CLUSTER ON kx_tenant_bt;
SELECT 'cluster tenant' AS state, i AS index, leaf, decision FROM (VALUES ('kx_tenant_first'), ('kx_parent_first')) c (i), LATERAL kdecide(i::regclass) ORDER BY 2;
ALTER TABLE kx CLUSTER ON kx_parent_bt;
SELECT 'cluster parent' AS state, i AS index, leaf, decision FROM (VALUES ('kx_tenant_first'), ('kx_parent_first')) c (i), LATERAL kdecide(i::regclass) ORDER BY 2;
ALTER TABLE kx CLUSTER ON kx_kind_bt;
SELECT 'cluster kind' AS state, i AS index, leaf, decision FROM (VALUES ('kx_tenant_first'), ('kx_parent_first')) c (i), LATERAL kdecide(i::regclass) ORDER BY 2;

-- what a build does
DROP INDEX kx_kind_plain, kx_kind_first, kx_tenant_first, kx_parent_first, kx_kind_last;
ALTER TABLE kx CLUSTER ON kx_parent_bt;
CREATE INDEX kx_built ON kx USING sift_col (id, tenant_id, parent_id, kind_id, empty_id, a1);
SELECT sift_helpers.index_key_cut_reloption('kx_built');

DROP TABLE kg, kx;
DROP FUNCTION kdecide(regclass);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
