-- the entity index only: the pairs are read through the member key of the entities (the join arm of the scope documentation)
CREATE EXTENSION pg_sift;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, channel_id, region, score, amount) WITH (key_order = 'dim_a_id, created_at');
VACUUM (ANALYZE) events;
