-- one fact and one dimension: the knobs of the registry row decide the shape (dimension size, hot key, null keys, clustering)
CREATE TABLE dim  (id int PRIMARY KEY, grp text NOT NULL, state text NOT NULL, flag boolean NOT NULL, weight int NOT NULL);
CREATE TABLE fact (id bigint PRIMARY KEY, dim_id int, amount numeric(12,2) NOT NULL, status text NOT NULL, at timestamp NOT NULL);
