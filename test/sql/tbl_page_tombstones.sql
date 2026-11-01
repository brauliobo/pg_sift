-- Rows a tombstone deletes are taken off a page of a column group at once, where a scan of a few rows asks the log for each row: the
-- aggregates of a table with deletes and updates not yet vacuumed equal those of the same statements with the pre-aggregates off,
-- for the snapshot of a statement, of a cursor that is older than the delete of its own transaction, and of a snapshot that is older
-- than a committed delete.
CREATE EXTENSION pg_sift;
CREATE TABLE t (id int, g int, n int) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 7, i % 100 FROM generate_series(1, 12000) i;
VACUUM t;
SELECT kind, rows, dead_rows FROM sift.table_segments('t') WHERE kind = 'columns';

-- 10% deleted and 5% updated (the old versions are tombstones), nothing vacuumed
DELETE FROM t WHERE id % 10 = 3;
UPDATE t SET n = n + 1000 WHERE id % 20 = 5;
SELECT count(*) > 0 AS tombstones FROM sift.table_segments('t') WHERE kind = 'tombstones';

CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (' || q || ') x' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (' || q || ') x' INTO b;
	RETURN a = b;
END $$;

SELECT same('SELECT count(*), sum(n), min(id), max(id) FROM t');
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g');
SELECT same('SELECT g, count(*), sum(n) FROM t WHERE g IN (1, 2) GROUP BY g');
SELECT same('SELECT count(*), sum(n) FROM t WHERE id BETWEEN 3000 AND 9000');
SELECT count(*), sum(n) FROM t;

-- a cursor opened before the delete of its own transaction still sees the rows it deletes
BEGIN;
DECLARE c CURSOR FOR SELECT count(*), sum(n) FROM t;
DELETE FROM t WHERE id % 10 = 4;
UPDATE t SET n = 0 WHERE id % 20 = 6;
SELECT count(*), sum(n) FROM t;
FETCH c;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g');
ROLLBACK;
SELECT count(*), sum(n) FROM t;

-- a snapshot older than a committed delete (another session's: dblink is not here, so the delete is a subtransaction that rolls back, and a savepoint that commits)
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*), sum(n) FROM t;
SAVEPOINT s;
DELETE FROM t WHERE id % 10 = 5;
SELECT count(*), sum(n) FROM t;
ROLLBACK TO s;
SELECT count(*), sum(n) FROM t;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g');
COMMIT;

-- after the VACUUM the same
VACUUM t;
SELECT same('SELECT g, count(*), sum(n) FROM t GROUP BY g');
SELECT count(*), sum(n) FROM t;

DROP FUNCTION same(text);
DROP TABLE t;
DROP EXTENSION pg_sift;
