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

DROP TABLE IF EXISTS orders, orders_odd, orders_part, t_part, t_part_ni, t_part_op, t_inh, tenants, pf_fact, pf_fact_empty, pf_dim, metrics_map, categories_map, kinds_map, analytics_sets, t_int, t_float, t_nan, t_text, t_dist, t_wide, t_misc, t_net,
	map_dup, map_null, map_case, map_part, map_empty, map_vals, map_nums, dim_source, dim_status, dim_lang, dim_country, dim_day, dim_customer, dim_pair,
	t_tags CASCADE;

CREATE TABLE orders (
	id integer NOT NULL, tenant_id integer NOT NULL, customer_id integer, placed_at date NOT NULL, status text, source text NOT NULL,
	country_code varchar(2), lang text, total numeric(12,2), discount numeric(12,2),
	attributes jsonb NOT NULL DEFAULT '{}',   -- object: channel, device, locale and normalized (true: the items carry canonical names)
	metrics jsonb NOT NULL DEFAULT '{}',      -- object with dynamic keys and float values
	items jsonb NOT NULL DEFAULT '[]',        -- array of objects {category, sku, kind, qty, price, discount}
	notes jsonb NOT NULL DEFAULT '[]',        -- array of objects {type, author_kind, text}
	memo text,                                -- free text, the column of the BM25 index
	deleted_at timestamp
) WITH (fillfactor = 80);

-- the keys of the attributes with a value of any JSON type (jsonb_typeof): weight, a float8 leaf with numbers (one decimal) among strings, booleans, objects,
-- arrays, JSON nulls and missing keys, tier, a text leaf with strings among numbers, booleans, objects, arrays, JSON nulls and missing keys, and code,
-- an astext leaf with every kind of scalar (a string that looks like a number too) among objects, arrays, JSON nulls and missing keys
CREATE FUNCTION coldiff.fx_mixed(id bigint, seed int DEFAULT 0) RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT CASE WHEN coldiff.u(id, 30, seed) < 0.05 THEN '{}'::jsonb
				ELSE jsonb_build_object('weight', CASE floor(coldiff.u(id, 31, seed) * 20)::int
					WHEN 0 THEN to_jsonb('heavy'::text) WHEN 1 THEN 'true'::jsonb WHEN 2 THEN '{"kg": 2}'::jsonb WHEN 3 THEN '[1, 2]'::jsonb WHEN 4 THEN 'null'::jsonb
					ELSE to_jsonb(round((coldiff.u(id, 32, seed) * 80)::numeric, 1)::float8) END) END
		|| CASE WHEN coldiff.u(id, 33, seed) < 0.05 THEN '{}'::jsonb
				ELSE jsonb_build_object('tier', CASE floor(coldiff.u(id, 34, seed) * 20)::int
					WHEN 0 THEN to_jsonb(floor(coldiff.u(id, 35, seed) * 100)::int) WHEN 1 THEN 'false'::jsonb WHEN 2 THEN '{"level": 1}'::jsonb WHEN 3 THEN '[]'::jsonb
					WHEN 4 THEN 'null'::jsonb ELSE to_jsonb((ARRAY['gold', 'silver', 'bronze', 'basic'])[1 + floor(coldiff.u(id, 36, seed) * 4)::int]) END) END
		|| CASE WHEN coldiff.u(id, 37, seed) < 0.05 THEN '{}'::jsonb
				ELSE jsonb_build_object('code', CASE floor(coldiff.u(id, 38, seed) * 20)::int
					WHEN 0 THEN 'true'::jsonb WHEN 1 THEN 'false'::jsonb WHEN 2 THEN '{"k": 1}'::jsonb WHEN 3 THEN '[2]'::jsonb WHEN 4 THEN 'null'::jsonb
					WHEN 5 THEN to_jsonb(floor(coldiff.u(id, 39, seed) * 100)::int) WHEN 6 THEN to_jsonb(round((coldiff.u(id, 39, seed) * 100)::numeric, 2))
					WHEN 7 THEN to_jsonb(floor(coldiff.u(id, 39, seed) * 100)::int::text)
					ELSE to_jsonb((ARRAY['ab', 'cd', 'ef'])[1 + floor(coldiff.u(id, 36, seed) * 3)::int]) END) END $$;

-- orders first_id .. last_id: store ~ u^2 over 400, source ~ u^2 over 30, dates 2022-01-01 .. 2025-10-31, 12 languages, 60 countries,
-- 0..8 items (mean 3.2), 1..6 metrics, 0..3 notes, 2% soft-deleted, 70% with normalized items; NULLs in customer, status, country, in
-- the kind of the items (JSON null, or the key absent in 10% of the orders) and 3% of the orders have no metric at all
CREATE FUNCTION coldiff.fx_orders(first_id int, last_id int, seed int DEFAULT 0) RETURNS bigint LANGUAGE sql AS $$
	WITH ins AS (
		INSERT INTO orders (id, tenant_id, customer_id, placed_at, status, source, country_code, lang, total, discount, attributes, metrics,
							items, notes, memo, deleted_at)
		SELECT g.id, p.tenant_id, CASE WHEN coldiff.u(g.id, 13, seed) < 0.1 THEN NULL ELSE 1 + floor(coldiff.u(g.id, 14, seed) * 500)::int END,
			   p.placed_at, CASE WHEN coldiff.u(g.id, 15, seed) < 0.05 THEN NULL
								 ELSE (ARRAY['paid','shipped','delivered','cancelled','refunded'])[1 + floor(power(coldiff.u(g.id, 16, seed), 1.5) * 5)::int] END,
			   p.src, CASE WHEN coldiff.u(g.id, 17, seed) < 0.02 THEN NULL ELSE p.cc END, p.lang,
			   round((10 + 490 * power(coldiff.u(g.id, 18, seed), 2))::numeric, 2),
			   round((10 + 490 * power(coldiff.u(g.id, 18, seed), 2) * coldiff.u(g.id, 19, seed) * 0.2)::numeric, 2),
			   jsonb_build_object('channel', (ARRAY['web','mobile','pos','marketplace'])[1 + floor(coldiff.u(g.id, 20, seed) * 4)::int],
								  'device', (ARRAY['desktop','phone','tablet','kiosk'])[1 + floor(coldiff.u(g.id, 21, seed) * 4)::int], 'locale', p.lang)
				 || CASE WHEN p.normalized THEN '{"normalized": true}'::jsonb ELSE '{}'::jsonb END || coldiff.fx_mixed(g.id, seed),
			   CASE WHEN coldiff.u(g.id, 22, seed) < 0.03 THEN '{}'::jsonb ELSE
				   (SELECT jsonb_object_agg((ARRAY['mhead','m_speed','packaging','support','value','quality','size_fit','accuracy',
												   'returns_ease','checkout','communication','freshness'])[1 + (p.rs + i) % 12],
											round((1 + 4 * sqrt(coldiff.u(g.id, 100 + i, seed)))::numeric, 1))
					FROM generate_series(0, p.nr - 1) i) END,
			   CASE WHEN coldiff.u(g.id, 23, seed) < 0.1 THEN jsonb_strip_nulls(it.items) ELSE it.items END,
			   (SELECT coalesce(jsonb_agg(jsonb_build_object('type', (ARRAY['reply','comment','internal'])[1 + floor(coldiff.u(g.id, 500 + j, seed) * 3)::int],
															 'author_kind', (ARRAY['staff','customer'])[1 + floor(coldiff.u(g.id, 600 + j, seed) * 2)::int],
															 'text', 'note ' || j) ORDER BY j), '[]'::jsonb)
				FROM generate_series(0, floor(coldiff.u(g.id, 24, seed) * 4)::int - 1) j),
			   (SELECT string_agg((ARRAY['fast','late','damaged','refund','gift','invoice','exchange','missing','perfect','broken','express','pickup',
										 'thanks','wrong','size','color','clean','wire','box','label'])[1 + floor(coldiff.u(g.id, 700 + w, seed) * 20)::int], ' ')
				FROM generate_series(1, 3 + (g.id % 5)) w),
			   CASE WHEN coldiff.u(g.id, 6, seed) < 0.02 THEN timestamp '2025-11-01 00:00:00' END
		FROM generate_series(first_id, last_id) AS g(id)
		CROSS JOIN LATERAL (SELECT 1 + floor(power(coldiff.u(g.id, 1, seed), 2) * 400)::int AS tenant_id,
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

CREATE TABLE tenants (id integer PRIMARY KEY, kind text, country_code text, name text);
INSERT INTO tenants SELECT i, (ARRAY['flagship','outlet','popup','online','warehouse'])[1 + hashint8extended(i, 41) % 5],
	(ARRAY['US','GB','DE','FR','ES','IT'])[1 + hashint8extended(i, 42) % 6], 'Store ' || i FROM generate_series(1, 400) i;

-- one dimension table per scalar column of an order (lookup joins of up to eight tables): keys without a row, a key with two rows, a NULL key,
-- and a pair of columns as one key
CREATE TABLE dim_source (source text, region text);
INSERT INTO dim_source SELECT s, 'r' || (hashtext(s) & 3) FROM (SELECT DISTINCT source AS s FROM orders) d WHERE hashtext(s) % 7 <> 0;
INSERT INTO dim_source SELECT s, 'dup' FROM (SELECT min(source) AS s FROM dim_source) d;
CREATE TABLE dim_status (status text, stage integer);
INSERT INTO dim_status VALUES ('paid', 1), ('shipped', 2), ('delivered', 3), ('cancelled', 9), (NULL, 0);
CREATE TABLE dim_lang (lang text, family text);
INSERT INTO dim_lang SELECT l, CASE WHEN l IN ('en', 'de', 'nl', 'sv') THEN 'germanic' WHEN l IN ('fr', 'es', 'it', 'pt') THEN 'romance' ELSE 'other' END
FROM unnest(ARRAY['en','de','fr','es','it','pt','nl','ru','ja','zh','sv']) l;
CREATE TABLE dim_country (country_code text, continent text);
INSERT INTO dim_country SELECT c, (ARRAY['north','south','east','west'])[1 + (hashtext(c) & 3)] FROM (SELECT DISTINCT country_code AS c FROM orders WHERE country_code IS NOT NULL) d
WHERE hashtext(c) % 11 <> 0;
CREATE TABLE dim_day (day date, quarter text);
INSERT INTO dim_day SELECT d, to_char(d, 'YYYY-"Q"Q') FROM (SELECT DISTINCT placed_at AS d FROM orders) x WHERE extract(dow FROM d) <> 3;
CREATE TABLE dim_customer (customer_id integer, segment text);
INSERT INTO dim_customer SELECT i, (ARRAY['retail','trade','vip'])[1 + i % 3] FROM generate_series(1, 500) i WHERE i % 11 <> 0;
CREATE TABLE dim_pair (source text, lang text, band integer);
INSERT INTO dim_pair SELECT s, l, hashtext(s || l) & 3 FROM (SELECT DISTINCT source AS s, lang AS l FROM orders) d WHERE hashtext(s || l) % 3 <> 0;

-- the preserved side of outer joins (ps_*): a fact with two keys (NULLs among them), its dimension with a key of two rows, a pair with no fact, NULL keys,
-- and an empty twin of the fact
CREATE TABLE pf_fact (id integer PRIMARY KEY, a integer, b integer, t text, v integer, f float8);
INSERT INTO pf_fact SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 7 END, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 5 END, (ARRAY['x', 'y', 'z'])[1 + i % 3],
	CASE WHEN i % 13 = 0 THEN NULL ELSE i % 13 END, i / 7.0 FROM generate_series(1, 600) i;
CREATE TABLE pf_fact_empty (LIKE pf_fact INCLUDING ALL);
CREATE TABLE pf_dim (a integer, b integer, label text, w float8);
INSERT INTO pf_dim SELECT a, b, 'l' || (a + b) % 4, a * 10 + b FROM generate_series(0, 9) a, generate_series(0, 4) b WHERE (a + b) % 6 <> 5;
INSERT INTO pf_dim VALUES (1, 1, 'dup', 0.5), (NULL, 1, 'null a', 1), (2, NULL, 'null b', 2), (NULL, NULL, 'null both', NULL);

-- the keys of the metrics of a source -> metric and aspect (about one in seven has no row: the split of the key is the fallback)
CREATE TABLE metrics_map (source text NOT NULL, metric_key text NOT NULL, derived_metric text, mapped_aspect text);
INSERT INTO metrics_map
SELECT s, key, initcap(split_part(key, '_', 1)), nullif(initcap(split_part(key, '_', 2)), '')
FROM unnest(ARRAY['web','ios','android','pos','phone'] || ARRAY(SELECT 'marketplace_' || lpad(i::text, 2, '0') FROM generate_series(1, 25) i)) s,
	 unnest(ARRAY['mhead','m_speed','packaging','support','value','quality','size_fit','accuracy','returns_ease','checkout','communication','freshness']) key
WHERE hashint8extended(length(s) * 100 + length(key), 31) % 7 <> 0;
CREATE TABLE categories_map (category text NOT NULL, derived_category text NOT NULL);
INSERT INTO categories_map SELECT lower(c), CASE WHEN c IN ('Pet', 'Auto') THEN '' ELSE c END
FROM unnest(ARRAY['Apparel','Shoes','Electronics','Home','Beauty','Toys','Sports','Books','Grocery','Garden','Pet','Office','Auto','Jewelry','Baby']) c;
CREATE TABLE kinds_map (kind text NOT NULL, derived_kind text NOT NULL);
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
SELECT s.name, p.tenant_id::text FROM (SELECT DISTINCT tenant_id FROM orders) p
JOIN (VALUES ('K10', 40), ('K400', 4), ('K4000', 1)) s(name, m) ON hashint8extended(p.tenant_id, 77) % s.m = 0;
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
-- arrays of strings (the path tags[] text): empty arrays, a tag repeated inside one array, tags that few rows have
CREATE TABLE t_tags (id integer PRIMARY KEY, grp integer NOT NULL, tags jsonb NOT NULL);
INSERT INTO t_tags
SELECT i, 1 + i % 5, coalesce((SELECT jsonb_agg((ARRAY['gift','express','fragile','wholesale','promo','returned','rare'])[1 + floor(power(coldiff.u(i, 50 + j), 2) * 7)::int] ORDER BY j)
							   FROM generate_series(1, floor(coldiff.u(i, 49) * 5)::int) j), '[]')
FROM generate_series(1, :n) i;
CREATE TABLE t_text (id integer PRIMARY KEY, grp integer NOT NULL, t text, vc varchar(3));
INSERT INTO t_text SELECT i, 1 + i % 3, v, left(v, 3)
FROM unnest(ARRAY['a','B','_x','Z','é','ä','10','9','Ab','aB','ab','a b','A','b','Ç','c','z','Zz','-a','a-','ß','ss','','  ']) WITH ORDINALITY u(v, i);
-- strings longer than a dictionary entry (the engine stores them inline), repeated and distinct, with NULLs
INSERT INTO t_text SELECT 100 + i, 1 + i % 3, CASE WHEN i % 11 = 0 THEN NULL ELSE repeat(chr(97 + i % 4), 250 + (i % 9) * 40) || (i % 5) END, 'lng'
FROM generate_series(1, 90) i;

-- the wide leaves: numeric of 19..38 digits (two lanes), any other numeric (a canonical image: unconstrained, a negative
-- scale, more than 38 digits), money and interval, with their traps: NaN and the infinities, the largest and smallest values, equal numbers of
-- different scales, equal intervals spelled differently, constants beyond the 128 bits
CREATE TABLE t_wide (id integer PRIMARY KEY, grp integer NOT NULL, qty integer, n38 numeric(38,10), n19 numeric(19,2), nimg numeric, nneg numeric(5,-2),
	nhuge numeric(40,5), m money, iv interval);
INSERT INTO t_wide VALUES
	(1, 1, 1, 0, 0, 0, 0, 0, '0', '0'),
	(2, 1, 2, 9999999999999999999999999999.9999999999, 99999999999999999.99, 1, 99900, 99999999999999999999999999999999999.99999, '92233720368547758.07', 'infinity'),
	(3, 1, 3, -9999999999999999999999999999.9999999999, -99999999999999999.99, -1, -99900, -99999999999999999999999999999999999.99999, '-92233720368547758.08', '-infinity'),
	(4, 1, 4, 'NaN', 'NaN', 'NaN', 100, 'NaN', '1.00', '1 month'),
	(5, 1, 5, 'NaN', 'NaN', 'NaN', -100, 'NaN', '-1.00', '30 days'),
	(6, 2, 6, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
	(7, 2, 7, 0.0000000001, 0.01, 1.0, 0, 0.00001, '0.01', '1 microsecond'),
	(8, 2, 8, 0.0000000002, 0.02, 1.00, 0, 0.00002, '0.02', '-1 microsecond'),
	(9, 2, 9, 1234.5, 1234.50, 1, 1200, 1234.5, '1234.50', '720 hours'),
	(10, 2, 10, 1234.5000000001, 1234.51, 1.000, 1300, 1234.50001, '1234.51', '1 mon 1 day'),
	(11, 3, 11, 12345678901234567890.1234567890, 12345678901234567.89, 1.5, 2000, 12345678901234567890.12345, '123456789012345.67', '31 days'),
	(12, 3, 12, -12345678901234567890.1234567890, -12345678901234567.89, 1.50, -2000, -12345678901234567890.12345, '-123456789012345.67', '1 day 1 hour'),
	(13, 3, 13, 1e18, 1e16, 'Infinity', 9999900, 18446744073709551616, '184467440737095.52', '25 hours'),
	(14, 3, 14, -1e18, -1e16, '-Infinity', -9999900, -18446744073709551616, '-184467440737095.52', '-1 month'),
	(15, 3, 15, 9223372036854775808, 9223372036854775.81, 10::numeric ^ 400 + 1, 0, 9223372036854775808, '92233720368547.75', '-30 days'),
	(16, 4, 16, -9223372036854775809, -9223372036854775.81, -(10::numeric ^ 400), 0, -9223372036854775809, '-92233720368547.75', '1 year'),
	(17, 4, 17, 0.5, 0.5, 1 / 10::numeric ^ 300, 0, 0.5, '0.50', '12 mons'),
	(18, 4, 18, -0.5, -0.5, -1 / 10::numeric ^ 300, 0, -0.5, '-0.50', '365 days'),
	(19, 4, 19, 1, 1, 100, 0, 1, '1.00', '2 mons'),
	(20, 4, 20, 1, 1, 100.0, 0, 1, '1.00', '59 days'),
	(21, 5, 21, 0, 0, 0.0, 0, 0, '0', '60 days'),
	(22, 5, 22, 18446744073709551615, 18446744073709551.62, 123456789012345678901234567890123456789.123456789, 99999, 18446744073709551615, '1844674407370955.16', '2147483647 months 2147483647 days 9223372036854775807 us'),
	(23, 5, 23, -18446744073709551615, -18446744073709551.62, -123456789012345678901234567890123456789.123456789, -99999, -18446744073709551615, '-1844674407370955.16', '-2147483648 months -2147483648 days -9223372036854775808 us');
INSERT INTO t_wide SELECT 100 + i, 6 + i % 5, i % 100,
	CASE WHEN i % 997 = 0 THEN 'NaN' WHEN i % 13 = 0 THEN NULL ELSE round(((i * 7919) % 2000001 - 1000000) / 100.0 * 1000000000000.0001, 10) END,
	CASE WHEN i % 991 = 0 THEN 'NaN' WHEN i % 11 = 0 THEN NULL ELSE ((i * 104729) % 2000001 - 1000000) / 100.0 END,
	CASE WHEN i % 983 = 0 THEN 'NaN' WHEN i % 10 = 0 THEN NULL ELSE round(((i * 7919) % 200001 - 100000) / 7.0, 1 + i % 5) END,
	CASE WHEN i % 6 = 0 THEN NULL ELSE ((i % 300) - 150) * 100 END,
	CASE WHEN i % 977 = 0 THEN 'NaN' WHEN i % 8 = 0 THEN NULL ELSE (i::numeric * 10::numeric ^ 30) + i / 1000.0 END,
	CASE WHEN i % 9 = 0 THEN NULL ELSE (((i * 7919) % 200001 - 100000)::numeric / 100)::money END,
	CASE WHEN i % 12 = 0 THEN NULL ELSE (ARRAY['1 month', '30 days', '720 hours', '1 mon 1 day', '31 days', '1 day 1 hour', '25 hours', '-1 month', '-30 days', '0', '1 year', '2 mons', '60 days'])[1 + i % 13]::interval END
FROM generate_series(1, greatest(:n - 4000, 400)) i;

-- the other 3.2 leaf types: time, enum (a value added after the first ones, so that the OID order is not the sort order), uuid, bytea, arrays,
-- ranges and a multirange, with NULLs, empty values and equal values spelled differently (the numeric elements 1.0 and 1.00)
DROP TYPE IF EXISTS coldiff_lvl3 CASCADE;
CREATE TYPE coldiff_lvl3 AS ENUM ('lo', 'ok', 'hi');
ALTER TYPE coldiff_lvl3 ADD VALUE 'meh' BEFORE 'ok';
CREATE TABLE t_misc (id integer PRIMARY KEY, grp integer NOT NULL, qty integer, t time, e coldiff_lvl3, u uuid, b bytea, ia integer[], ta text[], na numeric[],
	r int4range, tr tstzrange, mr int4multirange);
INSERT INTO t_misc SELECT i, i % 7, i % 100,
	CASE WHEN i % 9 = 0 THEN NULL WHEN i % 29 = 0 THEN '24:00:00' WHEN i % 31 = 0 THEN '00:00:00' ELSE (((i * 7919) % 86400) * interval '1 second' + (i % 5) * interval '1 microsecond')::time END,
	CASE WHEN i % 8 = 0 THEN NULL ELSE (ARRAY['lo', 'ok', 'hi', 'meh'])[1 + i % 4]::coldiff_lvl3 END,
	CASE WHEN i % 11 = 0 THEN NULL WHEN i % 37 = 0 THEN '00000000-0000-0000-0000-000000000000' WHEN i % 41 = 0 THEN 'ffffffff-ffff-ffff-ffff-ffffffffffff' ELSE md5((i % 50)::text)::uuid END,
	CASE WHEN i % 13 = 0 THEN NULL WHEN i % 17 = 0 THEN '\x'::bytea WHEN i % 19 = 0 THEN '\x00ff00'::bytea WHEN i % 23 = 0 THEN '\x00'::bytea ELSE decode(md5((i % 40)::text), 'hex') END,
	CASE WHEN i % 10 = 0 THEN NULL WHEN i % 14 = 0 THEN '{}' ELSE ARRAY[i % 5, i % 3] END,
	CASE WHEN i % 10 = 0 THEN NULL ELSE ARRAY['a' || (i % 4), NULL, 'z'] END,
	CASE WHEN i % 10 = 0 THEN NULL ELSE ARRAY[(ARRAY[1.0, 1.00, 1, 1.50, 1.5])[1 + i % 5], (i % 3)::numeric] END,
	CASE WHEN i % 12 = 0 THEN NULL WHEN i % 15 = 0 THEN 'empty' ELSE int4range(i % 10, i % 10 + 5) END,
	CASE WHEN i % 12 = 0 THEN NULL ELSE tstzrange('2020-01-01'::timestamptz + (i % 30) * interval '1 day', '2020-03-01') END,
	CASE WHEN i % 12 = 0 THEN NULL ELSE int4multirange(int4range(i % 5, i % 5 + 2), int4range(20, 30)) END
FROM generate_series(1, greatest(:n - 4000, 800)) i;

-- the network, time zone and bit string types: inet with and without a mask, IPv4-mapped IPv6, equal values spelled differently (the IPv6 and mac
-- address texts), timetz at its extremes, bit(8) with leading zeros, varbit of lengths 0 to 3 and a few of 12800 bits (out of line), NULLs; a point, which no index holds
CREATE TABLE t_net (id integer PRIMARY KEY, grp integer NOT NULL, qty integer, ip inet, cd cidr, mac macaddr, mac8 macaddr8, tz timetz, b bit(8), vb varbit, pt point);
INSERT INTO t_net SELECT i, i % 7, i % 100,
	CASE WHEN i % 9 = 0 THEN NULL ELSE (ARRAY['10.0.0.1', '10.0.0.1/24', '10.0.0.1/32', '10.0.0.2/24', '192.168.1.5', '0.0.0.0/0', '255.255.255.255', '::1', '2001:db8::1',
		'2001:0db8:0:0:0:0:0:1', '2001:db8::1/64', '::ffff:10.0.0.1', '::ffff:10.0.0.1/128', '::', 'ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128', '10.0.0.0/8'])[1 + i % 16]::inet END,
	CASE WHEN i % 10 = 0 THEN NULL ELSE (ARRAY['10.0.0.0/24', '10.0.0.0/8', '192.168.0.0/16', '::/0', '2001:db8::/32', '10.0.0.0/32', '0.0.0.0/0'])[1 + i % 7]::cidr END,
	CASE WHEN i % 11 = 0 THEN NULL ELSE (ARRAY['08:00:2b:01:02:03', '08-00-2B-01-02-03', '0800.2b01.0203', 'AA:BB:CC:DD:EE:FF', 'aabbccddeeff', '00:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff'])[1 + i % 7]::macaddr END,
	CASE WHEN i % 12 = 0 THEN NULL ELSE (ARRAY['08:00:2b:01:02:03:04:05', '08-00-2B-01-02-03-04-05', '0800.2b01.0203.0405', 'aabbccddeeff0011', '00:00:00:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff:ff:ff', '08:00:2b:ff:fe:01:02:03'])[1 + i % 7]::macaddr8 END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE (ARRAY['12:00+01', '11:00+00', '12:00+00', '00:00:00+00', '24:00:00-15:59', '00:00:00+15:59', '23:59:59.999999-15:59', '04:05:06+00', '04:05:06.5-03'])[1 + i % 9]::timetz END,
	CASE WHEN i % 14 = 0 THEN NULL ELSE (ARRAY[B'00000000', B'00000001', B'10000000', B'11111111', B'00001111', B'01010101'])[1 + i % 6] END,
	CASE WHEN i % 15 = 0 THEN NULL WHEN i % 97 = 0 THEN ('x' || (SELECT string_agg(md5(i || '-' || k), '') FROM generate_series(1, 400) k))::varbit
		ELSE (ARRAY['', '0', '00', '000', '1', '10', '0001', '101010101010101', '00000000000000000000000000000000000000000000000000000000000000001'])[1 + i % 9]::varbit END,
	point(i % 5, i % 3)
FROM generate_series(1, greatest(:n - 4000, 800)) i;

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

-- the truth tables of jsonpath and containment on an array of objects (docs/AGG.md): the members s (text), n (int4) and b (bool) take every value
-- their leaves hold (equal numbers spelled 1 and 1.0, the empty string, non-ASCII text, the extremes of the type), a JSON null and a missing key
DROP TABLE IF EXISTS t_jp;
CREATE TABLE t_jp (id integer PRIMARY KEY, grp integer NOT NULL, doc jsonb NOT NULL, mdoc jsonb);
CREATE FUNCTION coldiff.jp_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT coalesce(jsonb_agg(
		(ARRAY['{"s": "gift"}', '{"s": ""}', '{"s": "\u00fcn\u00ef"}', '{"s": "toy"}', '{"s": null}', '{}'])[1 + (i * 5 + j * 7) % 6]::jsonb
		|| (ARRAY['{"n": 1}', '{"n": 1.0}', '{"n": 3}', '{"n": -2}', '{"n": 0}', '{"n": 2147483647}', '{"n": null}', '{}'])[1 + (i * 3 + j * 11) % 8]::jsonb
		|| (ARRAY['{"b": true}', '{"b": false}', '{"b": null}', '{}'])[1 + (i + j * 5) % 4]::jsonb ORDER BY j), '[]')
	FROM generate_series(1, i % 5) j $$;
INSERT INTO t_jp SELECT i, i % 7, coldiff.jp_doc(i), CASE WHEN i % 11 = 0 THEN NULL ELSE coldiff.jp_doc(i) END FROM generate_series(1, 2000) i;

-- an element compared with a column of its own row (docs/AGG.md): the members q (int4), q8 (int8) and f (float8) of the elements against
-- r4, r8 and rf of the row, with NULL columns, a NaN, a -0, a JSON null and a missing member, empty arrays and a NULL document. Element j of row i
-- holds the value that column r4 has in row i + j - 1 (mod 6): every row has elements equal to the columns of the neighbour rows
DROP TABLE IF EXISTS t_eo;
CREATE TABLE t_eo (id integer PRIMARY KEY, grp integer NOT NULL, r4 integer, r8 bigint, rf float8, doc jsonb);
CREATE FUNCTION coldiff.eo_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN i % 10 = 0 THEN NULL ELSE coalesce(jsonb_agg(
		jsonb_build_object('q', CASE WHEN (i * 3 + j * 5) % 8 = 0 THEN NULL ELSE (i + j - 1) % 6 END, 'q8', (i + j - 1) % 6 + CASE WHEN (i * 2 + j) % 13 = 0 THEN 3000000000 ELSE 0 END,
						   'f', ((i + j - 1) % 6) * 0.5)
		- CASE WHEN (i * 5 + j * 3) % 11 = 0 THEN 'q' ELSE '' END ORDER BY j), '[]') END
	FROM generate_series(1, i % 5) j $$;
CREATE FUNCTION coldiff.fx_eo(first_id int, last_id int) RETURNS bigint LANGUAGE sql AS $$
	WITH ins AS (
		INSERT INTO t_eo SELECT i, i % 7, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 6 END,
			   CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN 3000000000 ELSE i % 6 END,
			   CASE WHEN i % 13 = 0 THEN NULL WHEN i % 17 = 0 THEN 'NaN'::float8 WHEN i % 23 = 0 THEN '-0'::float8 ELSE (i % 6) * 0.5 END, coldiff.eo_doc(i)
		FROM generate_series(first_id, last_id) i RETURNING 1)
	SELECT count(*) FROM ins $$;
SELECT coldiff.fx_eo(1, greatest(2000, :n / 2));

-- documents that hold what a typed leaf cannot store: the root that is an array, a string, a number or a JSON null, a number or a
-- numeric string where a text, an int4, a float8 or a bool is declared. The rows of grp 6 whose id is a multiple of 13 hold a string that is no number
-- (the error SQL raises for a row that reaches jsonb_to_recordset); the queries that must not raise filter them out with grp <> 6
DROP TABLE IF EXISTS t_rc;
CREATE TABLE t_rc (id integer PRIMARY KEY, grp integer NOT NULL, doc jsonb);
CREATE FUNCTION coldiff.rc_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 17 WHEN 0 THEN '[1, 2]'::jsonb WHEN 1 THEN '"text"'::jsonb WHEN 2 THEN '5'::jsonb WHEN 3 THEN 'null'::jsonb
		ELSE jsonb_build_object('l', (SELECT coalesce(jsonb_agg(jsonb_build_object(
			'k', CASE WHEN (i + j) % 11 = 0 THEN to_jsonb(j) ELSE to_jsonb('k' || (i + j) % 4) END,
			'v', CASE WHEN i % 7 = 6 AND i % 13 = 0 AND j = 1 THEN to_jsonb('abc'::text) WHEN (i * 3 + j) % 7 = 0 THEN to_jsonb(((i + j) % 9)::text) ELSE to_jsonb(((i + j) % 9) * 0.5) END,
			'n', CASE WHEN (i + j * 2) % 13 = 0 THEN to_jsonb(j::text) ELSE to_jsonb((i + j) % 5) END,
			'ok', CASE WHEN (i + j) % 19 = 0 THEN to_jsonb('t'::text) ELSE to_jsonb((i + j) % 2 = 0) END) ORDER BY j), '[]') FROM generate_series(1, 1 + i % 4) j)) END $$;
CREATE FUNCTION coldiff.fx_rc(first_id int, last_id int) RETURNS bigint LANGUAGE sql AS $$
	WITH ins AS (INSERT INTO t_rc SELECT i, i % 7, coldiff.rc_doc(i) FROM generate_series(first_id, last_id) i RETURNING 1)
	SELECT count(*) FROM ins $$;
SELECT coldiff.fx_rc(1, greatest(2000, :n / 2));

-- documents whose typed leaves hold what they cannot store, read through a key of an object leaf (m.*), a scalar path (k), the elements of a list (l) and the
-- length of an array (n). grp is the kind of value of a, k and v: 0 and 11 numbers, 1 a numeric string, 2 a text that is no number,
-- 3 an object, 4 an array, 5 a JSON null, 6 a boolean, 7 the string NaN, 8 the string Infinity, 9 the string -0, 10 a padded numeric string
DROP TABLE IF EXISTS t_vd;
CREATE TABLE t_vd (id integer PRIMARY KEY, grp integer NOT NULL, doc jsonb);
CREATE FUNCTION coldiff.vd_val(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 12 WHEN 1 THEN to_jsonb((i % 10)::text) WHEN 2 THEN to_jsonb('abc'::text) WHEN 3 THEN '{"x": 1}'::jsonb WHEN 4 THEN '[1, 2]'::jsonb
		WHEN 5 THEN 'null'::jsonb WHEN 6 THEN 'true'::jsonb WHEN 7 THEN '"NaN"'::jsonb WHEN 8 THEN '"Infinity"'::jsonb WHEN 9 THEN '"-0"'::jsonb
		WHEN 10 THEN to_jsonb(' ' || (i % 10) || ' ') ELSE to_jsonb((i % 10) * 0.5) END $$;
CREATE FUNCTION coldiff.vd_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT jsonb_build_object('m', jsonb_build_object('a', coldiff.vd_val(i), 'b', i % 7), 'k', coldiff.vd_val(i),
		'l', jsonb_build_array(jsonb_build_object('v', coldiff.vd_val(i)), jsonb_build_object('v', i % 10)),
		'n', CASE i % 12 WHEN 2 THEN '"abc"'::jsonb WHEN 3 THEN '{"x": 1}'::jsonb WHEN 5 THEN 'null'::jsonb WHEN 6 THEN '7'::jsonb
			ELSE jsonb_build_array(jsonb_build_object('x', i % 5), jsonb_build_object('x', i % 3)) END) $$;
INSERT INTO t_vd SELECT i, i % 12, coldiff.vd_doc(i) FROM generate_series(1, 2000) i;

-- partitioned by year (the partition key is part of the primary key)
CREATE TABLE orders_part (LIKE orders INCLUDING DEFAULTS) PARTITION BY RANGE (placed_at);
CREATE TABLE orders_part_2022 PARTITION OF orders_part FOR VALUES FROM ('2022-01-01') TO ('2023-01-01');
CREATE TABLE orders_part_2023 PARTITION OF orders_part FOR VALUES FROM ('2023-01-01') TO ('2024-01-01');
CREATE TABLE orders_part_2024 PARTITION OF orders_part FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE orders_part_2025 PARTITION OF orders_part FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO orders_part SELECT * FROM orders;

-- a partitioned table of one column index (docs/AGG.md, "Partitioned tables"): range by day, a partition that has other attribute numbers (a dropped column
-- first, the columns in another order), a branch partitioned again by hash, a default partition that takes the days no range has and an unlogged partition
CREATE TABLE t_part (id integer NOT NULL, grp integer NOT NULL, d date NOT NULL, qty integer, amount numeric(10,2), tag text, doc jsonb, note text) PARTITION BY RANGE (d);
CREATE TABLE t_part_q1 PARTITION OF t_part FOR VALUES FROM ('2024-01-01') TO ('2024-04-01');
CREATE TABLE t_part_q2 (junk text, note text, doc jsonb, tag text, amount numeric(10,2), qty integer, d date NOT NULL, grp integer NOT NULL, id integer NOT NULL);
ALTER TABLE t_part_q2 DROP COLUMN junk;
ALTER TABLE t_part ATTACH PARTITION t_part_q2 FOR VALUES FROM ('2024-04-01') TO ('2024-07-01');
CREATE TABLE t_part_q3 PARTITION OF t_part FOR VALUES FROM ('2024-07-01') TO ('2024-10-01') PARTITION BY HASH (id);
CREATE TABLE t_part_q3_0 PARTITION OF t_part_q3 FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE t_part_q3_1 PARTITION OF t_part_q3 FOR VALUES WITH (MODULUS 2, REMAINDER 1);
CREATE UNLOGGED TABLE t_part_q4 PARTITION OF t_part FOR VALUES FROM ('2024-10-01') TO ('2025-01-01');
CREATE TABLE t_part_rest PARTITION OF t_part DEFAULT;
CREATE FUNCTION coldiff.fx_part(first_id int, last_id int, seed int DEFAULT 0) RETURNS bigint LANGUAGE sql AS $$
	WITH ins AS (
		INSERT INTO t_part
		SELECT i, 1 + i % 5, date '2023-11-15' + floor(coldiff.u(i, 70, seed) * 460)::int, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 7) % 50 END,
			   CASE WHEN i % 17 = 0 THEN NULL ELSE round((coldiff.u(i, 71, seed) * 1000)::numeric, 2) END, (ARRAY['red', 'green', 'blue', 'plain'])[1 + i % 4],
			   jsonb_build_object('k', i % 9, 's', (ARRAY['x', 'y', 'z'])[1 + i % 3]), 'n' || i % 7
		FROM generate_series(first_id, last_id) i RETURNING 1)
	SELECT count(*) FROM ins $$;
SELECT coldiff.fx_part(1, :n);

-- the traps: a partition without an index, partitions whose indexes hold other leaves, an inheritance parent
CREATE TABLE t_part_ni (id integer NOT NULL, grp integer NOT NULL, d date NOT NULL, qty integer) PARTITION BY RANGE (d);
CREATE TABLE t_part_ni_a PARTITION OF t_part_ni FOR VALUES FROM ('2024-01-01') TO ('2024-07-01');
CREATE TABLE t_part_ni_b PARTITION OF t_part_ni FOR VALUES FROM ('2024-07-01') TO ('2025-01-01');
CREATE TABLE t_part_op (id integer NOT NULL, grp integer NOT NULL, d date NOT NULL, qty integer, doc jsonb) PARTITION BY RANGE (d);
CREATE TABLE t_part_op_a PARTITION OF t_part_op FOR VALUES FROM ('2024-01-01') TO ('2024-07-01');
CREATE TABLE t_part_op_b PARTITION OF t_part_op FOR VALUES FROM ('2024-07-01') TO ('2025-01-01');
CREATE TABLE t_inh (id integer NOT NULL, grp integer NOT NULL, d date NOT NULL, qty integer);
CREATE TABLE t_inh_a (CHECK (d < '2024-07-01')) INHERITS (t_inh);
CREATE TABLE t_inh_b (CHECK (d >= '2024-07-01')) INHERITS (t_inh);
INSERT INTO t_part_ni SELECT i, 1 + i % 5, date '2024-01-01' + (i * 7) % 366, i % 40 FROM generate_series(1, greatest(:n / 4, 400)) i;
INSERT INTO t_part_op SELECT i, 1 + i % 5, date '2024-01-01' + (i * 7) % 366, i % 40, jsonb_build_object('k', i % 6) FROM generate_series(1, greatest(:n / 4, 400)) i;
INSERT INTO t_inh_a SELECT i, 1 + i % 5, date '2024-01-01' + (i * 7) % 182, i % 40 FROM generate_series(1, greatest(:n / 4, 400)) i;
INSERT INTO t_inh_b SELECT 100000 + i, 1 + i % 5, date '2024-07-01' + (i * 7) % 184, i % 40 FROM generate_series(1, greatest(:n / 4, 400)) i;

-- the check analytics workload (workload.sql): a fact table of items with jsonb metrics_a and a document with a nested array of parts, its dimensions
\ir workload.sql

-- ---------- the DML of the scenarios (heap states) ----------
-- rows of orders with the hash of the id below frac (draw selects the sample)
CREATE FUNCTION coldiff.sample(id int, draw int, frac float8) RETURNS boolean LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$ SELECT coldiff.u(id, draw, 5) < frac $$;

-- new orders after the last id, without a VACUUM
CREATE FUNCTION coldiff.sc_insert(frac float8) RETURNS bigint LANGUAGE sql AS $$
	WITH j AS (INSERT INTO t_jp SELECT i, i % 7, coldiff.jp_doc(i + 3), coldiff.jp_doc(i + 5) FROM (SELECT max(id) AS m FROM t_jp) x,
			   generate_series(m + 1, m + greatest(1, (frac * m)::int)) i RETURNING 1)
	SELECT coldiff.fx_orders(m + 1, m + greatest(1, (frac * m)::int), 7) + (SELECT count(*) FROM j) +
		   (SELECT coldiff.fx_eo(e + 1, e + greatest(1, (frac * e)::int)) FROM (SELECT max(id) AS e FROM t_eo) z) +
		   (SELECT coldiff.fx_rc(r + 1, r + greatest(1, (frac * r)::int)) FROM (SELECT max(id) AS r FROM t_rc) q) +
		   (SELECT coldiff.fx_part(p + 1, p + greatest(1, (frac * p)::int), 7) FROM (SELECT max(id) AS p FROM t_part) y) +
		   (SELECT coldiff.fx_wl(w + 1, w + greatest(1, (frac * w)::int), 7) FROM (SELECT max(id) AS w FROM t_work) v)
	FROM (SELECT max(id) AS m FROM orders) x $$;
-- indexed columns (a new tuple version with a new TID) and the jsonb documents
CREATE FUNCTION coldiff.sc_update_indexed(frac float8) RETURNS bigint LANGUAGE sql AS $$
	WITH u AS (UPDATE orders SET tenant_id = 1 + (tenant_id * 7) % 400, placed_at = placed_at + 1 + id % 3, source = 'web', country_code = 'ZZ'
			   WHERE coldiff.sample(id, 901, frac) RETURNING 1),
		 j AS (UPDATE orders SET metrics = metrics || '{"mhead": 1.5, "extra_key": 2.5}', items = items || '[{"category": "Toys", "kind": "gift", "qty": 2, "price": 9.9, "discount": 0}]'
			   WHERE coldiff.sample(id, 902, frac) RETURNING 1),
		 m AS (UPDATE categories_map SET derived_category = CASE WHEN derived_category = '' THEN 'Other' ELSE initcap(reverse(derived_category)) END RETURNING 1),
		 t AS (UPDATE map_dup SET tag = tag || '2' WHERE category = 'Apparel' RETURNING 1),
		 g AS (INSERT INTO map_empty VALUES ('Home', 'late'), (NULL, 'late') RETURNING 1),
		 d AS (DELETE FROM map_part WHERE category = 'Home' RETURNING 1),
		 p AS (UPDATE t_jp SET doc = coldiff.jp_doc(id + 1), mdoc = coldiff.jp_doc(id + 2) WHERE coldiff.sample(id, 906, frac) RETURNING 1),
		 pt AS (UPDATE t_part SET qty = coalesce(qty, 0) + 3, d = d + 1 + id % 40, tag = 'moved' WHERE coldiff.sample(id, 908, frac) RETURNING 1),
		 eo AS (UPDATE t_eo SET r4 = (r4 + 1) % 6, rf = rf + 0.5, doc = coldiff.eo_doc(id + 1) WHERE coldiff.sample(id, 911, frac) RETURNING 1),
		 rc AS (UPDATE t_rc SET doc = coldiff.rc_doc(id + 7) WHERE coldiff.sample(id, 917, frac) RETURNING 1),
		 wl AS (UPDATE t_work SET dim_a_id = 1 + dim_a_id % 30, item_date = item_date + 1, metrics_a = coldiff.wl_metrics_a(id + 1, 3), doc = coldiff.wl_doc(id + 1, 3)
				WHERE coldiff.sample(id, 913, frac) RETURNING 1)
	SELECT (SELECT count(*) FROM wl) + (SELECT count(*) FROM rc) + (SELECT count(*) FROM eo) + (SELECT count(*) FROM pt) + (SELECT count(*) FROM p) + (SELECT count(*) FROM u) + (SELECT count(*) FROM j) + (SELECT count(*) FROM m) + (SELECT count(*) FROM t) + (SELECT count(*) FROM g) + (SELECT count(*) FROM d) $$;
-- hard deletes and soft deletes
CREATE FUNCTION coldiff.sc_delete(frac float8) RETURNS bigint LANGUAGE sql AS $$
	WITH d AS (DELETE FROM orders WHERE coldiff.sample(id, 903, frac) RETURNING 1),
		 s AS (UPDATE orders SET deleted_at = timestamp '2025-11-02' WHERE coldiff.sample(id, 904, frac) AND deleted_at IS NULL RETURNING 1),
		 p AS (DELETE FROM t_jp WHERE coldiff.sample(id, 907, frac) RETURNING 1),
		 pt AS (DELETE FROM t_part WHERE coldiff.sample(id, 909, frac) RETURNING 1),
		 eo AS (DELETE FROM t_eo WHERE coldiff.sample(id, 912, frac) RETURNING 1),
		 rc AS (DELETE FROM t_rc WHERE coldiff.sample(id, 918, frac) RETURNING 1),
		 wl AS (DELETE FROM t_work WHERE coldiff.sample(id, 914, frac) RETURNING 1),
		 ws AS (UPDATE t_work SET deleted_at = timestamp '2025-11-02' WHERE coldiff.sample(id, 915, frac) AND deleted_at IS NULL RETURNING 1)
	SELECT (SELECT count(*) FROM wl) + (SELECT count(*) FROM ws) + (SELECT count(*) FROM rc) + (SELECT count(*) FROM eo) + (SELECT count(*) FROM pt) + (SELECT count(*) FROM p) + (SELECT count(*) FROM d) + (SELECT count(*) FROM s) $$;
-- columns that no index holds (no new index entries: a HOT update when the page has room)
CREATE FUNCTION coldiff.sc_hot(frac float8) RETURNS bigint LANGUAGE sql AS $$
	WITH u AS (UPDATE orders SET total = total + 1, discount = discount + 0.5, customer_id = customer_id + 1 WHERE coldiff.sample(id, 905, frac) RETURNING 1),
		 pt AS (UPDATE t_part SET note = 'h' || id % 3 WHERE coldiff.sample(id, 910, frac) RETURNING 1),
		 wl AS (UPDATE t_work SET title = 'h' || id % 3 WHERE coldiff.sample(id, 916, frac) RETURNING 1)
	SELECT (SELECT count(*) FROM u) + (SELECT count(*) FROM pt) + (SELECT count(*) FROM wl) $$;
