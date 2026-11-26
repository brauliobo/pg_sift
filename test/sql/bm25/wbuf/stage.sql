-- The docs a statement inserts are staged and written together (bm25/wbuf.c): the statement after them, a trigger, a rolled back subtransaction or transaction, a
-- REINDEX or TRUNCATE of the same transaction and a lost ON CONFLICT leave the index as the heap says, and a read of an open buffer of 2,000 docs writes no WAL
CREATE EXTENSION pg_sift;
CREATE TABLE st (id serial PRIMARY KEY, body text);
CREATE INDEX st_idx ON st USING sift (body);
SET enable_seqscan = off;
SET enable_indexscan = off;

CREATE FUNCTION st_counts() RETURNS TABLE (term text, idx bigint, heap bigint) LANGUAGE plpgsql AS $$
DECLARE t text;
BEGIN
	FOREACH t IN ARRAY ARRAY['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'eta', 'theta', 'iota', 'kappa']
	LOOP
		term := t;
		EXECUTE 'SELECT count(*) FROM st WHERE body @@> $1' INTO idx USING t;
		EXECUTE 'SELECT count(*) FROM st WHERE body ~ (''\m'' || $1 || ''\M'')' INTO heap USING t;
		IF idx <> 0 OR heap <> 0 THEN
			RETURN NEXT;
		END IF;
	END LOOP;
END $$;

-- the statements after the inserts of a transaction find them
BEGIN;
INSERT INTO st (body) SELECT 'alpha w' || i FROM generate_series(1, 300) i;
SELECT count(*) FROM st WHERE body @@> 'alpha';
INSERT INTO st (body) VALUES ('alpha tail');
SELECT count(*) FROM st WHERE body @@> 'alpha';
COMMIT;
SELECT * FROM st_counts();

-- a trigger finds the docs of the rows before its row (BEFORE) and of all the rows of the statement (AFTER)
CREATE TABLE seen (kind text, n bigint);
CREATE FUNCTION st_seen() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO seen SELECT TG_WHEN, count(*) FROM st WHERE body @@> 'beta';
	RETURN NEW;
END $$;
CREATE TRIGGER st_before BEFORE INSERT ON st FOR EACH ROW EXECUTE FUNCTION st_seen();
CREATE TRIGGER st_after AFTER INSERT ON st FOR EACH ROW EXECUTE FUNCTION st_seen();
INSERT INTO st (body) SELECT 'beta' FROM generate_series(1, 3);
SELECT kind, n FROM seen ORDER BY kind, n;
DROP TRIGGER st_before ON st;
DROP TRIGGER st_after ON st;

-- a subtransaction that is rolled back leaves no doc, one that is released keeps them
BEGIN;
INSERT INTO st (body) VALUES ('gamma one');
SAVEPOINT s1;
INSERT INTO st (body) SELECT 'gamma two' FROM generate_series(1, 50);
ROLLBACK TO s1;
INSERT INTO st (body) VALUES ('gamma three');
SAVEPOINT s2;
INSERT INTO st (body) SELECT 'delta' FROM generate_series(1, 5);
RELEASE s2;
SELECT count(*) FILTER (WHERE body @@> 'gamma') AS gamma, count(*) FILTER (WHERE body @@> 'delta') AS delta FROM st;
COMMIT;
DO $$
BEGIN
	BEGIN
		INSERT INTO st (body) SELECT 'epsilon' FROM generate_series(1, 20);
		RAISE EXCEPTION 'undo';
	EXCEPTION WHEN raise_exception THEN
		NULL;
	END;
	INSERT INTO st (body) VALUES ('epsilon');
END $$;
SELECT * FROM st_counts();

-- the rows of an ON CONFLICT that loses are not in the index
INSERT INTO st (id, body) SELECT i, 'eta' FROM generate_series(1, 400) i ON CONFLICT (id) DO NOTHING;
SELECT * FROM st_counts() WHERE term = 'eta';
SELECT setval(pg_get_serial_sequence('st', 'id'), 1000) AS next_id;

-- a REINDEX or a TRUNCATE of the transaction rebuilds the index from the rows: the staged docs are not written twice
BEGIN;
INSERT INTO st (body) SELECT 'theta' FROM generate_series(1, 10);
REINDEX INDEX st_idx;
SELECT * FROM st_counts() WHERE term = 'theta';
COMMIT;
BEGIN;
INSERT INTO st (body) SELECT 'kappa' FROM generate_series(1, 10);
TRUNCATE st;
INSERT INTO st (body) SELECT 'iota' FROM generate_series(1, 3);
SELECT * FROM st_counts();
COMMIT;
SELECT * FROM st_counts();

-- a rolled back transaction and a dropped index
BEGIN;
INSERT INTO st (body) SELECT 'kappa' FROM generate_series(1, 100);
ROLLBACK;
SELECT * FROM st_counts();
BEGIN;
INSERT INTO st (body) SELECT 'kappa' FROM generate_series(1, 100);
DROP INDEX st_idx;
COMMIT;
CREATE INDEX st_idx ON st USING sift (body);
SELECT * FROM st_counts();
SELECT sift.verify('st_idx', true);

-- a read of an open buffer of 2,000 docs and more writes no WAL
TRUNCATE st;
INSERT INTO st (body) SELECT 'alpha w' || i % 7 || ' x' || i FROM generate_series(1, 3000) i;
SELECT sum(ndocs) >= 2000 AS open_buffer FROM sift.segments('st_idx') WHERE kind = 'buffer';
RESET enable_seqscan;
SET enable_indexonlyscan = off;
SELECT count(body) FROM st;
SET enable_seqscan = off;
SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), pg_current_wal_insert_lsn()) <= 0 AS measured_before;
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS l0 \gset
DO $$
BEGIN
	FOR i IN 1..100 LOOP
		PERFORM sum(id) FROM st WHERE body @@> 'w3';
	END LOOP;
END $$;
SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'l0') AS read_wal_bytes;
SELECT sift.verify('st_idx', true);
DROP FUNCTION st_counts(), st_seen();
DROP TABLE st, seen;
DROP EXTENSION pg_sift;
