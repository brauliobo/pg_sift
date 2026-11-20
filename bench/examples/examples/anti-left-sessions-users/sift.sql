CREATE INDEX session_col ON session USING sift_col (user_id, started_at, duration, kind);
CREATE INDEX users_col ON users USING sift_col (id, plan, country);
