-- An inspector given an index of another sift access method names the function that serves it, and sift.indexes lists every sift object once.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE ish (id int PRIMARY KEY, body text, g int, v float8) WITH (autovacuum_enabled = off);
INSERT INTO ish SELECT g, 'alpha beta ' || g, g % 5, g FROM generate_series(1, 2000) g;
CREATE INDEX ish_bm ON ish USING sift (body);
CREATE INDEX ish_col ON ish USING sift_col (id, g, v);
CREATE TABLE ish_t (id int, g int, v float8) USING sift_table;
INSERT INTO ish_t SELECT g, g % 5, g FROM generate_series(1, 2000) g;
RESET client_min_messages;

-- an inspector of one access method given another names the function that serves it
SELECT * FROM sift.col_index_info('ish_bm');
SELECT * FROM sift.col_segments('ish_bm');
SELECT * FROM sift.index_info('ish_col');
SELECT * FROM sift.segments('ish_col');
SELECT * FROM sift.verify('ish_col');
SELECT * FROM sift.segment_postings('ish_col');
SELECT * FROM sift.col_space('ish_bm');
SELECT * FROM sift.worker_state('ish_pkey');
SELECT * FROM sift.index_info('ish_pkey');
SELECT * FROM sift.col_segments('ish_pkey');
SELECT * FROM sift.table_space('ish');
SELECT * FROM sift.table_segments('ish');

-- one view over every sift object
SELECT relation, kind, "table", bytes > 0 AS sized, segments, format ~ '^[0-9]+[.][0-9]+$' AS formatted, state FROM sift.indexes ORDER BY relation::text;
UPDATE pg_index SET indisvalid = false WHERE indexrelid = 'ish_bm'::regclass;
SELECT relation, state, segments FROM sift.indexes WHERE relation = 'ish_bm'::regclass;
UPDATE pg_index SET indisvalid = true, indisready = false WHERE indexrelid = 'ish_bm'::regclass;
SELECT relation, state, segments FROM sift.indexes WHERE relation = 'ish_bm'::regclass;
UPDATE pg_index SET indisready = true WHERE indexrelid = 'ish_bm'::regclass;
SELECT relation, state FROM sift.indexes WHERE relation = 'ish_bm'::regclass;

DROP TABLE ish, ish_t;
DROP EXTENSION pg_sift;
