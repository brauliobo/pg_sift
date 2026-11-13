CREATE INDEX reading_device_ts_idx ON reading (device_id, ts);
CREATE INDEX reading_ts_idx ON reading (ts);
CREATE INDEX device_site_idx ON device (site_id);
