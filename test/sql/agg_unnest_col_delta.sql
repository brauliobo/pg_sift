-- The aggregate scan of a LATERAL jsonb_to_recordset reads a Format 2.0 sift_col as a snapshot sees it: an UPDATE that changes a few scalar leaves of a row appends a column delta to its part (the new
-- heap address of the row and the new cells) instead of ending the row, and the scan applies the delta as every other read of the index does, so the Top-N of a store and a CTE of
-- several grains give the rows of the heap, after the updates, after the deletes, after a rolled back writer and in the transaction that writes, serial and with workers allowed.
-- Every check prints t.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE o (id int PRIMARY KEY, tenant_id int NOT NULL, placed_at date NOT NULL, source text NOT NULL, deleted_at timestamp, items jsonb NOT NULL) WITH (fillfactor = 80, autovacuum_enabled = false);
INSERT INTO o
SELECT g, 1 + (g * 7) % 40, date '2030-01-01' + (g * 13) % 200, (ARRAY['web', 'ios', 'android'])[1 + g % 3], NULL,
	(SELECT coalesce(jsonb_agg(jsonb_build_object('category', (ARRAY['Apparel', 'Home', 'Garden', 'Toys', 'Pet', 'Books'])[1 + (g + k) % 6], 'qty', 1 + (g * k) % 5,
												   'price', ((g * 13 + k * 7) % 900) / 10.0, 'discount', (k % 3) / 2.0)), '[]')
	 FROM generate_series(1, g % 5) k)
FROM generate_series(1, 1500) g;
CREATE INDEX o_col ON o USING sift_col (id, tenant_id, placed_at, source, deleted_at, items) WITH (paths = 'items[]{category text, qty int4, price float8, discount float8}');
ANALYZE o;

CREATE FUNCTION shapes() RETURNS SETOF text LANGUAGE sql AS $$ SELECT unnest(ARRAY[
	'SELECT o.tenant_id, i.category, count(*) AS items, round(avg(i.price)::numeric, 3) AS avg_price FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, price float8)
	 WHERE o.deleted_at IS NULL GROUP BY o.tenant_id, i.category ORDER BY items DESC, o.tenant_id, i.category LIMIT 10',
	'SELECT o.tenant_id, count(*) AS n FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, price float8) GROUP BY 1',
	'WITH base AS (SELECT o.id AS order_id, o.placed_at, i.qty, i.qty * (i.price - i.discount) AS net
				   FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int, price float8, discount float8) WHERE o.deleted_at IS NULL)
	 SELECT ''day'' AS grain, placed_at AS bucket, count(DISTINCT order_id) AS orders, sum(qty) AS units, round(sum(net)::numeric, 3) AS revenue FROM base GROUP BY 2
	 UNION ALL SELECT ''month'', date_trunc(''month'', placed_at)::date, count(DISTINCT order_id), sum(qty), round(sum(net)::numeric, 3) FROM base GROUP BY 2']) $$;
CREATE FUNCTION answer(q text, agg boolean, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	PERFORM set_config('parallel_setup_cost', '0', true), set_config('parallel_tuple_cost', '0', true), set_config('min_parallel_table_scan_size', '0', true),
			set_config('min_parallel_index_scan_size', '0', true), set_config('seq_page_cost', '10000', true), set_config('random_page_cost', '10000', true);
	EXECUTE format('SELECT md5(string_agg(r::text, E''\n'' ORDER BY r::text)) FROM (%s) r', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION unnest_node(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; node boolean := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true), set_config('max_parallel_workers_per_gather', '0', true), set_config('seq_page_cost', '10000', true), set_config('random_page_cost', '10000', true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		node := node OR line LIKE '%Unnest: lateral columnar%';
	END LOOP;
	RETURN node;
END $$;
CREATE FUNCTION check_all(label text) RETURNS TABLE (step text, shape int, pushed boolean, serial boolean, workers boolean) LANGUAGE plpgsql AS $$
DECLARE q text;
BEGIN
	shape := 0;
	FOR q IN SELECT * FROM shapes() LOOP
		step := label;
		shape := shape + 1;
		pushed := unnest_node(q);
		serial := answer(q, true, 0) = answer(q, false, 0);
		workers := answer(q, true, 2) = answer(q, false, 0);
		RETURN NEXT;
	END LOOP;
END $$;

SELECT * FROM check_all('built');

UPDATE o SET tenant_id = 1 + (tenant_id * 7) % 40, placed_at = placed_at + 1 + id % 3, source = 'web' WHERE id % 29 = 0;
SELECT coalesce(sum(dead_rows), 0) = 0 AS patched_in_place FROM sift.parts('o_col');
SELECT * FROM check_all('updated');

UPDATE o SET items = items || '[{"category": "Toys", "qty": 2, "price": 9.9, "discount": 0}]' WHERE id % 31 = 0;
UPDATE o SET deleted_at = timestamp '2031-01-01' WHERE id % 37 = 0;
DELETE FROM o WHERE id % 41 = 0;
SELECT * FROM check_all('deleted');

BEGIN;
UPDATE o SET tenant_id = 1 + (tenant_id * 3) % 40 WHERE id % 17 = 0;
DELETE FROM o WHERE id % 19 = 0;
SELECT * FROM check_all('own transaction');
ROLLBACK;
SELECT * FROM check_all('rolled back');

VACUUM o;
SELECT * FROM check_all('vacuumed');
DROP TABLE o;
DROP FUNCTION shapes(), answer(text, boolean, int), unnest_node(text), check_all(text);
DROP EXTENSION pg_sift;
