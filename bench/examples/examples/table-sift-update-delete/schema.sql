-- the fact is a sift_table (a column store table access method, no heap; the native engine builds a heap table with the same columns); the dimension is a heap table
CREATE TABLE dim  (id int PRIMARY KEY, grp text NOT NULL, state text NOT NULL, flag boolean NOT NULL, weight int NOT NULL);
CREATE TABLE fact (id bigint PRIMARY KEY, dim_id int, amount numeric(12,2) NOT NULL, status text NOT NULL, at timestamp NOT NULL) USING :table_am;
