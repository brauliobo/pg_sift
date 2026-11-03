-- the batches of a write buffer that an index lets grow big (write_buffer_limit raised) are kept for the next scan whatever their size, not decoded again by every scan
-- label: `SELECT count(*) WHERE k BETWEEN ..` over a 13-column `sift_col` index with `write_buffer_limit = 65536` and two thirds of the rows of the run still in the write buffer, no VACUUM, the same statement again
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE wb_big (id int PRIMARY KEY, k int NOT NULL, c1 int NOT NULL, c2 int NOT NULL, c3 int NOT NULL, c4 int NOT NULL, c5 int NOT NULL, c6 int NOT NULL, c7 int NOT NULL, ts timestamp NOT NULL, s smallint NOT NULL, n text NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO wb_big SELECT i, i % 100000, i % 97, i % 1013, i % 7, i % 31, i % 12, i % 211, i % 5, timestamp '2024-01-01' + (i % 100000) * interval '10 minutes', i % 9, 'name ' || (i % 5000), i % 1000 FROM generate_series(1, {rows} / 3) i;
CREATE INDEX wb_big_col ON wb_big USING sift_col (id, k, c1, c2, c3, c4, c5, c6, c7, ts, s, n, amount) WITH (write_buffer_limit = 65536);
VACUUM (FREEZE, ANALYZE) wb_big;
INSERT INTO wb_big SELECT i, i % 100000, i % 97, i % 1013, i % 7, i % 31, i % 12, i % 211, i % 5, timestamp '2024-01-01' + (i % 100000) * interval '10 minutes', i % 9, 'name ' || (i % 5000), i % 1000 FROM generate_series({rows} / 3 + 1, {rows}) i;
ANALYZE wb_big
--@ pg
SELECT count(*) FROM wb_big WHERE k BETWEEN 1000 AND 1999
