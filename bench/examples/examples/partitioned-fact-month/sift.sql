CREATE INDEX event_col ON event USING sift_col (feed_id, ts, value, status);
CREATE INDEX source_col ON source USING sift_col (id, kind, region);
