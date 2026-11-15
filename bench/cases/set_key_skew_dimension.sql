-- the 12 hot keys of a membership table joined through a dimension whose statistics say a table 40 times its size, over a fact table of {rows} rows of 2,000 entities (the entity the cube of a uniform value): the normal plan probes each key at the mean's rows, the scan reads the units of the keys
-- label: `JOIN dim ON dim.id = ev.entity WHERE dim.entity IN (SELECT entity FROM dim_b WHERE set_id = 1) GROUP BY dim.grp` for 12 hot keys of 2,000, {rows} rows
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS sk_ev, sk_dim, sk_dim_b;
CREATE TABLE sk_ev (id int PRIMARY KEY, entity int NOT NULL, amount int NOT NULL, pad text NOT NULL DEFAULT repeat('x', 200)) WITH (autovacuum_enabled = false);
INSERT INTO sk_ev (id, entity, amount) SELECT g, 1 + (2000 * power((hashint4(g) & 2147483647) / 2147483648.0, 3))::int, g % 1000 FROM generate_series(1, {rows}) g;
CREATE INDEX sk_ev_entity ON sk_ev (entity);
CREATE INDEX sk_ev_col ON sk_ev USING sift_col (id, entity, amount) WITH (key_columns = 'entity', key_order = 'entity');
CREATE TABLE sk_dim (id int PRIMARY KEY, entity int NOT NULL, grp int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO sk_dim SELECT g, 1 + g % 2000, g % 100 FROM generate_series(1, {rows}) g;
CREATE INDEX sk_dim_entity ON sk_dim (entity);
CREATE TABLE sk_dim_b (set_id int NOT NULL, entity int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO sk_dim_b SELECT 1, g FROM generate_series(1, 12) g;
VACUUM (FREEZE, ANALYZE) sk_ev, sk_dim, sk_dim_b;
UPDATE pg_class SET relpages = relpages * 40, reltuples = reltuples * 40 WHERE oid = 'sk_dim'::regclass
--@ pg
SELECT d.grp, count(*), sum(v.amount) FROM sk_ev v JOIN sk_dim d ON d.id = v.entity WHERE d.entity IN (SELECT entity FROM sk_dim_b WHERE set_id = 1) GROUP BY 1 ORDER BY 1
