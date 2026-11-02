-- The index option row_key names the column that identifies a row across updates: it must be a column of the index that a unique index of its own covers and that cannot be NULL.
-- Off by default (no log segment, no option written); named, or auto on a primary key, it creates the tombstone log with the index and turns the point directory of the column on.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int, u int NOT NULL UNIQUE, p int NOT NULL, d int NOT NULL) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX t_partial ON t (p) WHERE p > 0;
ALTER TABLE t ADD CONSTRAINT t_d_key UNIQUE (d) DEFERRABLE;
INSERT INTO t SELECT i, i % 10, i, i, i, i FROM generate_series(1, 2000) i;

-- default: nothing kept, nothing written
CREATE INDEX t_off ON t USING sift_col (id, g, v);
SELECT count(*) AS logs FROM sift.col_segments('t_off') WHERE kind = 'tombstones';
SELECT reloptions::text ~ 'row_key' AS has_option FROM pg_class WHERE relname = 't_off';
DROP INDEX t_off;

-- a primary key, named and automatic: one log segment, the key in the point directory
CREATE INDEX t_named ON t USING sift_col (id, g, v) WITH (row_key = 'id');
SELECT count(*) AS logs FROM sift.col_segments('t_named') WHERE kind = 'tombstones';
SELECT split_part(o, '=', 1) AS opt, split_part(o, '=', 2) AS val FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_named' AND o ~ '^(row_key|point_tid_columns)' ORDER BY 1;
DROP INDEX t_named;
CREATE INDEX t_auto ON t USING sift_col (id, g, v) WITH (row_key = 'auto');
SELECT count(*) AS logs FROM sift.col_segments('t_auto') WHERE kind = 'tombstones';
SELECT o AS row_key_written FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_auto' AND o ~ '^row_key';
-- REINDEX keeps the option and the log
REINDEX INDEX t_auto;
SELECT count(*) AS logs FROM sift.col_segments('t_auto') WHERE kind = 'tombstones';
SELECT o AS row_key_after_reindex FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_auto' AND o ~ '^row_key';
-- ALTER INDEX checks the new value like CREATE INDEX does
ALTER INDEX t_auto SET (row_key = 'g');
ALTER INDEX t_auto SET (row_key = 'id');
DROP INDEX t_auto;

-- a second unique column
CREATE INDEX t_unique ON t USING sift_col (u, g, v) WITH (row_key = 'u');
SELECT count(*) AS logs FROM sift.col_segments('t_unique') WHERE kind = 'tombstones';
DROP INDEX t_unique;

-- refused, each with what to do
CREATE INDEX t_bad ON t USING sift_col (g, v) WITH (row_key = 'g');
CREATE INDEX t_bad ON t USING sift_col (id, g, v) WITH (row_key = 'v');
CREATE INDEX t_bad ON t USING sift_col (id, g) WITH (row_key = 'v');
CREATE INDEX t_bad ON t USING sift_col (id, g, p) WITH (row_key = 'p');
CREATE INDEX t_bad ON t USING sift_col (id, g, d) WITH (row_key = 'd');
CREATE INDEX t_bad ON t USING sift_col (id, g) WITH (row_key = 'nope');
CREATE INDEX t_bad ON t USING sift_col (id, g) WITH (row_key = 'id, g');
CREATE INDEX t_bad ON t USING sift_col (g, v) WITH (row_key = 'id');

-- a table with no primary key: auto says so and keeps no key
CREATE TABLE n (a int, b int);
INSERT INTO n SELECT i, i FROM generate_series(1, 100) i;
CREATE INDEX n_c ON n USING sift_col (a, b) WITH (row_key = 'auto');
SELECT count(*) AS logs FROM sift.col_segments('n_c') WHERE kind = 'tombstones';
DROP TABLE n;

-- the build with the log does not change an answer, and the checks pass
CREATE INDEX t_c ON t USING sift_col (id, g, v) WITH (row_key = 'id');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS n, sum(v) AS s FROM t WHERE g = 3;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT sift.col_verify('t_c');
-- CREATE INDEX CONCURRENTLY is allowed with row_key alone
CREATE INDEX CONCURRENTLY t_cic ON t USING sift_col (id, g, v) WITH (row_key = 'id');
SELECT count(*) AS logs FROM sift.col_segments('t_cic') WHERE kind = 'tombstones';
DROP INDEX t_cic;
-- RESET stops keeping a key
ALTER INDEX t_c RESET (row_key);
SELECT o AS row_key_after_reset FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_c' AND o ~ '^row_key';
DROP TABLE t;
DROP EXTENSION pg_sift;
