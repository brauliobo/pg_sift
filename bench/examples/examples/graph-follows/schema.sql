-- edges between targets of one table: follows(follower, followee), duplicates allowed (a repeated follow), popular users
CREATE TABLE person  (id int PRIMARY KEY, country text NOT NULL, tier int NOT NULL);
CREATE TABLE follows (id bigint PRIMARY KEY, follower int NOT NULL, followee int NOT NULL, since date NOT NULL);
