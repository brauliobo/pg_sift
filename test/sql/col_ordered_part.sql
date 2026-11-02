-- The ordered index-only scan is for the indexes of a table that has rows: a partitioned table's index (also one made ON ONLY, which a partition attached later completes) has no access method routine,
-- and a hash sub-partition of a range partition is such a table read as the relation of a plan. EXPLAIN of min, max and ORDER BY ... LIMIT 1 over them must plan, and give the rows of the leaves.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE op (id int4, d date, pad text) PARTITION BY RANGE (d);
CREATE TABLE op_a PARTITION OF op FOR VALUES FROM ('2024-01-01') TO ('2024-07-01');
CREATE TABLE op_b PARTITION OF op FOR VALUES FROM ('2024-07-01') TO ('2025-01-01') PARTITION BY HASH (id);
CREATE TABLE op_b0 PARTITION OF op_b FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE op_b1 PARTITION OF op_b FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO op SELECT i, date '2024-01-01' + (i * 7) % 360, 'x' FROM generate_series(1, 3000) i;
CREATE INDEX op_i ON ONLY op USING sift_col (id, d);
CREATE INDEX op_a_i ON op_a USING sift_col (id, d);
CREATE INDEX op_b_i ON ONLY op_b USING sift_col (id, d);
CREATE INDEX op_b0_i ON op_b0 USING sift_col (id, d);
CREATE INDEX op_b1_i ON op_b1 USING sift_col (id, d);
ALTER INDEX op_b_i ATTACH PARTITION op_b0_i;
ALTER INDEX op_b_i ATTACH PARTITION op_b1_i;
ALTER INDEX op_i ATTACH PARTITION op_a_i;
ALTER INDEX op_i ATTACH PARTITION op_b_i;
VACUUM ANALYZE op;

EXPLAIN (COSTS OFF) SELECT max(id) FROM op_b;
SELECT max(id), min(id) FROM op_b;
SELECT max(id), min(id) FROM op;
SELECT id FROM op_b ORDER BY id LIMIT 3;
SELECT id FROM op ORDER BY id DESC LIMIT 3;

-- an index that is not valid yet (CONCURRENTLY failed, or ON ONLY on a table whose partitions are not attached)
CREATE TABLE ov (id int4, d date) PARTITION BY HASH (id);
CREATE TABLE ov0 PARTITION OF ov FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE ov1 PARTITION OF ov FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO ov SELECT i, date '2024-01-01' + i % 300 FROM generate_series(1, 1000) i;
CREATE INDEX ov_i ON ONLY ov USING sift_col (id, d);
SELECT max(id), min(id) FROM ov;
SELECT id FROM ov ORDER BY id LIMIT 2;

DROP TABLE op, ov;
DROP EXTENSION pg_sift CASCADE;
