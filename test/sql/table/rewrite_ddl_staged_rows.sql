-- A statement that rewrites a sift_table (SET TABLESPACE, SET UNLOGGED, SET LOGGED) sees the rows the transaction wrote before it: the inserts, the updates and, but for SET TABLESPACE, the deletes it
-- staged are in the table that the rewrite leaves, and the ones that follow it too.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE rewrite_ts LOCATION '';

CREATE TABLE orders (id int, quantity int) USING sift_table;
INSERT INTO orders SELECT g, g FROM generate_series(1, 100) g;

BEGIN;
INSERT INTO orders SELECT g, g FROM generate_series(1001, 1010) g;
UPDATE orders SET quantity = quantity + 1000 WHERE id = 6;
ALTER TABLE orders SET TABLESPACE rewrite_ts;
INSERT INTO orders VALUES (2001, 1);
COMMIT;
SELECT count(*), sum(quantity), max(quantity) FILTER (WHERE id = 6) AS updated, (SELECT reltablespace <> 0 FROM pg_class WHERE oid = 'orders'::regclass) AS moved FROM orders;

BEGIN;
INSERT INTO orders SELECT g, g FROM generate_series(3001, 3010) g;
DELETE FROM orders WHERE id = 7;
UPDATE orders SET quantity = quantity + 1000 WHERE id = 8;
ALTER TABLE orders SET UNLOGGED;
COMMIT;
SELECT count(*), sum(quantity), count(*) FILTER (WHERE id = 7) AS deleted, max(quantity) FILTER (WHERE id = 8) AS updated, (SELECT relpersistence FROM pg_class WHERE oid = 'orders'::regclass) AS persistence FROM orders;

BEGIN;
INSERT INTO orders SELECT g, g FROM generate_series(4001, 4010) g;
DELETE FROM orders WHERE id = 9;
UPDATE orders SET quantity = quantity + 1000 WHERE id = 10;
ALTER TABLE orders SET LOGGED;
COMMIT;
SELECT count(*), sum(quantity), count(*) FILTER (WHERE id = 9) AS deleted, max(quantity) FILTER (WHERE id = 10) AS updated, (SELECT relpersistence FROM pg_class WHERE oid = 'orders'::regclass) AS persistence FROM orders;

DROP TABLE orders;
DROP TABLESPACE rewrite_ts;
DROP EXTENSION pg_sift;
