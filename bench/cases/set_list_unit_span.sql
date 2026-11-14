-- an IN list of 75 keys against the key summaries of the units of a key-cut index laid out in 30 batches is cut to the keys of each unit's range, where each of its members was looked up in each unit
-- label: `count(*), sum(amount) GROUP BY kind_id WHERE dim_a_id IN (75 keys)` over {rows} rows, `key_cut = dim_a_id`, 30 promoted batches
-- ordered: yes
-- set: sift.agg_force = on
--@ setup
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
DROP TABLE IF EXISTS sl_fact;
CREATE TABLE sl_fact (id int, dim_a_id int, kind_id int, amount float8) WITH (autovacuum_enabled = false);
CREATE INDEX sl_fact_col ON sl_fact USING sift_col (id, dim_a_id, kind_id, amount) WITH (key_cut = dim_a_id);
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 0;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 1;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 2;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 3;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 4;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 5;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 6;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 7;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 8;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 9;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 10;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 11;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 12;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 13;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 14;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 15;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 16;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 17;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 18;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 19;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 20;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 21;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 22;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 23;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 24;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 25;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 26;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 27;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 28;
SELECT sift.col_promote('sl_fact_col');
INSERT INTO sl_fact SELECT g, g % 1500, g % 12, ((g::bigint * 7919 % 100000) / 100.0)::float8 FROM generate_series(1, {rows}) g WHERE g % 30 = 29;
SELECT sift.col_promote('sl_fact_col');
VACUUM (ANALYZE, INDEX_CLEANUP off) sl_fact
--@ pg
SELECT kind_id, count(*), sum(amount) FROM sl_fact WHERE dim_a_id IN (1,21,41,61,81,101,121,141,161,181,201,221,241,261,281,301,321,341,361,381,401,421,441,461,481,501,521,541,561,581,601,621,641,661,681,701,721,741,761,781,801,821,841,861,881,901,921,941,961,981,1001,1021,1041,1061,1081,1101,1121,1141,1161,1181,1201,1221,1241,1261,1281,1301,1321,1341,1361,1381,1401,1421,1441,1461,1481) GROUP BY kind_id ORDER BY kind_id
