-- a status lookup of 5 rows keyed by text, referenced by every fact
CREATE TABLE status (code text PRIMARY KEY, label text NOT NULL, is_final boolean NOT NULL, rank int NOT NULL);
CREATE TABLE fact   (id bigint PRIMARY KEY, status_code text NOT NULL, amount numeric(12,2) NOT NULL, at timestamp NOT NULL, owner int NOT NULL);
