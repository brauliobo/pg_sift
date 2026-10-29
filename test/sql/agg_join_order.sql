-- The scan of an inner join reads the table that is dear to load as its own rows and loads the small ones as lookups, whichever table the FROM list
-- starts with.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE customers AS SELECT i AS id, (ARRAY['A', 'B', 'C', 'D'])[1 + i % 4] AS segment FROM generate_series(1, 5000) i;
CREATE TABLE sales AS
SELECT i AS id, 1 + (i::bigint * 7919 % 5000)::int AS customer_id, DATE '2024-01-01' + (i * 13) % 700 AS ordered, DATE '2024-01-01' + (i * 13) % 700 + (i * 31) % 40 AS shipped,
	   (i % 97)::float8 AS amount
FROM generate_series(1, 300000) i;
CREATE INDEX customers_col ON customers USING sift_col (id, segment);
CREATE INDEX sales_col ON sales USING sift_col (customer_id, ordered, shipped, amount);
VACUUM (FREEZE, ANALYZE) customers, sales;
\ir test/agg_helpers.sql
-- the index the Custom Scan (SiftAgg) of a plan reads
CREATE FUNCTION scanned(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; found text;
BEGIN
  PERFORM set_config('sift.enable_agg', 'on', true);
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP found := coalesce(found, substring(line from 'Index: (\w+)')); END LOOP;
  RETURN found;
END $$;
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT outcome(q, false) = outcome(q, true) $$;
CREATE TABLE shapes (name text, q text);
INSERT INTO shapes VALUES
 ('segment', $q$ SELECT segment, count(*) AS n, sum(amount) AS total FROM %s WHERE sales.customer_id = customers.id AND ordered < DATE '2025-03-01' GROUP BY 1 $q$),
 ('late', $q$ SELECT segment, count(*) AS n FROM %s WHERE sales.customer_id = customers.id AND shipped > ordered + 30 AND segment IN ('A', 'B') GROUP BY 1 $q$);
-- both orders of the FROM list scan the large table and give the answers of the normal plan
SELECT name, scanned(format(q, 'customers, sales')) AS dimension_first, scanned(format(q, 'sales, customers')) AS fact_first,
       same(format(q, 'customers, sales')) AS dimension_first_same, same(format(q, 'sales, customers')) AS fact_first_same
FROM shapes ORDER BY name;
-- a lookup table read whole from the leaves of a column index that holds its columns is priced below the same read from the heap
CREATE TEMP TABLE load_price (leaves bool, scan_cost float8);
SET sift.agg_lookup_columns = on;
INSERT INTO load_price SELECT true, scan_cost FROM sift.explain_why(format((SELECT q FROM shapes WHERE name = 'segment'), 'sales, customers')) WHERE arm = 'single';
SET sift.agg_lookup_columns = off;
INSERT INTO load_price SELECT false, scan_cost FROM sift.explain_why(format((SELECT q FROM shapes WHERE name = 'segment'), 'sales, customers')) WHERE arm = 'single';
RESET sift.agg_lookup_columns;
SELECT (SELECT scan_cost FROM load_price WHERE leaves) < (SELECT scan_cost FROM load_price WHERE NOT leaves) AS cheaper_from_the_leaves;
DROP FUNCTION scanned(text), same(text), outcome(text, bool), runs_in_scan(text), pushed(text), runq(text, bool);
DROP TABLE shapes, sales, customers;
DROP EXTENSION pg_sift;
