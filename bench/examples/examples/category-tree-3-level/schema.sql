-- a 3-level category tree in ONE table (root -> branch -> leaf) and listings that reference leaves
CREATE TABLE category (id int PRIMARY KEY, parent_id int, depth int NOT NULL, name text NOT NULL);
CREATE TABLE listing  (id bigint PRIMARY KEY, category_id int NOT NULL, price numeric(10,2) NOT NULL, status text NOT NULL, listed_at timestamp NOT NULL);
