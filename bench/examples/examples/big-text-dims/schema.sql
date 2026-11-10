-- a dimension with large text attributes (a title and a description of about a kilobyte) joined to small facts
CREATE TABLE doc  (id int PRIMARY KEY, kind text NOT NULL, title text NOT NULL, description text NOT NULL);
CREATE TABLE page_view (id bigint PRIMARY KEY, doc_id int NOT NULL, seconds int NOT NULL, at timestamp NOT NULL);
