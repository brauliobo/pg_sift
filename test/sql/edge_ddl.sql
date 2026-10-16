-- DDL around sift indexes: partitioned tables, ALTER INDEX options and the analyzer mismatch ("REINDEX required"),
-- REINDEX (also CONCURRENTLY), RENAME, unsupported index shapes, partial and expression indexes, temp tables, DROP EXTENSION
CREATE EXTENSION pg_sift;

CREATE FUNCTION ids(tbl regclass, q text) RETURNS text LANGUAGE plpgsql SET enable_seqscan = off AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM %s WHERE body @@> %L', tbl, q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION seq_ids(tbl regclass, q text) RETURNS text LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM %s WHERE body @@> %L', tbl, q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION agree(tbl regclass, q text) RETURNS text LANGUAGE sql
	RETURN ids(tbl, q) || CASE WHEN ids(tbl, q) = seq_ids(tbl, q) THEN '' ELSE ' MISMATCH ' || seq_ids(tbl, q) END;

-- partitioned table: the index on the parent creates one per partition, also for partitions attached later
CREATE TABLE p (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (1000);
CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (1000) TO (2000);
INSERT INTO p SELECT i, 'doc' || i % 10 || ' common' FROM generate_series(0, 1999) i;
CREATE INDEX p_idx ON p USING sift (body) WITH (mutable_limit = 64);
CREATE TABLE p3 PARTITION OF p FOR VALUES FROM (2000) TO (3000);
INSERT INTO p3 SELECT i, 'doc3 common' FROM generate_series(2000, 2100) i;
CREATE TABLE p4 (id int, body text);
INSERT INTO p4 SELECT i, 'doc3 attached' FROM generate_series(3000, 3050) i;
ALTER TABLE p ATTACH PARTITION p4 FOR VALUES FROM (3000) TO (4000);
SELECT c.relname, c.relkind, i.indisvalid FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid
  JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift' ORDER BY 1;
SELECT agree('p', 'doc3'), agree('p', 'common'), agree('p', 'attached'), agree('p', 'doc3 NOT common');
SELECT tableoid::regclass AS part, count(*) FROM p WHERE body @@> 'doc3' GROUP BY 1 ORDER BY 1;
SELECT sift.verify(c.oid::regclass, true) FROM pg_class c WHERE c.relname ~ '^p[0-9]_body_idx' ORDER BY c.relname;
ALTER TABLE p DETACH PARTITION p2;
SELECT agree('p', 'common');
ALTER TABLE p ATTACH PARTITION p2 FOR VALUES FROM (1000) TO (2000);
UPDATE p SET body = 'moved common' WHERE id IN (5, 1500, 2050);
DELETE FROM p WHERE id % 7 = 0;
SELECT agree('p', 'common'), agree('p', 'moved');
REINDEX INDEX p_idx;
REINDEX TABLE p;
SELECT agree('p', 'common'), agree('p', 'moved');
VACUUM p;
SELECT agree('p', 'common'), agree('p', 'moved');
SELECT sift.verify(c.oid::regclass, true) FROM pg_class c WHERE c.relname ~ '^p[0-9]_body_idx' ORDER BY c.relname;
DROP TABLE p;

-- an option change that alters the analyzer needs a REINDEX; reads, writes and verify say so
CREATE TABLE a (id int, body text);
INSERT INTO a SELECT i, 'running runner runs cats' FROM generate_series(1, 100) i;
CREATE INDEX a_idx ON a USING sift (body);
SELECT agree('a', 'runs'), agree('a', 'run');
ALTER INDEX a_idx SET (stemmer = 'english_stem');
SELECT ids('a', 'run');
INSERT INTO a VALUES (200, 'x');
SELECT sift.verify('a_idx');
REINDEX INDEX a_idx;
SELECT agree('a', 'runs'), agree('a', 'run');
INSERT INTO a VALUES (200, 'x');
SELECT sift.verify('a_idx', true);
-- k1 and b only change scores: matching keeps working, verify asks for a REINDEX
ALTER INDEX a_idx SET (k1 = 2.0);
SELECT agree('a', 'run');
SELECT sift.verify('a_idx');
REINDEX INDEX a_idx;
SELECT sift.verify('a_idx', true);
ALTER INDEX a_idx RESET (k1, stemmer);
SELECT ids('a', 'run');
REINDEX INDEX CONCURRENTLY a_idx;
SELECT agree('a', 'runs'), agree('a', 'run');
SELECT sift.verify('a_idx', true);
ALTER INDEX a_idx RENAME TO a_renamed;
ALTER TABLE a ALTER COLUMN body TYPE varchar;
SELECT agree('a', 'runs');
ALTER TABLE a ALTER COLUMN body TYPE text;
SELECT agree('a', 'runs');
SELECT sift.verify('a_renamed', true);

-- shapes the access method does not offer
CREATE INDEX a_bad ON a USING sift (body, id);
CREATE INDEX a_bad ON a USING sift (id);
CREATE INDEX a_bad ON a USING sift (body) INCLUDE (id);
CREATE UNIQUE INDEX a_bad ON a USING sift (body);
CREATE INDEX a_bad ON a USING sift (body) WITH (stemmer = 'klingon');
CREATE INDEX a_bad ON a USING sift (body) WITH (mutable_limit = 0);

-- partial and expression indexes
CREATE INDEX a_part ON a USING sift (body) WHERE id < 50;
CREATE INDEX a_expr ON a USING sift (lower(body));
SELECT agree('a', 'runs');
SELECT count(*) FROM a WHERE body @@> 'runs' AND id < 50;
SELECT count(*) FROM a WHERE lower(body) @@> 'runs';
SELECT sift.verify('a_part', true), sift.verify('a_expr', true);
DROP INDEX a_part, a_expr;

-- temporary tables
CREATE TEMP TABLE tt (id int, body text);
INSERT INTO tt SELECT i, 'hello world ' || i FROM generate_series(1, 100) i;
CREATE INDEX tt_idx ON tt USING sift (body);
INSERT INTO tt VALUES (101, 'hello late');
SELECT agree('tt', 'hello'), agree('tt', 'late');
SELECT sift.verify('tt_idx', true);
DROP TABLE tt;

-- the extension owns the access method: dropping it needs CASCADE and takes the indexes along
DROP EXTENSION pg_sift;
DROP EXTENSION pg_sift CASCADE;
SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE c.relname = 'a_renamed';
DROP TABLE a;
DROP FUNCTION agree(regclass, text), ids(regclass, text), seq_ids(regclass, text);
