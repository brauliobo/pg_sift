-- The shredder's throughput (docs/COL-SHRED.md, bench/steps/shred.sh): documents per second of sift._test_shred_count against
-- plain SQL on the same jsonb, and elements per second of the SIMD kernels per sift.simd level. Variables: ndocs, rounds.
\set ON_ERROR_STOP on
\pset tuples_only on
\pset format unaligned
\pset fieldsep '\t'
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE EXTENSION IF NOT EXISTS pg_sift_test;
SELECT setseed(0.5);
\set spec_orders 'attributes->locale text, metrics.* float4, items[]{category text, sku text, kind text, qty int4, price float4}, notes[]{type text, author_kind text}, tags[] text, total float8, paid bool'
\set spec_sparse 'k17 text, deep.a.b int8, k63 float8'

-- orders of 1..8 items: about 1 KiB of jsonb each
CREATE TABLE orders AS
	SELECT i AS id, jsonb_build_object('order_id', i, 'total', round((random() * 1000)::numeric, 2), 'paid', i % 3 = 0,
		'attributes', jsonb_build_object('locale', (ARRAY['en', 'pt', 'de', 'fr'])[1 + i % 4], 'channel', 'web', 'currency', 'EUR'),
		'metrics', jsonb_build_object('cpu', random(), 'io', random(), 'net', random(), 'latency', random() * 100),
		'items', (SELECT jsonb_agg(jsonb_build_object('category', (ARRAY['x', 'y', 'z'])[1 + (i + j) % 3], 'sku', 'SKU-' || (i * 31 + j),
				'kind', (ARRAY['p', 'q'])[1 + j % 2], 'qty', 1 + (i + j) % 9, 'price', round((random() * 99)::numeric, 2), 'note', 'unused text of the item'))
				FROM generate_series(1, 1 + i % 8) j),
		'notes', (SELECT jsonb_agg(jsonb_build_object('type', 'n' || (j % 3), 'author_kind', (ARRAY['type_d', 'type_b'])[1 + j % 2], 'text', repeat('lorem ', 5)))
				FROM generate_series(1, i % 4) j),
		'tags', jsonb_build_array('a' || i % 7, 'b' || i % 5)) AS doc
	FROM generate_series(1, :ndocs) i;
-- few documents with long arrays
CREATE TABLE bigs AS
	SELECT i AS id, jsonb_build_object('items', (SELECT jsonb_agg(jsonb_build_object('category', (ARRAY['x', 'y', 'z'])[1 + j % 3], 'sku', 'SKU-' || j,
				'kind', (ARRAY['p', 'q'])[1 + j % 2], 'qty', 1 + j % 9, 'price', round((random() * 99)::numeric, 2))) FROM generate_series(1, 500) j)) AS doc
	FROM generate_series(1, greatest(:ndocs / 50, 20)) i;
-- wide documents of which three paths are wanted
CREATE TABLE sparse AS
	SELECT i AS id, (SELECT jsonb_object_agg('k' || j, CASE j % 3 WHEN 0 THEN to_jsonb(j * 1.5) WHEN 1 THEN to_jsonb('value ' || j) ELSE jsonb_build_object('x', j, 'y', jsonb_build_array(1, 2, 3)) END)
					FROM generate_series(1, 80) j) || jsonb_build_object('deep', jsonb_build_object('a', jsonb_build_object('b', i), 'other', jsonb_build_array(1, 2, 3))) AS doc
	FROM generate_series(1, :ndocs) i;
VACUUM ANALYZE orders, bigs, sparse;
CREATE TEMP TABLE out (metric text, per_s float8, unit text);

-- best of :rounds, in units of work per second
CREATE FUNCTION best(q text, work float8, rounds int) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE t timestamptz; best float8 := NULL; s float8; x int8;
BEGIN
	FOR r IN 1..rounds + 1 LOOP			-- the first round warms the cache
		t := clock_timestamp();
		EXECUTE q INTO x;
		s := extract(epoch FROM clock_timestamp() - t);
		IF r > 1 AND (best IS NULL OR s < best) THEN best := s; END IF;
	END LOOP;
	RETURN work / best;
END $$;

INSERT INTO out SELECT 'orders: read the jsonb (one key)', best('SELECT sum(jsonb_array_length(doc -> ''items'')) FROM orders', :ndocs, :rounds), 'docs/s';
INSERT INTO out SELECT 'orders: shred (' || :'spec_orders' || ')', best(format('SELECT sum(sift._test_shred_count(doc, %L)) FROM orders', :'spec_orders'), :ndocs, :rounds), 'docs/s';
INSERT INTO out SELECT 'orders: jsonb_to_recordset(items) only', best('SELECT count(*) FROM orders, jsonb_to_recordset(doc -> ''items'') AS x(category text, sku text, kind text, qty int4, price float4)', :ndocs, :rounds), 'docs/s';
INSERT INTO out SELECT 'long arrays (500 items): shred items[]', best('SELECT sum(sift._test_shred_count(doc, ''items[]{category text, sku text, kind text, qty int4, price float4}'')) FROM bigs', (SELECT count(*) * 500 FROM bigs), :rounds), 'elements/s';
INSERT INTO out SELECT 'long arrays (500 items): jsonb_to_recordset', best('SELECT count(*) FROM bigs, jsonb_to_recordset(doc -> ''items'') AS x(category text, sku text, kind text, qty int4, price float4)', (SELECT count(*) * 500 FROM bigs), :rounds), 'elements/s';
INSERT INTO out SELECT 'wide documents (82 keys): read the jsonb (one key)', best('SELECT sum((doc #>> ''{deep,a,b}'')::int8) FROM sparse', :ndocs, :rounds), 'docs/s';
INSERT INTO out SELECT 'wide documents (82 keys): shred 3 paths', best(format('SELECT sum(sift._test_shred_count(doc, %L)) FROM sparse', :'spec_sparse'), :ndocs, :rounds), 'docs/s';

-- the kernels, per level the CPU supports (elements per second, 4096 elements in L1/L2 and 4 Mi elements from memory)
DO $$
DECLARE lv text; k text; n int; r float8;
BEGIN
	FOREACH lv IN ARRAY ARRAY['scalar', 'avx2', 'avx512'] LOOP
		EXECUTE format('SET sift.simd = %L', lv);
		CONTINUE WHEN sift._test_shred_kern_level() <> lv;
		FOREACH k IN ARRAY ARRAY['state_counts', 'state_bits', 'minmax_i64', 'minmax_f64', 'expand_parents'] LOOP
			FOREACH n IN ARRAY ARRAY[4096, 4194304] LOOP
				r := sift._test_shred_kern_rate(k, n, greatest(2, (400000000 / n)));
				INSERT INTO out VALUES (format('kernel %s, %s elements, %s', k, n, lv), r, 'elements/s');
			END LOOP;
		END LOOP;
	END LOOP;
	RESET sift.simd;
END $$;
SELECT metric, round(per_s), unit FROM out;
