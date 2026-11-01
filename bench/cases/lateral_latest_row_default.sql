-- the latest row of each of 200 entities (LATERAL .. ORDER BY d DESC, id LIMIT 1) over an entity of 3,000 keys and a date of 730 values, with the default index: the grouped order array is built by default for a date and an entity
-- label: `FROM generate_series(1, 200) e, LATERAL (SELECT id, d FROM ev WHERE entity = e ORDER BY d DESC, id LIMIT 1)` over {rows} rows of 7 columns, the default index (no order option); the variant `btree` is the same rows with a b-tree on (entity, d DESC, id)
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS ev;
CREATE TABLE ev AS SELECT id, ((hashint8extended(id, 5) & 2147483647) % 3000)::int AS entity, (date '2023-01-01' + (hashint8extended(id, 9) & 1023)::int % 730) AS d, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::int AS c, ((hashint8extended(id, 11) & 16777215) % 1000)::int AS v FROM orders ORDER BY id;
CREATE INDEX ev_col ON ev USING sift_col (id, entity, d, a, b, c, v);
VACUUM (FREEZE, ANALYZE) ev;
DROP SCHEMA IF EXISTS c_lateral_latest_row_default__btree CASCADE;
CREATE SCHEMA c_lateral_latest_row_default__btree;
CREATE TABLE c_lateral_latest_row_default__btree.ev AS TABLE ev;
CREATE INDEX ev_bt ON c_lateral_latest_row_default__btree.ev (entity, d DESC, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_lateral_latest_row_default__btree.ev
--@ pg
SELECT e, l.id, l.d FROM generate_series(1, 200) e, LATERAL (SELECT id, d FROM ev WHERE entity = e ORDER BY d DESC, id LIMIT 1) l ORDER BY e
