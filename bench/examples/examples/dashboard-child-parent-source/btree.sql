CREATE INDEX child_parent_idx ON child (parent_id);
CREATE INDEX child_created_idx ON child (created_at);
CREATE INDEX child_source_idx ON child (feed_id);
CREATE INDEX parent_source_key_idx ON parent (feed_id, external_key);
CREATE INDEX parent_source_idx ON parent (feed_id);
