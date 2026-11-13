-- documents with a text body, authors as the dimension; the text filter is native tsvector (GIN) or sift BM25, then the join
CREATE TABLE author   (id int PRIMARY KEY, country text NOT NULL, tier int NOT NULL);
CREATE TABLE document (id bigint PRIMARY KEY, author_id int NOT NULL, body text NOT NULL, published_at timestamp NOT NULL);
