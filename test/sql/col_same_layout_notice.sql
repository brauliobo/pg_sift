-- CREATE INDEX of a sift_col index that lists columns another index of the table lists, with the same predicate and the same layout (key_order, key_cut, rollup_columns,
-- order arrays), says so once: one index over the union of the columns is smaller at the same read cost. Advice only: the index is built as written.
CREATE EXTENSION pg_sift;
SET client_min_messages = notice;
\set VERBOSITY terse
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, kind int, qty int, status text, created_at timestamptz);
INSERT INTO events SELECT i, i % 40, i % 5, i % 13, (ARRAY['new', 'open', 'held'])[1 + i % 3], timestamptz '2026-01-01' + i * interval '1 minute' FROM generate_series(1, 3000) i;
-- the narrow index is first: nothing to say
CREATE INDEX narrow ON events USING sift_col (dim_a_id, created_at, kind, qty) WITH (key_order = 'dim_a_id, created_at', rollup_columns = 'dim_a_id, kind: qty', track_changes = off);
-- the same layout over more columns, and a third: the first match is named, once
CREATE INDEX wide ON events USING sift_col (dim_a_id, created_at, kind, qty, status) WITH (key_order = 'dim_a_id, created_at', rollup_columns = 'dim_a_id, kind: qty', track_changes = off);
CREATE INDEX wider ON events USING sift_col (dim_a_id, created_at, kind, qty, status, id) WITH (key_order = 'dim_a_id, created_at', rollup_columns = 'dim_a_id, kind: qty', track_changes = off);
-- a layout that differs in one structure: no notice
CREATE INDEX other_key ON events USING sift_col (dim_a_id, created_at, kind, qty) WITH (key_order = 'created_at', rollup_columns = 'dim_a_id, kind: qty', track_changes = off);
CREATE INDEX other_rollup ON events USING sift_col (dim_a_id, created_at, kind, qty) WITH (key_order = 'dim_a_id, created_at', rollup_columns = 'kind, qty', track_changes = off);
-- no column in common
CREATE INDEX only_qty ON events USING sift_col (qty) WITH (track_changes = off);
CREATE INDEX only_status ON events USING sift_col (status) WITH (track_changes = off);
-- another predicate
CREATE INDEX open_only ON events USING sift_col (dim_a_id, created_at, kind, qty) WITH (key_order = 'dim_a_id, created_at', rollup_columns = 'dim_a_id, kind: qty', track_changes = off) WHERE status = 'open';
DROP INDEX narrow, wide, wider, other_key, other_rollup, only_qty, only_status, open_only;
-- the same predicate: the notice again
CREATE INDEX open_a ON events USING sift_col (dim_a_id, created_at, kind, qty, id) WITH (key_order = 'dim_a_id, created_at', rollup_columns = 'dim_a_id, kind: qty', track_changes = off) WHERE status = 'open';
CREATE INDEX open_b ON events USING sift_col (dim_a_id, created_at, kind, qty) WITH (key_order = 'dim_a_id, created_at', rollup_columns = 'dim_a_id, kind: qty', track_changes = off) WHERE status = 'open';
-- a rebuild says it again for the other index and never names the index itself (the copy REINDEX CONCURRENTLY builds), a btree is no sift_col index, client_min_messages = warning hides it
REINDEX INDEX open_a;
REINDEX INDEX CONCURRENTLY open_b;
CREATE INDEX events_kind_btree ON events (kind, qty);
SET client_min_messages = warning;
REINDEX INDEX open_a;
RESET client_min_messages;
DROP TABLE events;
DROP EXTENSION pg_sift;
