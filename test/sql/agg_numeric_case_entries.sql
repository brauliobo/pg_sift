-- A sum of a CASE over a column of a lookup table and a numeric measure is evaluated by PostgreSQL once per distinct pair of the
-- tuple's key and measure (nearly every tuple when the measure has many values): the scan is priced with those entries and loses to
-- the normal plan, while the same CASE over an integer measure, evaluated without them, keeps the scan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '30s';

CREATE TABLE days AS SELECT 100 + d AS id, d / 7 AS week, (ARRAY['Sun','Mon','Tue','Wed','Thu','Fri','Sat'])[1 + d % 7] AS name FROM generate_series(0, 999) d;
CREATE TABLE sales AS SELECT 100 + (i * 7919) % 1000 AS day_id, 1 + i % 4 AS store, (i * 31 % 20000)::numeric(7, 2) AS price, 1 + i % 100 AS qty FROM generate_series(1, 200000) i;
CREATE INDEX sales_col ON sales USING sift_col (day_id, store, price, qty);
CREATE INDEX days_col ON days USING sift_col (id, week, name);
VACUUM (FREEZE, ANALYZE) sales;
ANALYZE days;

CREATE FUNCTION scan_used(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

SELECT m AS measure, scan_used(format('SELECT d.week, s.store, sum(CASE WHEN d.name = ''Sun'' THEN s.%s END) FROM sales s JOIN days d ON d.id = s.day_id GROUP BY 1, 2', m)) AS scan
FROM (VALUES ('price'), ('qty')) v(m) ORDER BY 1;
DROP FUNCTION scan_used(text);
DROP TABLE sales, days;
DROP EXTENSION pg_sift;
