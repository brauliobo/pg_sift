-- notes with a title and a body, groups as the dimension; the text filter (BM25 in sift, tsvector + GIN natively) is on the title and on the body, then the join
CREATE TABLE grp  (id int PRIMARY KEY, kind text NOT NULL, tier int NOT NULL);
CREATE TABLE note (id bigint PRIMARY KEY, grp_id int NOT NULL, title text NOT NULL, body text NOT NULL, at timestamp NOT NULL);
