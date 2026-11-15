-- a dimension whose key is meant to be unique but is not (no unique constraint, a plain index): duplicates multiply joined rows
CREATE TABLE dim  (id int NOT NULL, grp text NOT NULL, state text NOT NULL, version int NOT NULL);
CREATE TABLE fact (id bigint PRIMARY KEY, dim_id int NOT NULL, amount numeric(12,2) NOT NULL, at timestamp NOT NULL);
