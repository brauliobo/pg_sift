-- CREATE INDEX CONCURRENTLY on a sift_table: the second pass looks every row up among the TIDs the first found, which it holds a batch at a time within
-- maintenance_work_mem (6 bytes a row) and scans the table once for each batch, a row being looked for in the batch of its TID only. At the smallest
-- budget 30,000 rows are three batches; a unique index shows a row looked for in the wrong batch (it would be inserted twice).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

CREATE TABLE vb (id int, v int) USING sift_table;
INSERT INTO vb SELECT i, i FROM generate_series(1, 20000) i;
VACUUM vb;
INSERT INTO vb SELECT i, i FROM generate_series(20001, 30000) i;
UPDATE vb SET v = v + 1 WHERE id % 5 = 0;
DELETE FROM vb WHERE id % 7 = 0;

SET maintenance_work_mem = '64kB';
CREATE UNIQUE INDEX CONCURRENTLY vb_id ON vb (id);
CREATE INDEX CONCURRENTLY vb_v ON vb (v);
RESET maintenance_work_mem;
SELECT indisvalid, indisready FROM pg_index WHERE indexrelid IN ('vb_id'::regclass, 'vb_v'::regclass) ORDER BY indexrelid::regclass::text;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS by_id, sum(id) FROM vb WHERE id >= 0;
SELECT count(*) AS by_v, sum(v) FROM vb WHERE v >= 0;
SET enable_seqscan = on;
SELECT count(*) AS by_scan, sum(id), sum(v) FROM vb;

DROP TABLE vb;
DROP EXTENSION pg_sift;
