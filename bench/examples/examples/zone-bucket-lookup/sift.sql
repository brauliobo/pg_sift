CREATE INDEX reading_col ON reading USING sift_col (zone_id, level, at);
CREATE INDEX zone_col ON zone USING sift_col (id, region, kind, capacity);
