-- VACUUM of a table that was never analyzed rewrites the segments of its sift_col index and chooses the order arrays again; the choice needs statistics and
-- must not run ANALYZE inside the VACUUM (PostgreSQL refuses it): it chooses from what is known, and the statement ends
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, foreign_id int NOT NULL, event_date date, updated_at timestamp NOT NULL, deleted_at timestamp) WITH (autovacuum_enabled = false);
CREATE INDEX events_col ON events USING sift_col (dim_a_id, event_date, foreign_id, deleted_at, updated_at) WITH (write_buffer_limit = 256);
INSERT INTO events SELECT g, 1 + (g::bigint * 7919) % 710, 1 + g % 30, date '2025-01-01' + g % 300, timestamp '2025-01-01' + g * interval '1 second', NULL FROM generate_series(1, 150000) g;
SELECT count(*) > 8 AS many_segments FROM sift.col_segments('events_col');
VACUUM (FREEZE) events;
SELECT count(*) < 8 AS merged FROM sift.col_segments('events_col');
SELECT count(*) AS rows FROM events WHERE dim_a_id = 5;
DROP TABLE events;
DROP EXTENSION pg_sift;
