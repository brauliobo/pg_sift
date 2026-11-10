-- a parent with one column of each kind the queries read through the connection (tier int, opened date, region text) and a column they filter on besides (name)
CREATE TABLE parent (id int PRIMARY KEY, tier int NOT NULL, opened date NOT NULL, region text NOT NULL, name text NOT NULL);
CREATE TABLE child  (id bigint PRIMARY KEY, parent_id int, amount numeric(12,2) NOT NULL, status text NOT NULL, at timestamp NOT NULL);
