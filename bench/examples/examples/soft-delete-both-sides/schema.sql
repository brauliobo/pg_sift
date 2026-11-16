-- soft deletes on the fact and on the dimension: rows stay, deleted_at says they are gone; every query filters both sides
CREATE TABLE dim  (id int PRIMARY KEY, grp text NOT NULL, state text NOT NULL, deleted_at timestamp);
CREATE TABLE fact (id bigint PRIMARY KEY, dim_id int NOT NULL, amount numeric(12,2) NOT NULL, at timestamp NOT NULL, deleted_at timestamp);
