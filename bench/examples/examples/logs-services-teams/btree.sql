CREATE INDEX log_service_ts_idx ON log (service_id, ts);
CREATE INDEX log_ts_idx ON log (ts);
CREATE INDEX service_team_idx ON service (team_id);
