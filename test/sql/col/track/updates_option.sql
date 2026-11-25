-- The index option track_changes names the column that identifies a row across updates: it must be a column of the index that a unique index of its own covers and that cannot be NULL.
-- On by default (auto: the primary key when the index holds it, else off with a NOTICE); named, or auto, it creates the tombstone log with the index and turns the point directory of the column on;
-- 'off' keeps the index as it was before 4.0 (no log segment, no option written).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int, u int NOT NULL UNIQUE, p int NOT NULL, d int NOT NULL) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX t_partial ON t (p) WHERE p > 0;
ALTER TABLE t ADD CONSTRAINT t_d_key UNIQUE (d) DEFERRABLE;
INSERT INTO t SELECT i, i % 10, i, i, i, i FROM generate_series(1, 2000) i;

-- default: the primary key is kept, the option is written
CREATE INDEX t_dflt ON t USING sift_col (id, g, v);
SELECT count(*) AS logs FROM sift.col_segments('t_dflt') WHERE kind = 'tombstones';
SELECT reloptions::text ~ 'track_changes=id' AS has_option FROM pg_class WHERE relname = 't_dflt';
DROP INDEX t_dflt;
-- off: nothing kept, nothing written
CREATE INDEX t_off ON t USING sift_col (id, g, v) WITH (track_changes = 'off');
SELECT count(*) AS logs FROM sift.col_segments('t_off') WHERE kind = 'tombstones';
SELECT reloptions::text ~ 'track_changes' AS has_option FROM pg_class WHERE relname = 't_off';
DROP INDEX t_off;

-- a primary key, named and automatic: one log segment, the key in the point directory
CREATE INDEX t_named ON t USING sift_col (id, g, v) WITH (track_changes = 'id');
SELECT count(*) AS logs FROM sift.col_segments('t_named') WHERE kind = 'tombstones';
SELECT split_part(o, '=', 1) AS opt, split_part(o, '=', 2) AS val FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_named' AND o ~ '^(track_changes|point_tid_columns)' ORDER BY 1;
DROP INDEX t_named;
CREATE INDEX t_auto ON t USING sift_col (id, g, v) WITH (track_changes = 'auto');
SELECT count(*) AS logs FROM sift.col_segments('t_auto') WHERE kind = 'tombstones';
SELECT o AS track_changes_written FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_auto' AND o ~ '^track_changes';
-- REINDEX keeps the option and the log
REINDEX INDEX t_auto;
SELECT count(*) AS logs FROM sift.col_segments('t_auto') WHERE kind = 'tombstones';
SELECT o AS track_changes_after_reindex FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_auto' AND o ~ '^track_changes';
-- ALTER INDEX checks the new value like CREATE INDEX does
ALTER INDEX t_auto SET (track_changes = 'g');
ALTER INDEX t_auto SET (track_changes = 'id');
DROP INDEX t_auto;

-- a second unique column
CREATE INDEX t_unique ON t USING sift_col (u, g, v) WITH (track_changes = 'u');
SELECT count(*) AS logs FROM sift.col_segments('t_unique') WHERE kind = 'tombstones';
DROP INDEX t_unique;

-- refused, each with what to do (a column without its unique index is let through with a NOTICE: a dump restores the index before the key)
CREATE INDEX t_bad ON t USING sift_col (g, v) WITH (track_changes = 'g');
DROP INDEX t_bad;
CREATE INDEX t_bad ON t USING sift_col (id, g, v) WITH (track_changes = 'v');
CREATE INDEX t_bad ON t USING sift_col (id, g) WITH (track_changes = 'v');
CREATE INDEX t_bad ON t USING sift_col (id, g, p) WITH (track_changes = 'p');
DROP INDEX t_bad;
CREATE INDEX t_bad ON t USING sift_col (id, g, d) WITH (track_changes = 'd');
DROP INDEX t_bad;
CREATE INDEX t_bad ON t USING sift_col (id, g) WITH (track_changes = 'nope');
CREATE INDEX t_bad ON t USING sift_col (id, g) WITH (track_changes = 'id, g');
CREATE INDEX t_bad ON t USING sift_col (g, v) WITH (track_changes = 'id');

-- a table with no primary key: auto says so and keeps no key
CREATE TABLE n (a int, b int);
INSERT INTO n SELECT i, i FROM generate_series(1, 100) i;
CREATE INDEX n_c ON n USING sift_col (a, b) WITH (track_changes = 'auto');
SELECT count(*) AS logs FROM sift.col_segments('n_c') WHERE kind = 'tombstones';
DROP TABLE n;

-- the build with the log does not change an answer, and the checks pass
CREATE INDEX t_c ON t USING sift_col (id, g, v) WITH (track_changes = 'id');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS n, sum(v) AS s FROM t WHERE g = 3;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT sift.col_verify('t_c');
-- CREATE INDEX CONCURRENTLY is allowed with track_changes alone
CREATE INDEX CONCURRENTLY t_cic ON t USING sift_col (id, g, v) WITH (track_changes = 'id');
SELECT count(*) AS logs FROM sift.col_segments('t_cic') WHERE kind = 'tombstones';
DROP INDEX t_cic;
-- RESET stops keeping a key
ALTER INDEX t_c RESET (track_changes);
SELECT o AS track_changes_after_reset FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_c' AND o ~ '^track_changes';
DROP TABLE t;
DROP EXTENSION pg_sift;
