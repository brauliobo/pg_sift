-- Fixtures of scripts/coldiff.sh: a small orders table (a few thousand rows of the shape of bench/sql/analytics: scalar columns plus jsonb
-- objects and arrays of objects), the helper tables of its queries, the edge-case tables of the trap queries and the DML of the
-- scenarios (heap states: inserts, updates, deletes, aborts, HOT, recycled TIDs).
--
--   psql -v n=4000 -v seed=0 -f fixture.sql
--
-- Every draw is a hash of (order id, draw number, seed): the same arguments give the same rows on every run.
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 4000
\endif
\if :{?seed}
\else
\set seed 0
\endif
CREATE SCHEMA IF NOT EXISTS coldiff;

CREATE FUNCTION coldiff.u(id bigint, draw int, seed int DEFAULT 0) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
	AS $$ SELECT (hashint8extended(id, draw * 1000003::bigint + seed) & 16777215)::float8 / 16777216 $$;

DROP TABLE IF EXISTS orders, orders_odd, orders_part, stores, metrics_map, categories_map, kinds_map, analytics_sets, t_int, t_float, t_nan, t_text, t_dist,
	map_dup, map_null, map_case, map_part, map_empty, map_vals, map_nums CASCADE;

CREATE TABLE orders (
	id integer NOT NULL, store_id integer NOT NULL, customer_id integer, placed_at date NOT NULL, status text, source text NOT NULL,
	country_code varchar(2), lang text, total numeric(12,2), discount numeric(12,2),
	attributes jsonb NOT NULL DEFAULT '{}',   -- object: channel, device, locale and normalized (true: the items carry canonical names)
	metrics jsonb NOT NULL DEFAULT '{}',      -- object with dynamic keys and float values
	items jsonb NOT NULL DEFAULT '[]',        -- array of objects {category, sku, kind, qty, price, discount}
	notes jsonb NOT NULL DEFAULT '[]',        -- array of objects {type, author_role, text}
	memo text,                                -- free text, the column of the BM25 index
	deleted_at timestamp
) WITH (fillfactor = 80);

-- orders first_id .. last_id: store ~ u^2 over 400, source ~ u^2 over 30, dates 2022-01-01 .. 2025-10-31, 12 languages, 60 countries,
-- 0..8 items (mean 3.2), 1..6 metrics, 0..3 notes, 2% soft-deleted, 70% with normalized items; NULLs in customer, status, country, in
-- the kind of the items (JSON null, or the key absent in 10% of the orders) and 3% of the orders have no metric at all
CREATE FUNCTION coldiff.fx_orders(first_id int, last_id int, seed int DEFAULT 0) RETURNS bigint LANGUAGE sql AS $$
	WITH ins AS (
		INSERT INTO orders (id, store_id, customer_id, placed_at, status, source, country_code, lang, total, discount, attributes, metrics,
							items, notes, memo, deleted_at)
		SELECT g.id, p.store_id, CASE WHEN coldiff.u(g.id, 13, seed) < 0.1 THEN NULL ELSE 1 + floor(coldiff.u(g.id, 14, seed) * 500)::int END,
			   p.placed_at, CASE WHEN coldiff.u(g.id, 15, seed) < 0.05 THEN NULL
								 ELSE (ARRAY['paid','shipped','delivered','cancelled','refunded'])[1 + floor(power(coldiff.u(g.id, 16, seed), 1.5) * 5)::int] END,
			   p.src, CASE WHEN coldiff.u(g.id, 17, seed) < 0.02 THEN NULL ELSE p.cc END, p.lang,
			   round((10 + 490 * power(coldiff.u(g.id, 18, seed), 2))::numeric, 2),
			   round((10 + 490 * power(coldiff.u(g.id, 18, seed), 2) * coldiff.u(g.id, 19, seed) * 0.2)::numeric, 2),
			   jsonb_build_object('channel', (ARRAY['web','mobile','pos','marketplace'])[1 + floor(coldiff.u(g.id, 20, seed) * 4)::int],
								  'device', (ARRAY['desktop','phone','tablet','kiosk'])[1 + floor(coldiff.u(g.id, 21, seed) * 4)::int], 'locale', p.lang)
				 || CASE WHEN p.normalized THEN '{"normalized": true}'::jsonb ELSE '{}'::jsonb END,
			   CASE WHEN coldiff.u(g.id, 22, seed) < 0.03 THEN '{}'::jsonb ELSE
				   (SELECT jsonb_object_agg((ARRAY['overall','delivery_speed','packaging','support','value','quality','size_fit','accuracy',
												   'returns_ease','checkout','communication','freshness'])[1 + (p.rs + i) % 12],
											round((1 + 4 * sqrt(coldiff.u(g.id, 100 + i, seed)))::numeric, 1))
					FROM generate_series(0, p.nr - 1) i) END,
			   CASE WHEN coldiff.u(g.id, 23, seed) < 0.1 THEN jsonb_strip_nulls(it.items) ELSE it.items END,
			   (SELECT coalesce(jsonb_agg(jsonb_build_object('type', (ARRAY['reply','comment','internal'])[1 + floor(coldiff.u(g.id, 500 + j, seed) * 3)::int],
															 'author_role', (ARRAY['staff','customer'])[1 + floor(coldiff.u(g.id, 600 + j, seed) * 2)::int],
															 'text', 'note ' || j) ORDER BY j), '[]'::jsonb)
				FROM generate_series(0, floor(coldiff.u(g.id, 24, seed) * 4)::int - 1) j),
			   (SELECT string_agg((ARRAY['fast','late','damaged','refund','gift','invoice','exchange','missing','perfect','broken','express','pickup',
										 'thanks','wrong','size','color','clean','wifi','box','label'])[1 + floor(coldiff.u(g.id, 700 + w, seed) * 20)::int], ' ')
				FROM generate_series(1, 3 + (g.id % 5)) w),
			   CASE WHEN coldiff.u(g.id, 6, seed) < 0.02 THEN timestamp '2025-11-01 00:00:00' END
		FROM generate_series(first_id, last_id) AS g(id)
		CROSS JOIN LATERAL (SELECT 1 + floor(power(coldiff.u(g.id, 1, seed), 2) * 400)::int AS store_id,
								   date '2022-01-01' + floor(1400 * sqrt(coldiff.u(g.id, 3, seed)))::int AS placed_at,
								   (ARRAY['web','ios','android','pos','phone'] || ARRAY(SELECT 'marketplace_' || lpad(i::text, 2, '0') FROM generate_series(1, 25) i))
									   [1 + floor(power(coldiff.u(g.id, 2, seed), 2) * 30)::int] AS src,
								   (ARRAY['en','de','fr','es','it','pt','nl','ru','ja','zh','sv','pl'])[1 + floor(power(coldiff.u(g.id, 4, seed), 2) * 12)::int] AS lang,
								   (ARRAY['US','GB','DE','FR','ES','IT','NL','PT','BR','MX','CA','AU','JP','CN','IN','RU','PL','SE','NO','DK',
										  'FI','CH','AT','BE','IE','GR','TR','AE','SA','EG','ZA','AR','CL','CO','PE','KR','TH','VN','ID','MY',
										  'SG','PH','NZ','CZ','HU','RO','BG','HR','RS','UA','IL','MA','TN','KE','NG','PK','BD','LK','IS','LU'])
									   [1 + floor(power(coldiff.u(g.id, 5, seed), 2) * 60)::int] AS cc,
								   coldiff.u(g.id, 12, seed) < 0.7 AS normalized,
								   floor(power(coldiff.u(g.id, 9, seed), 1.5) * 9)::int AS ni,
								   1 + floor(coldiff.u(g.id, 7, seed) * 6)::int AS nr,
								   floor(coldiff.u(g.id, 8, seed) * 12)::int AS rs) p
		CROSS JOIN LATERAL (
			SELECT coalesce(jsonb_agg(jsonb_build_object(
					'category', CASE WHEN p.normalized THEN c.cat ELSE lower(c.cat) END, 'sku', 'SKU-' || (1 + floor(coldiff.u(g.id, 800 + j, seed) * 200)::int),
					'kind', CASE WHEN p.normalized THEN c.kind ELSE upper(c.kind) END, 'qty', 1 + floor(coldiff.u(g.id, 900 + j, seed) * 5)::int,
					'price', c.price, 'discount', CASE WHEN coldiff.u(g.id, 1000 + j, seed) < 0.3 THEN round(c.price * 0.1, 1) ELSE 0 END) ORDER BY j), '[]'::jsonb) AS items
			FROM generate_series(0, p.ni - 1) j
			CROSS JOIN LATERAL (SELECT (ARRAY['Apparel','Shoes','Electronics','Home','Beauty','Toys','Sports','Books','Grocery','Garden','Pet','Office','Auto','Jewelry','Baby'])
										   [1 + floor(power(coldiff.u(g.id, 200 + j, seed), 2) * 15)::int] AS cat,
									   CASE WHEN coldiff.u(g.id, 300 + j, seed) < 0.3 THEN NULL
											ELSE (ARRAY['standard','express','pickup','digital','subscription','preorder','bundle','gift'])[1 + floor(coldiff.u(g.id, 310 + j, seed) * 8)::int] END AS kind,
									   round((5 + 295 * power(coldiff.u(g.id, 400 + j, seed), 2))::numeric, 1) AS price) c) it
		RETURNING 1)
	SELECT count(*) FROM ins $$;

SELECT coldiff.fx_orders(1, :n, :seed);

CREATE TABLE stores (id integer PRIMARY KEY, kind text, country_code text, name text);
INSERT INTO stores SELECT i, (ARRAY['flagship','outlet','popup','online','warehouse'])[1 + hashint8extended(i, 41) % 5],
	(ARRAY['US','GB','DE','FR','ES','IT'])[1 + hashint8extended(i, 42) % 6], 'Store ' || i FROM generate_series(1, 400) i;

-- the keys of the metrics of a source -> metric and aspect (about one in seven has no row: the split of the key is the fallback)
CREATE TABLE metrics_map (source text NOT NULL, metric_key text NOT NULL, mapped_metric text, mapped_aspect text);
INSERT INTO metrics_map
SELECT s, key, initcap(split_part(key, '_', 1)), nullif(initcap(split_part(key, '_', 2)), '')
FROM unnest(ARRAY['web','ios','android','pos','phone'] || ARRAY(SELECT 'marketplace_' || lpad(i::text, 2, '0') FROM generate_series(1, 25) i)) s,
	 unnest(ARRAY['overall','delivery_speed','packaging','support','value','quality','size_fit','accuracy','returns_ease','checkout','communication','freshness']) key
WHERE hashint8extended(length(s) * 100 + length(key), 31) % 7 <> 0;
CREATE TABLE categories_map (category text NOT NULL, mapped_category text NOT NULL);
INSERT INTO categories_map SELECT lower(c), CASE WHEN c IN ('Pet', 'Auto') THEN '' ELSE c END
FROM unnest(ARRAY['Apparel','Shoes','Electronics','Home','Beauty','Toys','Sports','Books','Grocery','Garden','Pet','Office','Auto','Jewelry','Baby']) c;
CREATE TABLE kinds_map (kind text NOT NULL, mapped_kind text NOT NULL);
INSERT INTO kinds_map SELECT upper(k), CASE WHEN k = 'gift' THEN '' ELSE k END
FROM unnest(ARRAY['standard','express','pickup','digital','subscription','preorder','bundle','gift']) k;

-- the traps of the lookup joins (the key is a category or a kind of the items): a key with two rows, NULL keys (never equal to anything),
-- keys that differ by case only, a few of the keys, no row
CREATE TABLE map_dup (category text, tag text);
INSERT INTO map_dup VALUES ('Apparel', 'a'), ('Apparel', 'b'), ('apparel', 'a'), ('apparel', 'b'), ('Home', 'a'), ('Shoes', 'a'), ('Shoes', 'a');
CREATE TABLE map_null (category text, tag text);
INSERT INTO map_null VALUES ('Home', 'h'), ('express', 'e'), (NULL, 'null key'), ('Shoes', NULL), (NULL, NULL);
CREATE TABLE map_case (category text, tag text);
INSERT INTO map_case VALUES ('Home', 'exact'), ('home', 'lower'), ('HOME', 'upper'), ('Shoes', 'exact'), ('EXPRESS', 'upper'), ('express', 'lower');
CREATE TABLE map_part (category text, tag text);
INSERT INTO map_part VALUES ('Apparel', 'a'), ('Home', 'h'), ('Toys', 't');
CREATE TABLE map_empty (category text, tag text);
-- values of every type to aggregate through a lookup join: a key with two rows (case differs in the label), NULL values, a key without a row
CREATE TABLE map_vals (category text, label text, weight numeric(8,2), factor float8, rank int, since date);
INSERT INTO map_vals VALUES ('Apparel', 'A', 1.50, 0.5, 1, '2022-01-01'), ('Apparel', 'a', 2.25, 1.5, 2, NULL), ('Shoes', 'S', NULL, NULL, 3, '2023-05-05'),
	('Home', 'H', 3.10, 2.0, NULL, '2021-01-01'), ('Toys', 'T', 0.05, 0.25, 5, '2024-02-02'), ('Toys', NULL, 0.10, NULL, 4, '2020-03-03');

-- ranges to place a date in (a date in several of them, in none; an empty, an open and a NULL range, a NULL label)
CREATE TABLE periods (label text, range daterange);
INSERT INTO periods VALUES ('cur', '[2024-06-01,2025-06-01)'), ('prev', '[2023-06-01,2024-06-01)'), ('wide', '[2023-01-01,2025-12-31]'),
	('empty', 'empty'), (NULL, '[2022-01-01,2022-04-01)'), ('open', '[2025-01-01,)'), ('none', NULL);

-- a joined column of every type PostgreSQL's own sum and average run on: a key with two rows, NULL values, a key without a row, big and negative values
CREATE TABLE map_nums (category text, i2 int2, i4 int4, i8 int8, f4 float4, iv interval);
INSERT INTO map_nums VALUES ('Apparel', 1, 10, 4000000000, 1.5, '1 day 02:00'), ('Apparel', 2, NULL, 5, 2.25, '2 mons'),
	('Shoes', NULL, 7, NULL, NULL, NULL), ('Home', -3, -4, -6, 0.1, '-3 hours'), ('Toys', 5, 5, 5, 3.5, '1 mon 1 day 1 sec');

-- the member lists of the queries (as bench/sql/analytics/sets.sql, with the moduli of a table of 4000 orders): K10 inside K400 inside K4000
CREATE TABLE analytics_sets (set_name text NOT NULL, member text NOT NULL);
INSERT INTO analytics_sets
SELECT s.name, p.store_id::text FROM (SELECT DISTINCT store_id FROM orders) p
JOIN (VALUES ('K10', 40), ('K400', 4), ('K4000', 1)) s(name, m) ON hashint8extended(p.store_id, 77) % s.m = 0;
INSERT INTO analytics_sets SELECT DISTINCT 'sources', source FROM orders;

-- edge cases of the trap queries: values at the limits of the integer types, NaN and infinities, strings that sort differently by collation
CREATE TABLE t_int (id integer PRIMARY KEY, grp integer NOT NULL, i4 integer, i8 bigint, n numeric(30,10));
INSERT INTO t_int SELECT i, 1 + i % 4,
	CASE i % 5 WHEN 0 THEN 2147483647 WHEN 1 THEN -2147483648 WHEN 2 THEN 2147483646 WHEN 3 THEN NULL ELSE i END,
	CASE i % 4 WHEN 0 THEN 9223372036854775807 WHEN 1 THEN 4611686018427387904 WHEN 2 THEN -9223372036854775807 ELSE i END,
	i * 33333333333333.3333333333
FROM generate_series(1, 40) i;
INSERT INTO t_int SELECT 100 + i, 1 + i % 4, (i % 1009) - 500, (i * 104729::bigint) % 20011 - 10000, (i % 5000) * 3.3333333333
FROM generate_series(1, greatest(:n - 4000, 0)) i;
CREATE TABLE t_float (id integer PRIMARY KEY, grp integer NOT NULL, f4 float4, f8 float8);
INSERT INTO t_float VALUES
	(1, 1, 0.1, 0.1), (2, 1, 0.2, 0.2), (3, 1, 0.3, 0.3), (4, 1, 16777216, 1e15), (5, 1, 1, 1e-15), (6, 1, NULL, NULL),
	(7, 2, 'NaN', 'NaN'), (8, 2, 1.5, 1.5), (9, 2, 2.5, 2.5), (10, 2, 'NaN', 'NaN'),
	(11, 3, 'Infinity', 'Infinity'), (12, 3, 1.5, 1.5), (13, 3, '-Infinity', '-Infinity'), (14, 3, 7, 7),
	(15, 4, 'Infinity', 'Infinity'), (16, 4, 3, 3), (17, 5, 0, 0), (18, 5, '-0', '-0'), (19, 5, 0, 0), (20, 5, '-0', '-0'),
	(21, 6, 1e30, 1e300), (22, 6, 1e30, 1e300), (23, 6, -1e30, -1e300), (24, 6, 3.4e38, 1.7e308), (25, 6, 3.4e38, 1.7e308);
INSERT INTO t_float SELECT 100 + i, 7, (0.1 * (i % 10))::float4, 0.1 * (i % 10) + 1e-9 * i FROM generate_series(1, 300) i;
INSERT INTO t_float SELECT 1000 + i, 7, ((i % 64) / 4.0 - 3)::float4, (i % 997) / 7.0 FROM generate_series(1, greatest(:n - 4000, 0)) i;
CREATE TABLE t_num (id integer PRIMARY KEY, grp integer NOT NULL, n2 numeric(12,2), n0 numeric(18,0), n6 numeric(18,6));
INSERT INTO t_num VALUES
	(1, 1, 10.50, 1, 0.000001), (2, 1, 0.05, 2, -0.5), (3, 1, -3.25, 3, 123456789012.123456), (4, 1, NULL, NULL, NULL), (5, 1, 7.00, 4, 0.1),
	(6, 2, NULL, NULL, NULL), (7, 2, NULL, NULL, NULL),
	(8, 3, 9999999999.99, 999999999999999999, 999999999999.999999), (9, 3, 9999999999.99, 999999999999999999, 999999999999.999999),
	(10, 3, 9999999999.99, 999999999999999999, -999999999999.999999),
	(11, 4, -9999999999.99, -999999999999999999, 0), (12, 4, -9999999999.99, -999999999999999999, 0.000001),
	(13, 5, 0.00, 0, 0.000000), (14, 5, 0.00, 0, 0.000000), (15, 5, 1.00, 1, 1.000000);
INSERT INTO t_num SELECT 100 + i, 7, round((i * 13.37 - 400)::numeric, 2), (i * 1000003) % 1000, round((i * 0.123457)::numeric, 6) FROM generate_series(1, 300) i;
INSERT INTO t_num SELECT 1000 + i, 3, 0.01, 999999999999999999, 0 FROM generate_series(1, 12) i;
INSERT INTO t_num SELECT 100000 + i, 7, round(((i * 37) % 4001 - 2000) / 7.0, 2), (i * 31) % 500 - 250, round(((i * 101) % 100001 - 50000) / 3.0, 6)
FROM generate_series(1, greatest(:n - 4000, 0)) i;
CREATE TABLE t_nan (id integer PRIMARY KEY, grp integer NOT NULL, n2 numeric(12,2), n6 numeric(18,6));
INSERT INTO t_nan VALUES
	(1, 1, 1.50, 0.5), (2, 1, 'NaN', 'NaN'), (3, 1, NULL, NULL), (4, 1, -7.25, 2),
	(5, 2, 'NaN', 3), (6, 2, 'NaN', 'NaN'), (7, 3, 0, 1), (8, 3, 2, 'NaN');
INSERT INTO t_nan SELECT 100 + i, 4 + i % 5, CASE WHEN i % 997 = 0 THEN 'NaN' ELSE round(((i * 37) % 4001 - 2000) / 7.0, 2) END, CASE WHEN i % 1999 = 0 THEN 'NaN' ELSE round(((i * 101) % 100001 - 50000) / 3.0, 6) END
FROM generate_series(1, greatest(:n - 4000, 0)) i;
CREATE TABLE t_text (id integer PRIMARY KEY, grp integer NOT NULL, t text, vc varchar(3));
INSERT INTO t_text SELECT i, 1 + i % 3, v, left(v, 3)
FROM unnest(ARRAY['a','B','_x','Z','é','ä','10','9','Ab','aB','ab','a b','A','b','Ç','c','z','Zz','-a','a-','ß','ss','','  ']) WITH ORDINALITY u(v, i);
-- strings longer than a dictionary entry (the engine stores them inline), repeated and distinct, with NULLs
INSERT INTO t_text SELECT 100 + i, 1 + i % 3, CASE WHEN i % 11 = 0 THEN NULL ELSE repeat(chr(97 + i % 4), 250 + (i % 9) * 40) || (i % 5) END, 'lng'
FROM generate_series(1, 90) i;

-- aggregates of the generic path: a user aggregate with an immutable transition function, and one with a volatile one
CREATE FUNCTION coldiff.fx_sq_add(acc float8, x float8) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$ SELECT acc + x * x $$;
CREATE AGGREGATE coldiff.sum_sq(float8) (sfunc = coldiff.fx_sq_add, stype = float8, initcond = '0');
CREATE FUNCTION coldiff.fx_vol_add(acc float8, x float8) RETURNS float8 LANGUAGE sql VOLATILE AS $$ SELECT acc + x + 0 * random() $$;
CREATE AGGREGATE coldiff.sum_vol(float8) (sfunc = coldiff.fx_vol_add, stype = float8, initcond = '0');
CREATE FUNCTION coldiff.fx_arr_add(acc bigint, a int[]) RETURNS bigint LANGUAGE sql IMMUTABLE AS $$ SELECT acc + coalesce((SELECT sum(x) FROM unnest(a) x), 0) $$;
CREATE AGGREGATE coldiff.sum_sq_par(float8) (sfunc = coldiff.fx_sq_add, stype = float8, initcond = '0', combinefunc = float8pl, parallel = safe);
CREATE AGGREGATE coldiff.sum_all(VARIADIC int[]) (sfunc = coldiff.fx_arr_add, stype = bigint, initcond = '0');
CREATE FUNCTION coldiff.fx_six_add(acc bigint, a int, b int, c int, d int, e int, f int) RETURNS bigint LANGUAGE sql IMMUTABLE AS $$ SELECT acc + a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 $$;
CREATE AGGREGATE coldiff.sum_six(int, int, int, int, int, int) (sfunc = coldiff.fx_six_add, stype = bigint, initcond = '0');

-- count(DISTINCT): a dense domain with negative values (v), a sparse wide one (w), a dense domain wide enough to outgrow a small hash_mem (x), NULLs
CREATE TABLE t_dist (id integer PRIMARY KEY, grp integer NOT NULL, v integer, w integer, x integer);
INSERT INTO t_dist SELECT i, 1 + i % 8, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 6007 - 3000 END, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 100003) % 2000000011 - 1000000000 END,
	(i * 37) % 1000000
FROM generate_series(1, 20000) i;

-- the same orders with objects that are not numbers among the metrics, items and notes that are not objects (the shredder's overflow cases)
CREATE TABLE orders_odd (LIKE orders INCLUDING DEFAULTS);
INSERT INTO orders_odd SELECT * FROM orders WHERE id <= 600;
UPDATE orders_odd SET metrics = metrics || CASE id % 7 WHEN 0 THEN '{"extra": "n/a"}' WHEN 1 THEN '{"flag": true}' WHEN 2 THEN '{"nested": {"a": 1}}'
		WHEN 3 THEN '{"nul": null}' WHEN 4 THEN '{"huge": 1e300}' WHEN 5 THEN '{"text_num": "4.5"}' ELSE '{"int": 3}' END::jsonb
WHERE id % 3 = 0;

-- partitioned by year (the partition key is part of the primary key)
CREATE TABLE orders_part (LIKE orders INCLUDING DEFAULTS) PARTITION BY RANGE (placed_at);
CREATE TABLE orders_part_2022 PARTITION OF orders_part FOR VALUES FROM ('2022-01-01') TO ('2023-01-01');
CREATE TABLE orders_part_2023 PARTITION OF orders_part FOR VALUES FROM ('2023-01-01') TO ('2024-01-01');
CREATE TABLE orders_part_2024 PARTITION OF orders_part FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE orders_part_2025 PARTITION OF orders_part FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO orders_part SELECT * FROM orders;

-- ---------- the DML of the scenarios (heap states) ----------
-- rows of orders with the hash of the id below frac (draw selects the sample)
CREATE FUNCTION coldiff.sample(id int, draw int, frac float8) RETURNS boolean LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$ SELECT coldiff.u(id, draw, 5) < frac $$;

-- new orders after the last id, without a VACUUM
CREATE FUNCTION coldiff.sc_insert(frac float8) RETURNS bigint LANGUAGE sql AS $$
	SELECT coldiff.fx_orders(m + 1, m + greatest(1, (frac * m)::int), 7) FROM (SELECT max(id) AS m FROM orders) x $$;
-- indexed columns (a new tuple version with a new TID) and the jsonb documents
CREATE FUNCTION coldiff.sc_update_indexed(frac float8) RETURNS bigint LANGUAGE sql AS $$
	WITH u AS (UPDATE orders SET store_id = 1 + (store_id * 7) % 400, placed_at = placed_at + 1 + id % 3, source = 'web', country_code = 'ZZ'
			   WHERE coldiff.sample(id, 901, frac) RETURNING 1),
		 j AS (UPDATE orders SET metrics = metrics || '{"overall": 1.5, "extra_key": 2.5}', items = items || '[{"category": "Toys", "kind": "gift", "qty": 2, "price": 9.9, "discount": 0}]'
			   WHERE coldiff.sample(id, 902, frac) RETURNING 1),
		 m AS (UPDATE categories_map SET mapped_category = CASE WHEN mapped_category = '' THEN 'Other' ELSE initcap(reverse(mapped_category)) END RETURNING 1),
		 t AS (UPDATE map_dup SET tag = tag || '2' WHERE category = 'Apparel' RETURNING 1),
		 g AS (INSERT INTO map_empty VALUES ('Home', 'late'), (NULL, 'late') RETURNING 1),
		 d AS (DELETE FROM map_part WHERE category = 'Home' RETURNING 1)
	SELECT (SELECT count(*) FROM u) + (SELECT count(*) FROM j) + (SELECT count(*) FROM m) + (SELECT count(*) FROM t) + (SELECT count(*) FROM g) + (SELECT count(*) FROM d) $$;
-- hard deletes and soft deletes
CREATE FUNCTION coldiff.sc_delete(frac float8) RETURNS bigint LANGUAGE sql AS $$
	WITH d AS (DELETE FROM orders WHERE coldiff.sample(id, 903, frac) RETURNING 1),
		 s AS (UPDATE orders SET deleted_at = timestamp '2025-11-02' WHERE coldiff.sample(id, 904, frac) AND deleted_at IS NULL RETURNING 1)
	SELECT (SELECT count(*) FROM d) + (SELECT count(*) FROM s) $$;
-- columns that no index holds (no new index entries: a HOT update when the page has room)
CREATE FUNCTION coldiff.sc_hot(frac float8) RETURNS bigint LANGUAGE sql AS $$
	WITH u AS (UPDATE orders SET total = total + 1, discount = discount + 0.5, customer_id = customer_id + 1 WHERE coldiff.sample(id, 905, frac) RETURNING 1)
	SELECT count(*) FROM u $$;
