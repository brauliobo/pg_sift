-- snowflake chain: sale -> item -> category -> department
CREATE TABLE department (id int PRIMARY KEY, division text NOT NULL, name text NOT NULL);
CREATE TABLE category   (id int PRIMARY KEY, department_id int NOT NULL, kind text NOT NULL);
CREATE TABLE item       (id int PRIMARY KEY, category_id int NOT NULL, brand text NOT NULL, price numeric(10,2) NOT NULL);
CREATE TABLE sale       (id bigint PRIMARY KEY, item_id int NOT NULL, qty int NOT NULL, amount numeric(12,2) NOT NULL, sold_at timestamp NOT NULL);
