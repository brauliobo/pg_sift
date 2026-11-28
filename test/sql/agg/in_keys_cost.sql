-- The scan tests a row against the constants of an IN list on an integer column by a binary search of them (a bitmap where the values of a chunk span few enough:
-- col/granule/cfilt/kern.c): about 6 ns a level for each row it reads, so 256 keys spread over a range of a million cost the scan 7.5 ms of 200,000 rows where 8 keys
-- cost it 0.4 ms. The scan is priced for the search, and a list on a small result keeps the index plan (0.2 ms), over a wide range or a narrow one.
-- The statements run with enable_agg on and off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE items (id int PRIMARY KEY, key int NOT NULL, kind int NOT NULL, amount float8 NOT NULL);
SELECT setseed(0.42);
INSERT INTO items SELECT g, 1 + (random() * 999999)::int, g % 9, random() * 100 FROM generate_series(1, 200000) g;
CREATE INDEX items_key ON items (key);
CREATE INDEX items_col ON items USING sift_col (id, key, kind, amount) WITH (point_tid_columns = 'off');
VACUUM (FREEZE, ANALYZE) items;

CREATE FUNCTION in_query(span int, n int) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT kind, count(*), max(amount) FROM items WHERE key IN (%s) GROUP BY 1',
	              (SELECT string_agg((1 + g * (span / n))::text, ',') FROM generate_series(0, n - 1) g))
$$;
CREATE FUNCTION scan_cost(q text) RETURNS numeric LANGUAGE sql AS $$ SELECT scan_cost::numeric FROM sift.explain_why(q) LIMIT 1 $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.differences_agg(q) $$;

-- 256 keys against 8 over the same range: the search of the longer list costs more by a good share of the normal plan's price
SELECT scan_cost(in_query(1000000, 256)) - scan_cost(in_query(1000000, 8)) > 500 AS search_priced;

-- 256 keys over the wide range and over the narrow one keep the index plan
SELECT span, bool_or(used) AS scan FROM (VALUES (256), (1000000)) v(span), LATERAL sift.explain_why(in_query(span, 256)) GROUP BY span ORDER BY span;
SELECT differences(in_query(1000000, 256)) AS differences_wide, differences(in_query(256, 256)) AS differences_narrow;

DROP FUNCTION in_query(int, int), scan_cost(text), differences(text);

-- A generic plan does not see the array of `key = ANY($1)`: PostgreSQL prices it for 10 elements. The scan of the sift_col index tests every row of the units it reads against
-- the list (256 keys over a range of a million: 9 ms of 200,000 rows), so a custom plan of 256 keys reads the b-tree (0.3 ms), and the generic plan, the same b-tree priced for
-- 10 keys, is taken after five of them, as fast; 8 keys keep the custom plans.
CREATE FUNCTION key_list(n int) RETURNS text LANGUAGE sql AS $$
	SELECT (SELECT string_agg(key::text, ',') FROM (SELECT key FROM items ORDER BY id LIMIT n) k)
$$;

-- the plan cache: five custom plans, then the generic plan if it is not dearer than their average
CREATE FUNCTION executions(n int) RETURNS TABLE (generic_plans bigint, custom_plans bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	DEALLOCATE ALL;
	PREPARE by_keys (int4[]) AS SELECT kind, count(*), max(amount) FROM items WHERE key = ANY ($1) GROUP BY 1;
	FOR i IN 1..12 LOOP
		EXECUTE format('EXECUTE by_keys(%L)', '{' || key_list(n) || '}');
	END LOOP;
	RETURN QUERY SELECT p.generic_plans, p.custom_plans FROM pg_prepared_statements p WHERE p.name = 'by_keys';
END $$;

-- the keys of the list are spread over a range of a million
SELECT n, generic_plans, custom_plans FROM unnest(ARRAY[8, 256]) n, LATERAL executions(n) ORDER BY n;

DEALLOCATE ALL;
DROP FUNCTION executions(int), key_list(int);

DROP TABLE items;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
