CREATE INDEX reading_col ON reading USING sift_col (device_id, ts, value, status);
CREATE INDEX device_col ON device USING sift_col (id, site_id, model);
CREATE INDEX site_col ON site USING sift_col (id, region, tz);
