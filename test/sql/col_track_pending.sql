-- track_changes names a column (the default 'auto' is stored as the primary key's name): a dump restores the index before the key when pg_restore runs in parallel and when pg_upgrade does, so
-- CREATE INDEX must accept a column whose unique index is not there yet. A read takes the log for complete only with the unique index, and a statement that could not find its rows by
-- the key (no unique index then) ends the completeness: the key made later does not bring back what was not heard.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION same_groups(t regclass) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SET LOCAL sift.enable_agg = on;
  SET LOCAL seq_page_cost = 10000;
  EXECUTE format('CREATE TEMP TABLE by_sift ON COMMIT DROP AS SELECT g, count(*) AS c, sum(v) AS s FROM %s GROUP BY g', t);
  SET LOCAL sift.enable_agg = off;
  RESET seq_page_cost;
  EXECUTE format('CREATE TEMP TABLE by_heap ON COMMIT DROP AS SELECT g, count(*) AS c, sum(v) AS s FROM %s GROUP BY g', t);
  SELECT count(*) INTO n FROM ((TABLE by_sift EXCEPT TABLE by_heap) UNION ALL (TABLE by_heap EXCEPT TABLE by_sift)) d;
  RETURN n;
END $$;

-- the index before the key, nothing written between: the log is complete as soon as the key is there
CREATE TABLE a (id int NOT NULL, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO a SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX a_c ON a USING sift_col (id, g, v) WITH (track_changes = 'id');
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('a_c');
ALTER TABLE a ADD PRIMARY KEY (id);
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('a_c');
DELETE FROM a WHERE id % 2 = 0;
UPDATE a SET id = id + 500 WHERE id % 5 = 1;
SELECT same_groups('a') AS groups_differing_a;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('a_c');

-- written before the key exists: those statements are not in the log, so it is not complete when the key comes
CREATE TABLE b (id int NOT NULL, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO b SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX b_c ON b USING sift_col (id, g, v) WITH (track_changes = 'id');
DELETE FROM b WHERE id % 2 = 0;
UPDATE b SET id = id + 500 WHERE id % 5 = 1;
ALTER TABLE b ADD PRIMARY KEY (id);
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('b_c');
SELECT same_groups('b') AS groups_differing_b;
REINDEX INDEX b_c;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('b_c');

-- the key dropped, rows written, the key made again
CREATE TABLE c (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO c SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX c_c ON c USING sift_col (id, g, v);
ALTER TABLE c DROP CONSTRAINT c_pkey;
DELETE FROM c WHERE id % 2 = 0;
UPDATE c SET id = id + 500 WHERE id % 5 = 1;
ALTER TABLE c ADD PRIMARY KEY (id);
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('c_c');
SELECT same_groups('c') AS groups_differing_c;

-- the copy of a rewriting extension has no NOT NULL and no key (pg_squeeze: tmp_<oid of the table>, in its schema, indexes named ind_<n>): its index must be made
CREATE TABLE e (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO e SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX e_c ON e USING sift_col (id, g, v);
DO $$
DECLARE t text := 'tmp_' || 'e'::regclass::oid;
BEGIN
  EXECUTE format('CREATE TABLE %I (id int, g int, v int) WITH (autovacuum_enabled = false)', t);
  EXECUTE format('INSERT INTO %I SELECT i, i %% 5, i FROM generate_series(1, 200) i', t);
  EXECUTE format('CREATE INDEX ind_1 ON %I USING sift_col (id, g, v) WITH (track_changes = ''id'')', t);
  EXECUTE format('DROP TABLE %I', t);
END $$;
-- a table of that name that copies no table of the schema is refused as any other
CREATE TABLE tmp_0 (id int, g int, v int);
CREATE INDEX tmp_0_c ON tmp_0 USING sift_col (id, g, v) WITH (track_changes = 'id');
DROP TABLE tmp_0;
DROP TABLE e;

-- still refused: ALTER INDEX with a column that has no unique index, a column that can be NULL, a column the table does not have
CREATE TABLE d (id int NOT NULL, g int NOT NULL, v int, n int);
CREATE INDEX d_c ON d USING sift_col (id, g, v, n) WITH (track_changes = 'off');
ALTER INDEX d_c SET (track_changes = 'id');
CREATE INDEX d_n ON d USING sift_col (id, g, v, n) WITH (track_changes = 'n');
CREATE INDEX d_x ON d USING sift_col (id, g, v, n) WITH (track_changes = 'nope');

DROP TABLE a, b, c, d;
DROP FUNCTION same_groups(regclass);
DROP EXTENSION pg_sift;
