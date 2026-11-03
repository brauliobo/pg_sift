-- VACUUM seals an open write buffer that holds a quarter of a unit of rows even when its pages are far under a quarter of write_buffer_limit (a column of one small integer is a few
-- bytes a row): the rows leave the buffer, which every probe reads row by row, for a segment. Fewer rows stay where they are.
CREATE EXTENSION pg_sift;
CREATE TABLE sr (k int) WITH (autovacuum_enabled = off);
CREATE INDEX sr_idx ON sr USING sift_col (k) WITH (write_buffer_limit = 65536);
CREATE FUNCTION buffered(t regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce(sum(rows), 0) FROM sift.col_segments(t) WHERE kind <> 'immutable' $$;

INSERT INTO sr SELECT g % 100 FROM generate_series(1, 5000) g;
VACUUM sr;
SELECT buffered('sr_idx') AS few_rows_stay;

INSERT INTO sr SELECT g % 100 FROM generate_series(1, 20000) g;
VACUUM sr;
SELECT buffered('sr_idx') AS many_rows_sealed;
SELECT count(*) FROM sr WHERE k = 7;

DROP TABLE sr;
DROP FUNCTION buffered(regclass);
DROP EXTENSION pg_sift;
