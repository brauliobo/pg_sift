-- the dashboard shape: child -> parent -> source, parents identified by a (feed_id, external_key) pair that is meant to be unique,
-- an assignment table that scopes the sources a user may see, and attributes whose presence (non-blank) is counted
CREATE TABLE source     (id int PRIMARY KEY, kind text NOT NULL, region text NOT NULL);
CREATE TABLE parent     (id bigint PRIMARY KEY, feed_id int NOT NULL, external_key text NOT NULL, label text, city text, bucket_a int, flag_a boolean NOT NULL);
CREATE TABLE child      (id bigint PRIMARY KEY, parent_id bigint NOT NULL, feed_id int NOT NULL, created_at timestamp NOT NULL, kind text NOT NULL, score int, body text);
CREATE TABLE assignment (user_id int NOT NULL, feed_id int NOT NULL, PRIMARY KEY (user_id, feed_id));
