-- composite keys (order_id, line_no): lines are the fact, shipments are 1:1 with some lines, headers are 1:N
CREATE TABLE header   (id bigint PRIMARY KEY, customer_id int NOT NULL, status text NOT NULL, placed_on date NOT NULL);
CREATE TABLE order_line     (order_id bigint NOT NULL, line_no int NOT NULL, sku int NOT NULL, qty int NOT NULL, price numeric(10,2) NOT NULL, PRIMARY KEY (order_id, line_no));
CREATE TABLE shipment (order_id bigint NOT NULL, line_no int NOT NULL, carrier text NOT NULL, shipped_on date NOT NULL, PRIMARY KEY (order_id, line_no));
