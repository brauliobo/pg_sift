-- The NOTICE that says which column format minor an index is written in names the minor of the scope option (45, read from 5.0), whatever the other choices of the build need, and its hint does not
-- offer the setting that cannot lower it; an index without the option is unchanged.
CREATE EXTENSION pg_sift;
CREATE TABLE pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO pairs SELECT i, i % 7 FROM generate_series(1, 3000) i;
CREATE INDEX pairs_journal ON pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int, dim_a_id int, created_at timestamptz, kind int, value_num numeric);
INSERT INTO events SELECT i, i % 3000 + 1, timestamptz '2026-01-01' + (i || ' min')::interval, i % 5, i FROM generate_series(1, 20000) i;
SET client_min_messages = notice;
CREATE INDEX events_scope_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (scope = 'tenant_id: dim_a_id -> pairs(dim_a_id, tenant_id)');
CREATE INDEX events_plain_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num);
SELECT format AS minor, count(*) AS segments FROM sift.col_segments('events_scope_col') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
DROP TABLE events;
DROP TABLE pairs;
DROP EXTENSION pg_sift;
