-- sessions with an optional user: NULL user ids (anonymous), orphan ids (deleted users), users that never had a session; anti, left, right and full joins
CREATE TABLE users   (id int PRIMARY KEY, plan text NOT NULL, country text NOT NULL, created_at date NOT NULL);
CREATE TABLE session (id bigint PRIMARY KEY, user_id int, started_at timestamp NOT NULL, duration int NOT NULL, kind text NOT NULL);
