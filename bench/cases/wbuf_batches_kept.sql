-- a scan keeps the rows of the write buffer as batches for the next scan, which reads the records appended since
-- label: `SELECT kind, count(*), sum(amount) WHERE ts >= .. GROUP BY kind` over 300,000 rows with 15,000 (5%) still in the write buffer, the same statement again
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE wb_kept (id int PRIMARY KEY, parent_id int NOT NULL, kind int NOT NULL, ts timestamp NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO wb_kept SELECT i, 1 + i % 4650, i % 9, timestamp '2024-01-01' + (i % 100000) * interval '10 minutes', i % 1000 FROM generate_series(1, 300000) i;
CREATE INDEX wb_kept_col ON wb_kept USING sift_col (id, parent_id, kind, ts, amount) WITH (key_cut = 'parent_id');
VACUUM (FREEZE, ANALYZE) wb_kept;
INSERT INTO wb_kept SELECT i, 1 + i % 4650, i % 9, timestamp '2024-01-01' + (i % 100000) * interval '10 minutes', i % 1000 FROM generate_series(300001, 315000) i;
ANALYZE wb_kept
--@ pg
SELECT kind, count(*), sum(amount) FROM wb_kept WHERE ts >= '2024-06-01' AND parent_id < 3000 GROUP BY kind ORDER BY kind
