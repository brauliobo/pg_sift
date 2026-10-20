-- The fact table of the set filter benchmarks in insertion order, with the btrees a normal plan can use and the column index SiftAgg reads.
--   psql -v sch=sets -f build.sql      after gen.sql
\set ON_ERROR_STOP on
\timing on
SET search_path = :sch, public;
SET maintenance_work_mem = '512MB';
SET max_parallel_maintenance_workers = 2;
CREATE EXTENSION IF NOT EXISTS pg_sift;

DROP TABLE IF EXISTS events CASCADE;
CREATE TABLE events AS SELECT id, entity_id, channel_id, lang_id, sentiment, event_date, score, deleted_at FROM events_src ORDER BY id;
CREATE INDEX ON events (entity_id, event_date);
CREATE INDEX ON events (event_date);
CREATE INDEX events_col ON events USING sift_col (entity_id, channel_id, lang_id, sentiment, event_date, score, deleted_at);
VACUUM (ANALYZE) events, entities, group_members, cities, countries;
SELECT relname, pg_size_pretty(pg_relation_size(oid)) AS size FROM pg_class WHERE relname IN ('events', 'events_col', 'entities', 'group_members') ORDER BY 1;
