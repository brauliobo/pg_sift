-- The options of a sift_col index that list columns take any column name: a name with a space or a tab, a quoted one ("a,b"), blanks of any kind around
-- a name; a name that is not a column of the index is named in the error; a repeated name is the same column. The deprecated options accept any value.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
-- Format 1: the options this test names are those of a Format 1 index.
SET sift.test_create_format1 = on;

DO $$ BEGIN
	EXECUTE format('CREATE TABLE cn (id int, %I date, %I int, amount int, %I int, %I int, %I text) WITH (autovacuum_enabled = off)',
		'Order Date', 'Cust Id', E'ta\tb', 'a,b', 'Cust Name');
END $$;
INSERT INTO cn SELECT i, date '2024-01-01' + i % 40, i % 7, i, i % 3, i % 5, 'n' || i % 11 FROM generate_series(1, 4000) i;

-- the result of one CREATE INDEX with one option: ok, or the error
CREATE FUNCTION try_option(opt text, val text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE INDEX cn_try ON cn USING sift_col (id, "Order Date", "Cust Id", amount, %I, "a,b", "Cust Name") WITH (%I = %L)', E'ta\tb', opt, val);
	PERFORM sift.col_verify('cn_try'::regclass, true);
	EXECUTE 'DROP INDEX cn_try';
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN sqlerrm;
END $$;

SELECT opt, val, try_option(opt, val) AS result FROM (VALUES
	('key_cut', '"Order Date"'),
	('key_cut', 'Order Date'),
	('key_cut', E'amount,\t"Order Date"'),
	('key_cut', '"Order Date", amount'),
	('key_cut', 'Order'),
	('rollup_columns', '"Cust Id", "Order Date"'),
	('rollup_columns', E'"Cust Id",\n"Order Date"'),
	('rollup_columns', '"Cust Id", nope'),
	('calendar_columns', '"Order Date"'),
	('key_columns', '"Cust Id"'),
	('key_columns', E'"Cust Id",\tamount'),
	('key_columns', E'\tamount ,  "Cust Id"  '),
	('key_columns', '"a,b"'),
	('key_columns', E'"ta\tb"'),
	('key_columns', 'amount, amount'),
	('key_columns', 'nope'),
	('key_columns', '"Cust Id", "nope"'),
	('key_columns', '"Cust Id'),
	('key_columns', '"Cust Id"x'),
	('key_columns', 'amount,'),
	('key_columns', 'amount,,id'),
	('order_array_columns', '"Order Date", amount'),
	('hll_columns', '"Cust Id"'),
	('count_columns', '"Cust Name"'),
	('return_columns', '"Cust Name"'),
	('return_columns', 'cust name'),
	('key_order', '"Order Date"'),
	('key_order', '"Cust Id", "Order Date"'),
	('key_order', '"Cust Id", "Order Date", amount'),
	('key_order', '"Cust Id",')
) AS t(opt, val) ORDER BY opt, val;

-- the options an automatic choice writes are read back: an index over names with spaces builds, and answers as the heap
CREATE INDEX cn_auto ON cn USING sift_col (id, "Order Date", "Cust Id", amount);
REINDEX INDEX cn_auto;
CREATE INDEX cn_named ON cn USING sift_col (id, "Order Date", "Cust Id", amount)
	WITH (key_cut = '"Order Date"', rollup_columns = '"Cust Id", "Order Date"', key_columns = '"Cust Id"');
SELECT sift.col_verify('cn_auto', true), sift.col_verify('cn_named', true);
SET max_parallel_workers_per_gather = 0;
SET sift.enable_agg = off;
SELECT "Cust Id", count(*), sum(amount) FROM cn WHERE "Order Date" >= date '2024-01-20' GROUP BY 1 ORDER BY 1;
SET sift.enable_agg = on;
SELECT "Cust Id", count(*), sum(amount) FROM cn WHERE "Order Date" >= date '2024-01-20' GROUP BY 1 ORDER BY 1;
-- the same options after a dump, a session of their own: the options text is the one the index was made with
SELECT reloptions FROM pg_class WHERE relname = 'cn_named';

-- the deprecated options accept every value of their type, as an index of an older release may store one; each is ignored
CREATE INDEX cn_old ON cn USING sift_col (id, amount) WITH (key_cut_rows = 100000, point_columns = 'nowhere', order_columns = 'x, y');
ALTER INDEX cn_old SET (key_cut_rows = 2000000000);
SELECT reloptions FROM pg_class WHERE relname = 'cn_old';
ALTER INDEX cn_old RESET (key_cut_rows, point_columns, order_columns);
SELECT reloptions FROM pg_class WHERE relname = 'cn_old';

DROP TABLE cn;
DROP FUNCTION try_option(text, text);
DROP EXTENSION pg_sift;
