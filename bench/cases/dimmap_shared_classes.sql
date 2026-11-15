-- a join to a dimension table of few distinct tuples: its classes are read from the leaves of its sift_col index once and kept in shared memory, where every query read the table again
-- label: `count(*), sum(amount)` of {rows} facts joined to a table of {rows} / 10 keys with 60 distinct values of its group column, `GROUP BY` that column, the lookup caches on (the default), `variants` without the map (`sift.agg_dimmap_kb = 0`)
-- ordered: yes
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS dm_fact;
DROP TABLE IF EXISTS dm_dim;
CREATE TABLE dm_dim (id int PRIMARY KEY, grp int, tier int) WITH (autovacuum_enabled = false);
INSERT INTO dm_dim SELECT g, g % 60, g % 7 FROM generate_series(1, {rows} / 10) g;
CREATE TABLE dm_fact (id int, dim_id int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO dm_fact SELECT g, 1 + (g * 31) % ({rows} / 10), g % 1000 FROM generate_series(1, {rows}) g;
CREATE INDEX dm_dim_col ON dm_dim USING sift_col (id, grp, tier);
CREATE INDEX dm_fact_col ON dm_fact USING sift_col (id, dim_id, amount);
VACUUM (FREEZE, ANALYZE) dm_dim, dm_fact
--@ pg
SELECT d.grp, count(*), sum(f.amount) FROM dm_fact f JOIN dm_dim d ON d.id = f.dim_id GROUP BY d.grp ORDER BY d.grp
