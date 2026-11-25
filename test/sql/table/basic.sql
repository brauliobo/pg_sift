-- sift_table: the table access method end to end in one session. DML, MVCC under rollback and savepoints, row locks,
-- btree indexes on it, VACUUM, ANALYZE, TRUNCATE, CLUSTER-style rewrites. Differential counterpart: tbl_diff.
CREATE EXTENSION pg_sift;

CREATE TABLE orders (id int, store text, total numeric, items jsonb, note text) USING sift_table;
SELECT amname, amtype FROM pg_am WHERE amname = 'sift_table';
SELECT c.relname, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname = 'orders';

-- insert, select, ctid shape
INSERT INTO orders SELECT g, 'store' || g % 3, g * 1.5, jsonb_build_array(jsonb_build_object('sku', g)), 'note ' || g
FROM generate_series(1, 20) g;
SELECT count(*), sum(total), min(id), max(id) FROM orders;
SELECT id, store, total, items, note FROM orders WHERE id IN (1, 7, 20) ORDER BY id;

-- delete and update
DELETE FROM orders WHERE id % 5 = 0;
UPDATE orders SET total = total + 100, note = 'upd' WHERE id IN (1, 2, 3);
SELECT count(*) FROM orders;
SELECT id, total, note FROM orders WHERE id <= 4 ORDER BY id;
UPDATE orders SET note = 'again' WHERE id = 1;
SELECT id, note FROM orders WHERE id = 1;

-- rollback restores, commit keeps
BEGIN;
UPDATE orders SET note = 'rolled back' WHERE id = 2;
DELETE FROM orders WHERE id = 3;
INSERT INTO orders VALUES (1000, 'x', 1, '[]', 'ghost');
SELECT count(*) FROM orders WHERE note = 'rolled back' OR id IN (3, 1000);
ROLLBACK;
SELECT count(*) FROM orders WHERE note = 'rolled back' OR id = 1000;
SELECT id FROM orders WHERE id = 3;

-- savepoints
BEGIN;
INSERT INTO orders VALUES (2000, 'x', 1, '[]', 'outer');
SAVEPOINT s1;
INSERT INTO orders VALUES (2001, 'x', 1, '[]', 'inner');
UPDATE orders SET note = 'inner upd' WHERE id = 2000;
SAVEPOINT s2;
DELETE FROM orders WHERE id = 2001;
ROLLBACK TO s2;
SELECT id, note FROM orders WHERE id >= 2000 ORDER BY id;
ROLLBACK TO s1;
SELECT id, note FROM orders WHERE id >= 2000 ORDER BY id;
COMMIT;
SELECT id, note FROM orders WHERE id >= 2000 ORDER BY id;

-- own writes by command id: a cursor does not see later changes of its own transaction
BEGIN;
DECLARE c CURSOR FOR SELECT id FROM orders WHERE id < 5 ORDER BY id;
DELETE FROM orders WHERE id = 4;
FETCH ALL FROM c;
COMMIT;

-- row locks and RETURNING
SELECT id FROM orders WHERE id = 6 FOR UPDATE;
SELECT id FROM orders WHERE id = 6 FOR SHARE;
UPDATE orders SET note = 'ret' WHERE id = 6 RETURNING id, note;
DELETE FROM orders WHERE id = 6 RETURNING id;

-- btree on a sift_table: build over existing rows, then maintained
CREATE INDEX orders_id ON orders (id);
CREATE INDEX orders_store ON orders (store, id);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (costs off) SELECT note FROM orders WHERE id = 7;
SELECT id, note FROM orders WHERE id = 7;
SELECT id FROM orders WHERE store = 'store1' AND id < 10 ORDER BY id;
INSERT INTO orders VALUES (3000, 'store1', 1, '[]', 'late');
UPDATE orders SET note = 'moved' WHERE id = 7;
SELECT id, note FROM orders WHERE id IN (7, 3000) ORDER BY id;
RESET enable_bitmapscan;
SET enable_indexscan = off;
EXPLAIN (costs off) SELECT note FROM orders WHERE id > 15;
SELECT id FROM orders WHERE id > 15 ORDER BY id;
RESET enable_indexscan;
RESET enable_seqscan;

-- unique and primary key, ON CONFLICT
CREATE TABLE uq (k int PRIMARY KEY, v text) USING sift_table;
INSERT INTO uq VALUES (1, 'a'), (2, 'b');
INSERT INTO uq VALUES (1, 'dup');
INSERT INTO uq VALUES (1, 'dup') ON CONFLICT (k) DO NOTHING;
INSERT INTO uq VALUES (2, 'upsert') ON CONFLICT (k) DO UPDATE SET v = excluded.v;
SELECT * FROM uq ORDER BY k;

-- VACUUM and ANALYZE
SELECT count(*) FROM orders;
VACUUM orders;
SELECT count(*) FROM orders;
VACUUM (FULL) orders;
SELECT count(*) FROM orders;
ANALYZE orders;
SELECT reltuples > 0 AS analyzed FROM pg_class WHERE relname = 'orders';
SELECT attname, n_distinct IS NOT NULL AS has_stats FROM pg_stats WHERE tablename = 'orders' AND attname = 'store';

-- rows wider than a page go to overflow pages
INSERT INTO orders VALUES (4000, 'big', 1, '[]', repeat('x', 30000));
SELECT id, length(note) FROM orders WHERE id = 4000;
UPDATE orders SET note = repeat('y', 25000) WHERE id = 4000;
SELECT id, length(note), left(note, 3) FROM orders WHERE id = 4000;
DELETE FROM orders WHERE id = 4000;
SELECT count(*) FROM orders WHERE id = 4000;

-- TRUNCATE, also rolled back
BEGIN;
TRUNCATE orders;
SELECT count(*) FROM orders;
ROLLBACK;
SELECT count(*) > 0 AS kept FROM orders;
TRUNCATE orders;
SELECT count(*) FROM orders;
INSERT INTO orders VALUES (1, 'a', 1, '[]', 'after truncate');
SELECT id, note FROM orders;

-- CREATE TABLE AS, SELECT INTO, partitions, temp tables, ALTER ... SET ACCESS METHOD both ways
CREATE TABLE copy_h USING sift_table AS SELECT g AS id, g::text AS s FROM generate_series(1, 5) g;
SELECT count(*), sum(id) FROM copy_h;
CREATE TABLE p (id int, v text) PARTITION BY RANGE (id);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10) USING sift_table;
CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (10) TO (20);
INSERT INTO p SELECT g, 'v' || g FROM generate_series(0, 19) g;
SELECT tableoid::regclass, count(*) FROM p GROUP BY 1 ORDER BY 1;
CREATE TEMP TABLE tmp_t (id int) USING sift_table;
INSERT INTO tmp_t SELECT generate_series(1, 5);
SELECT count(*) FROM tmp_t;
CREATE TABLE heap_t (id int, v text);
INSERT INTO heap_t SELECT g, 'h' || g FROM generate_series(1, 10) g;
ALTER TABLE heap_t SET ACCESS METHOD sift_table;
SELECT a.amname, (SELECT count(*) FROM heap_t) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname = 'heap_t';
ALTER TABLE heap_t SET ACCESS METHOD heap;
SELECT a.amname, (SELECT count(*) FROM heap_t), (SELECT sum(id) FROM heap_t) FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname = 'heap_t';

-- DDL on the table: add / drop column, rename, reindex, drop
ALTER TABLE orders ADD COLUMN extra int DEFAULT 5;
SELECT id, extra FROM orders;
ALTER TABLE orders DROP COLUMN note;
SELECT * FROM orders;
REINDEX TABLE orders;
SELECT count(*) FROM orders;
DROP TABLE orders, uq, copy_h, p, tmp_t, heap_t;
-- numeric(p, s) columns keep every value of their scaled integer domain through the write buffer and the sealed column groups: zero, negatives, trailing
-- zeros of a digit, one to five base-10000 digits, the limits of the scale and of 18 digits
CREATE TABLE nk (id int, a numeric(12,2), b numeric(18,6), c numeric(10,0), d numeric(6,3)) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE nh (LIKE nk);
INSERT INTO nk SELECT g, round(((g * 7919) % 2000003 - 1000000) / 7.0, 2), round(((g * 104729) % 2000000011 - 1000000000) / 3.0, 6),
                  (g * 31) % 20001 - 10000, round(((g * 13) % 200001 - 100000) / 1000.0, 3) FROM generate_series(1, 20000) g;
INSERT INTO nk VALUES (20001, 0, 0, 0, 0), (20002, 9999999999.99, 999999999999.999999, 9999999999, 999.999), (20003, -9999999999.99, -999999999999.999999, -9999999999, -999.999),
                      (20004, 0.01, 0.000001, 1, 0.001), (20005, -0.01, -0.000001, -1, -0.001), (20006, 100.00, 1000000.000000, 10000, 100.000), (20007, 10000.50, 100000000.500000, 100000000, 10.500),
                      (20008, NULL, NULL, NULL, NULL);
INSERT INTO nh SELECT * FROM nk;
SELECT (SELECT count(*) FROM (SELECT * FROM nk EXCEPT SELECT * FROM nh) x) AS only_in_buffer, (SELECT count(*) FROM (SELECT * FROM nh EXCEPT SELECT * FROM nk) y) AS only_in_twin;
VACUUM nk;
SELECT sift.table_maintain('nk') IS NOT NULL AS sealed;
SELECT (SELECT count(*) FROM (SELECT * FROM nk EXCEPT SELECT * FROM nh) x) AS only_in_table, (SELECT count(*) FROM (SELECT * FROM nh EXCEPT SELECT * FROM nk) y) AS only_in_twin,
       (SELECT sum(a + b + c + d) FROM nk) = (SELECT sum(a + b + c + d) FROM nh) AS same_sum;
DROP TABLE nk, nh;
DROP EXTENSION pg_sift;
