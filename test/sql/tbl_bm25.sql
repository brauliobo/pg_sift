-- BM25 `sift` index on a sift_table: build, maintenance, MVCC checks of candidates (delta rows have no visibility map),
-- agreement with the sequential scan, VACUUM, REINDEX CONCURRENTLY
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_tam;

CREATE TABLE docs (id int, body text) USING sift_table;
INSERT INTO docs SELECT g, 'order number ' || g || CASE WHEN g % 2 = 0 THEN ' express shipping' ELSE ' standard shipping' END
FROM generate_series(1, 200) g;
CREATE INDEX docs_bm25 ON docs USING sift (body);

CREATE FUNCTION idx_ids(q text) RETURNS text LANGUAGE plpgsql SET enable_seqscan = off AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM docs WHERE body @@> %L', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION seq_ids(q text) RETURNS text LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM docs WHERE body @@> %L', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION agree(q text) RETURNS text LANGUAGE sql
	RETURN idx_ids(q) || CASE WHEN idx_ids(q) = seq_ids(q) THEN '' ELSE ' MISMATCH ' || seq_ids(q) END;

SELECT agree('express'), agree('standard'), agree('number'), agree('absent');

-- maintenance: insert, update, delete
INSERT INTO docs VALUES (1000, 'express late arrival'), (1001, 'standard late arrival');
DELETE FROM docs WHERE id IN (2, 4, 5);
UPDATE docs SET body = 'refund processed' WHERE id IN (6, 7);
SELECT agree('express'), agree('standard'), agree('refund'), agree('late'), agree('arrival');

-- rollback and savepoints leave no trace; own uncommitted rows are visible to the transaction itself
BEGIN;
INSERT INTO docs VALUES (2000, 'express phantom');
DELETE FROM docs WHERE id = 8;
SELECT agree('phantom'), agree('express');
SAVEPOINT s;
UPDATE docs SET body = 'phantom again' WHERE id = 2000;
SELECT agree('phantom');
ROLLBACK TO s;
SELECT agree('phantom');
ROLLBACK;
SELECT agree('phantom'), agree('express');

-- VACUUM, VACUUM FULL, REINDEX
VACUUM docs;
SELECT agree('express'), agree('refund');
VACUUM (FULL) docs;
SELECT agree('express'), agree('refund');
REINDEX INDEX docs_bm25;
SELECT agree('express'), agree('refund');
REINDEX INDEX CONCURRENTLY docs_bm25;
SELECT agree('express'), agree('refund');

-- ranked search through the index returns visible rows only
SET enable_seqscan = off;
SELECT id FROM docs WHERE body @@> 'late' ORDER BY id;
RESET enable_seqscan;

-- CREATE INDEX CONCURRENTLY on a table with concurrent history
CREATE INDEX CONCURRENTLY docs_bm25_c ON docs USING sift (body);
SELECT agree('express');
DROP TABLE docs;
DROP EXTENSION pg_sift_tam;
DROP EXTENSION pg_sift;
