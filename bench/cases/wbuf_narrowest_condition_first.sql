-- the rows of the write buffers are looked up by the condition with the fewest values, an IN list next to an equality on the same key
-- label: `SELECT id, amount WHERE parent_id = ANY (4,650 ids) AND parent_id = <one> AND ts >= ..` over 300,000 rows with 15,000 (5%) still in the write buffer
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE wb_narrow (id int PRIMARY KEY, parent_id int NOT NULL, kind int NOT NULL, ts timestamp NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO wb_narrow SELECT i, 1 + i % 4650, i % 9, timestamp '2024-01-01' + (i % 100000) * interval '10 minutes', i % 1000 FROM generate_series(1, 300000) i;
CREATE INDEX wb_narrow_col ON wb_narrow USING sift_col (id, parent_id, kind, ts, amount) WITH (key_cut = 'parent_id');
VACUUM (FREEZE, ANALYZE) wb_narrow;
INSERT INTO wb_narrow SELECT i, 1 + i % 4650, i % 9, timestamp '2024-01-01' + (i % 100000) * interval '10 minutes', i % 1000 FROM generate_series(300001, 315000) i;
ANALYZE wb_narrow
--@ pg
SELECT id, amount FROM wb_narrow WHERE parent_id = ANY (ARRAY(SELECT g FROM generate_series(1, 4650) g)) AND parent_id = 77 AND ts >= '2024-06-01' ORDER BY id
