-- the latest time of each of 300 rows of dim_a as a correlated max() sub-select with a condition on the outer row that is no equality (a grouped aggregate joined cannot take it), over an entity of 3,000 keys (100 rows each) and a unique time, with an index that groups the order array of the time by the entity: the sub-select is the first row of its entity in that order
-- label: `SELECT t.id, (SELECT max(ts) FROM pings WHERE entity = t.id AND status <> t.tier) FROM dim_a t WHERE t.tier = 3` over {rows} rows of 4 columns; the variant `btree` is the same rows with a b-tree on (entity, ts)
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS pings, dim_a;
CREATE TABLE dim_a (id int PRIMARY KEY, tier smallint NOT NULL);
INSERT INTO dim_a SELECT g, g % 10 FROM generate_series(1, greatest({rows} / 100, 1)) g;
CREATE TABLE pings (id bigint PRIMARY KEY, entity int NOT NULL, ts timestamptz NOT NULL, status smallint NOT NULL);
INSERT INTO pings SELECT g, 1 + (hashint8extended(g, 11) & 2147483647) % greatest({rows} / 100, 1), timestamptz '2024-01-01' + interval '1 second' * (g % 100003) * 600 + interval '1 microsecond' * g, (g % 400)::smallint FROM generate_series(1, {rows}) g;
CREATE INDEX pings_col ON pings USING sift_col (id, entity, ts, status) WITH (order_array_columns = 'ts', order_array_group = 'entity');
VACUUM (FREEZE, ANALYZE) dim_a, pings;
DROP SCHEMA IF EXISTS c_subselect_max_per_entity__btree CASCADE;
CREATE SCHEMA c_subselect_max_per_entity__btree;
CREATE TABLE c_subselect_max_per_entity__btree.dim_a AS TABLE dim_a;
ALTER TABLE c_subselect_max_per_entity__btree.dim_a ADD PRIMARY KEY (id);
CREATE TABLE c_subselect_max_per_entity__btree.pings AS TABLE pings;
ALTER TABLE c_subselect_max_per_entity__btree.pings ADD PRIMARY KEY (id);
CREATE INDEX pings_bt ON c_subselect_max_per_entity__btree.pings (entity, ts);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_subselect_max_per_entity__btree.dim_a, c_subselect_max_per_entity__btree.pings
--@ pg
SELECT t.id, (SELECT max(p.ts) FROM pings p WHERE p.entity = t.id AND p.status <> t.tier) FROM dim_a t WHERE t.tier = 3 ORDER BY t.id
