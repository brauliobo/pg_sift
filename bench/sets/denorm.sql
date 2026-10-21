-- Design 5 (docs/SET-FILTERS.md section 6): the attribute of the entity copied into the fact table, stored in insertion order and clustered by it.
--   psql -d sets -v sch=sets -f bench/sets/denorm.sql      after build.sql
\set ON_ERROR_STOP on
SET search_path = :sch, public;
SET maintenance_work_mem = '512MB';
SET max_parallel_maintenance_workers = 2;
CREATE EXTENSION IF NOT EXISTS pg_sift;

DROP TABLE IF EXISTS events_dn, events_dn_u CASCADE;
CREATE TABLE events_dn_u AS
SELECT v.id, v.dim_a_id, v.channel_id, v.lang_id, v.signal, v.event_date, v.score, v.deleted_at, e.country_id, e.category_id
FROM events v JOIN dim_a e ON e.id = v.dim_a_id ORDER BY v.id;
CREATE TABLE events_dn AS SELECT * FROM events_dn_u ORDER BY country_id, event_date, id;
CREATE INDEX events_dn_u_col ON events_dn_u USING sift_col (dim_a_id, channel_id, lang_id, signal, event_date, score, deleted_at, country_id, category_id);
CREATE INDEX events_dn_col ON events_dn USING sift_col (dim_a_id, channel_id, lang_id, signal, event_date, score, deleted_at, country_id, category_id);
CREATE INDEX events_dn_entity ON events_dn (dim_a_id);
CREATE INDEX events_dn_u_entity ON events_dn_u (dim_a_id);
VACUUM (ANALYZE) events_dn, events_dn_u;
SELECT relname, pg_size_pretty(pg_relation_size(oid)) AS size FROM pg_class WHERE relname IN ('events_dn', 'events_dn_u', 'events_dn_col', 'events_dn_u_col') ORDER BY 1;
