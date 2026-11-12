-- the entity index with the scope of two paths (direct grant, group grant through memberships); the journal indexes of the three pair relations come first
CREATE EXTENSION pg_sift;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, channel_id, region, score, amount)
  WITH (key_order = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
VACUUM (ANALYZE) events, grants, memberships;
