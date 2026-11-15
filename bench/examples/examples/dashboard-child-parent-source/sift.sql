CREATE INDEX child_col ON child USING sift_col (parent_id, feed_id, created_at, kind, score, body);
CREATE INDEX parent_col ON parent USING sift_col (id, feed_id, external_key, label, city, bucket_a, flag_a);
CREATE INDEX source_col ON source USING sift_col (id, kind, region);
CREATE INDEX assignment_col ON assignment USING sift_col (user_id, feed_id);
