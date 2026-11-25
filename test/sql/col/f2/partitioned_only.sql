-- A partitioned table holds no rows of its own and its sift_col index has no storage: FROM ONLY the parent plans and runs as an empty scan whether the aggregate pushdown is on or off, and the
-- statement over the partitions still answers from their indexes.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET sift.test_create_format1 = off;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
CREATE TABLE po (id int NOT NULL, d date NOT NULL, qty int) PARTITION BY RANGE (d);
CREATE TABLE po_a PARTITION OF po FOR VALUES FROM ('2024-01-01') TO ('2024-07-01') WITH (autovacuum_enabled = false);
CREATE TABLE po_b PARTITION OF po FOR VALUES FROM ('2024-07-01') TO ('2025-01-01') WITH (autovacuum_enabled = false);
INSERT INTO po SELECT g, date '2024-01-01' + g % 360, g % 7 FROM generate_series(1, 2000) g;
CREATE INDEX po_sift ON po USING sift_col (id, d, qty);
ANALYZE po;

SELECT relkind FROM pg_class WHERE oid = 'po_sift'::regclass;
SELECT count(*) AS n, sum(qty) AS q FROM ONLY po;
SELECT count(*) AS n, sum(qty) AS q FROM ONLY po WHERE d >= '2024-03-01';
SELECT qty, count(*) AS n FROM ONLY po GROUP BY qty;
SELECT count(*) AS n, sum(qty) AS q FROM po;
SELECT count(*) AS n, sum(qty) AS q FROM po WHERE d >= '2024-07-01';
SET sift.enable_agg = off;
SELECT count(*) AS n, sum(qty) AS q FROM ONLY po;
SELECT count(*) AS n, sum(qty) AS q FROM po;
DROP TABLE po;
DROP EXTENSION pg_sift;
