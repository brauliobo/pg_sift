-- the latest row of each of 100 entities (LATERAL .. ORDER BY ts DESC LIMIT 1) over one table of entities in arrival order, with key_order = 'ent, ts': the variants are the same table with the default index and with a b-tree on (ent, ts)
-- the timestamps follow the arrival within a week (a feed that is late), so no zone of the default index tells the latest rows of an entity
-- label: `SELECT .. FROM 100 entities, LATERAL (SELECT ts .. WHERE ent = e ORDER BY ts DESC LIMIT 1)`, 2,000 entities, the index with `key_order = 'ent, ts'`; the variants `default` and `btree` are the same rows
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS ko;
CREATE TABLE ko AS SELECT g::int AS id, 1 + (hashint4(g) & 2147483647) % 2000 AS ent, timestamp '2024-01-01' + (g * 30 - (hashint4(g + 7) & 2147483647) % 604800) * interval '1 second' + g * interval '1 microsecond' AS ts, (1 + (hashint4(g + 11) & 2147483647) % 5)::int2 AS status, (hashint4(g + 13) & 2147483647) % 1001 AS counter, md5(g::text) AS tag FROM generate_series(1, {rows}) g ORDER BY g;
CREATE INDEX ko_col ON ko USING sift_col (ent, ts, status, counter) WITH (key_order = 'ent, ts');
CREATE SCHEMA c_key_order_latest__default;
CREATE TABLE c_key_order_latest__default.ko AS TABLE ko;
CREATE INDEX ko_col ON c_key_order_latest__default.ko USING sift_col (ent, ts, status, counter);
CREATE SCHEMA c_key_order_latest__btree;
CREATE TABLE c_key_order_latest__btree.ko AS TABLE ko;
CREATE INDEX ko_bt ON c_key_order_latest__btree.ko (ent, ts);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ko;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_latest__default.ko;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_latest__btree.ko
--@ pg
SELECT d.e, l.ts FROM (SELECT g * 19 AS e FROM generate_series(1, 100) g) d, LATERAL (SELECT ts FROM ko x WHERE x.ent = d.e ORDER BY ts DESC LIMIT 1) l ORDER BY d.e
