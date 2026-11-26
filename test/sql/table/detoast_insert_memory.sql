-- INSERT ... SELECT of toasted rows into a published sift_table: each row is detoasted to be written and logged, and the detoasted values go with the row. The memory of
-- the statement stays flat whatever the rows it inserts (it held every value it detoasted until the statement ended: 4 MB here, 40 GB for a load of 40 GB).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE dtm_src (id int, body text);
ALTER TABLE dtm_src ALTER COLUMN body SET STORAGE EXTERNAL;
INSERT INTO dtm_src SELECT g, string_agg(md5((g * 1000 + k)::text), '') FROM generate_series(1, 200) g, generate_series(1, 640) k GROUP BY g;
CREATE TABLE dtm_peak (format text, bytes int8);

-- the largest executor context of the backend when the INSERT ends: that of the INSERT
CREATE FUNCTION dtm_note() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO dtm_peak SELECT TG_ARGV[0], max(total_bytes) FROM pg_backend_memory_contexts WHERE name = 'ExecutorState';
	RETURN NULL;
END $$;

SET client_min_messages = error;
CREATE PUBLICATION dtm_pub;
SET client_min_messages = warning;
CREATE TABLE dtm_f2 (id int PRIMARY KEY, body text) USING sift_table;
SET sift.test_create_format1 = on;
CREATE TABLE dtm_f1 (id int PRIMARY KEY, body text) USING sift_table;
RESET sift.test_create_format1;
ALTER PUBLICATION dtm_pub ADD TABLE dtm_f2, dtm_f1;
CREATE TRIGGER dtm_f2_peak AFTER INSERT ON dtm_f2 FOR EACH STATEMENT EXECUTE FUNCTION dtm_note('f2');
CREATE TRIGGER dtm_f1_peak AFTER INSERT ON dtm_f1 FOR EACH STATEMENT EXECUTE FUNCTION dtm_note('f1');

INSERT INTO dtm_f2 SELECT * FROM dtm_src;
INSERT INTO dtm_f1 SELECT * FROM dtm_src;

SELECT format, bytes < 1024 * 1024 AS flat FROM dtm_peak ORDER BY format;
SELECT count(*), sum(length(body)) FROM dtm_f2;
SELECT count(*), sum(length(body)) FROM dtm_f1;

DROP PUBLICATION dtm_pub;
DROP TABLE dtm_f2, dtm_f1, dtm_src, dtm_peak;
DROP FUNCTION dtm_note();
DROP EXTENSION pg_sift;
