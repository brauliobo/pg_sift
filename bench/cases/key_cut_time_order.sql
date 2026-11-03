-- ORDER BY a timestamp LIMIT 20 OFFSET a tenth of the rows over a table whose timestamps are in no order of the heap: the default key_cut cuts the units by the timestamp and writes key_order with it; the variants are the same table with `key_order = 'off'` (`off`) and with a b-tree on the timestamp
-- label: `SELECT t, v ORDER BY t OFFSET <rows / 10> LIMIT 20` over a timestamp in arrival order, the index with the default key_cut (and the key_order it brings); the variants `off` and `btree` are the same rows
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS kt;
CREATE TABLE kt AS SELECT g::int AS id, timestamp '2020-01-01' + ((hashint4(g) & 2147483647) % 157766400) * interval '1 second' + g * interval '1 microsecond' AS t, (hashint4(g + 1) & 2147483647) % 1000 AS v, md5(g::text) AS tag FROM generate_series(1, {rows}) g;
CREATE INDEX kt_col ON kt USING sift_col (t, v, id);
CREATE SCHEMA c_key_cut_time_order__off;
CREATE TABLE c_key_cut_time_order__off.kt AS TABLE kt;
CREATE INDEX kt_col ON c_key_cut_time_order__off.kt USING sift_col (t, v, id) WITH (key_order = 'off');
CREATE SCHEMA c_key_cut_time_order__btree;
CREATE TABLE c_key_cut_time_order__btree.kt AS TABLE kt;
CREATE UNIQUE INDEX kt_bt ON c_key_cut_time_order__btree.kt (t);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) kt;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_cut_time_order__off.kt;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_cut_time_order__btree.kt
--@ pg
SELECT t, v FROM kt ORDER BY t OFFSET {rows} / 10 LIMIT 20
