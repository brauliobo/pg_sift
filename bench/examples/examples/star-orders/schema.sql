-- star: one fact (orders) with three dimensions (customer, item, store); no foreign keys, as in an analytics schema
CREATE TABLE customer (id int PRIMARY KEY, segment text NOT NULL, country text NOT NULL, tier int NOT NULL);
CREATE TABLE item     (id int PRIMARY KEY, category text NOT NULL, brand text NOT NULL, price numeric(10,2) NOT NULL);
CREATE TABLE store    (id int PRIMARY KEY, region text NOT NULL, kind text NOT NULL);
CREATE TABLE orders   (id bigint PRIMARY KEY, customer_id int NOT NULL, item_id int, place_id int, qty int NOT NULL,
                       amount numeric(12,2) NOT NULL, status text NOT NULL, created_at timestamp NOT NULL);
