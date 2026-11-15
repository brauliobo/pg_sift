CREATE INDEX session_user_idx ON session (user_id);
CREATE INDEX session_started_idx ON session (started_at);
CREATE INDEX users_plan_idx ON users (plan);
