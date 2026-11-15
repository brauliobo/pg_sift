-- sift.workers lists a sift_table next to the indexes: index is the table, kind sift_table, its directory entries and sealed write buffers are counted,
-- and the columns that note a failed background step are 0 and NULL (the background of a table keeps no note). worker_state refuses anything else.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;

CREATE TABLE tw (id int, g int, v float8) USING sift_table;
CREATE TABLE tp (id int, g int);
CREATE INDEX tp_col ON tp USING sift_col (id, g);
INSERT INTO tw SELECT i, i % 10, i FROM generate_series(1, 5000) i;
INSERT INTO tp SELECT i, i % 10 FROM generate_series(1, 100) i;

SELECT index, kind, segments >= 1 AS has_segments, sealed >= 0 AS has_sealed, failures, failed_at, next_retry, error_code, last_error, due
FROM sift.workers ORDER BY 1::text;
SELECT segments = (SELECT count(*) FROM sift.table_segments('tw')) AS same_segments FROM sift.worker_state('tw');
SELECT * FROM sift.worker_state('pg_class');
SELECT * FROM sift.worker_state('tp');

-- a role that cannot read the table does not see it
CREATE ROLE tw_reader NOLOGIN;
SET ROLE tw_reader;
SELECT count(*) AS rows_seen FROM sift.workers;
RESET ROLE;
GRANT SELECT ON tw TO tw_reader;
SET ROLE tw_reader;
SELECT index, kind FROM sift.workers;
RESET ROLE;

DROP TABLE tw, tp;
DROP ROLE tw_reader;
DROP EXTENSION pg_sift;
