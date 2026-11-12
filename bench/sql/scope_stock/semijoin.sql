-- stock b-tree tuned for the semi join, nothing maintained: a covering (member, time) index that also serves the per-member latest-N probe,
-- and a partial index for the selective filter class
CREATE INDEX events_cover ON events (dim_a_id, created_at, id) INCLUDE (kind, score, amount);
CREATE INDEX events_filtered ON events (dim_a_id) WHERE channel_id = 9 AND region = 'DE';
VACUUM (ANALYZE) events;
