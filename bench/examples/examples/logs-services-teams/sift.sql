CREATE INDEX log_col ON log USING sift_col (service_id, level, ts, message);
CREATE INDEX service_col ON service USING sift_col (id, name, team_id, env);
CREATE INDEX team_col ON team USING sift_col (id, owner, tier);
