-- Deterministic orders generator, loaded straight into the tables of schema.sql.
--
--   psql -1 -v n=1200000 -v tenants=40000 -v order_by= -f schema.sql -f gen.sql
--
-- n orders (1.2M, 10x = 12M) in `stores` stores. Every draw is a hash of (order id, draw number), so the same arguments give the same
-- rows on every run (hashint8extended is what hash partitioning uses: stable across versions and runs of a platform).
-- Prices are multiples of 0.25 and metrics multiples of 0.5, quantities are integers: every sum and average of them is exact in floating
-- point, so no plan, order of summation or engine can move an order across the thresholds that the queries count by.
-- Skews: store ~ u^2 over `stores`, source ~ u^2 over 30, 12 languages (u^2), 60 countries (u^2), dates 2022-01-01 .. 2025-10-31 (denser
-- towards the end), 0..8 items per order (mean 3.2), 1..6 metrics (always "overall"), 0..3 notes, 2% soft-deleted, 70% of the orders
-- with normalized items (attributes.normalized = true; the other 30% carry lower case categories and upper case kinds that
-- categories_map and kinds_map map).
-- order_by is the physical order of the heap: empty = by id (insertion order), "ORDER BY p.tenant_id, p.placed_at, g.id" = clustered.
-- Run in one transaction with schema.sql (-1): with wal_level = minimal the load of tables created in it writes no WAL.
\set ON_ERROR_STOP on
SET work_mem = '256MB';
SET maintenance_work_mem = '512MB';

CREATE FUNCTION bench_u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

CREATE TEMP TABLE gen_k AS
SELECT ARRAY['web','ios','android','pos','phone'] || ARRAY(SELECT 'marketplace_' || lpad(i::text, 2, '0') FROM generate_series(1, 25) i) AS sources,
       ARRAY['mhead','m_speed','packaging','support','value','quality','size_fit','accuracy','returns_ease','checkout','communication','freshness'] AS mkeys,
       ARRAY['Apparel','Shoes','Electronics','Home','Beauty','Toys','Sports','Books','Grocery','Garden','Pet','Office','Auto','Jewelry','Baby'] AS cats,
       ARRAY['standard','express','pickup','digital','subscription','preorder','bundle','gift'] AS kinds,
       ARRAY['en','de','fr','es','it','pt','nl','ru','ja','zh','sv','pl'] AS langs,
       ARRAY['US','GB','DE','FR','ES','IT','NL','PT','BR','MX','CA','AU','JP','CN','IN','RU','PL','SE','NO','DK',
             'FI','CH','AT','BE','IE','GR','TR','AE','SA','EG','ZA','AR','CL','CO','PE','KR','TH','VN','ID','MY',
             'SG','PH','NZ','CZ','HU','RO','BG','HR','RS','UA','IL','MA','TN','KE','NG','PK','BD','LK','IS','LU'] AS ccs,
       ARRAY['EUR','USD','GBP','BRL','JPY','CAD','AUD','SEK'] AS currencies,
       ARRAY['web','mobile','pos','marketplace'] AS channels,
       ARRAY['desktop','phone','tablet','kiosk'] AS devices,
       ARRAY['ground','express','pickup','courier'] AS shipping,
       ARRAY['card','paypal','transfer','voucher'] AS payment,
       ARRAY['Arrived on time.','Package was damaged.','Please change the delivery address.','Refund issued.',
             'Customer asked for an invoice.','Item out of stock, replaced.','Thanks, all good.','Escalated to the warehouse.'] AS sentences;

INSERT INTO orders (id, tenant_id, customer_id, number, placed_at, status, currency, source, country_code, lang, total, discount,
                    shipping_method, payment_method, attributes, metrics, items, notes, created_at, updated_at, deleted_at)
SELECT g.id, p.tenant_id, p.customer_id, 'R' || lpad(g.id::text, 9, '0'), p.placed_at, p.status, k.currencies[1 + p.ci % 8], p.source, p.cc, p.lang,
       it.total, it.discount, k.shipping[1 + (hashint8extended(g.id, 14) & 3)::int], k.payment[1 + (hashint8extended(g.id, 15) & 3)::int],
       jsonb_build_object('locale', p.lang || '-' || p.cc, 'channel', p.channel, 'device', k.devices[1 + (hashint8extended(g.id, 16) & 3)::int])
         || CASE WHEN p.normalized THEN '{"normalized": true}'::jsonb ELSE '{}'::jsonb END,
       -- metrics: nm keys of the 12, "overall" first, one number each
       (SELECT coalesce(jsonb_object_agg(k.mkeys[CASE WHEN i = 0 THEN 1 ELSE 2 + (p.ms + i) % 11 END],
                                         (round(2 + 8 * power(bench_u(g.id, 100 + i), 0.5)) / 2)::numeric(3,1)), '{}'::jsonb)
          FROM generate_series(0, p.nm - 1) i),
       it.items,
       (SELECT coalesce(jsonb_agg(jsonb_build_object('type', n1.type, 'author_kind', n2.role,
                                                      'text', k.sentences[1 + (hashint8extended(g.id, 650 + j) & 7)::int]) ORDER BY j), '[]'::jsonb)
          FROM generate_series(0, p.nn - 1) j
          CROSS JOIN LATERAL (SELECT CASE WHEN bench_u(g.id, 660 + j) < 0.4 THEN 'comment' WHEN bench_u(g.id, 660 + j) < 0.7 THEN 'reply' ELSE 'internal' END AS type) n1
          CROSS JOIN LATERAL (SELECT CASE n1.type WHEN 'comment' THEN 'customer' WHEN 'internal' THEN 'staff'
                                     ELSE CASE WHEN bench_u(g.id, 670 + j) < 0.8 THEN 'staff' ELSE 'system' END END AS role) n2),
       p.placed_at::timestamp + (hashint8extended(g.id, 23) & 86399) * interval '1 second',
       p.placed_at::timestamp + (hashint8extended(g.id, 23) & 86399) * interval '1 second' + interval '1 hour',
       CASE WHEN p.deleted THEN timestamp '2025-11-01 00:00:00' END
FROM generate_series(1, :n) AS g(id)
CROSS JOIN gen_k k
CROSS JOIN LATERAL (SELECT 1 + floor(power(bench_u(g.id, 1), 2) * :tenants)::int AS tenant_id,
                           1 + floor(power(bench_u(g.id, 29), 1.5) * 300000)::int AS customer_id,
                           k.sources[1 + floor(power(bench_u(g.id, 2), 2) * 30)::int] AS source,
                           date '2022-01-01' + floor(1400 * sqrt(bench_u(g.id, 3)))::int AS placed_at,
                           k.langs[1 + floor(power(bench_u(g.id, 4), 2) * 12)::int] AS lang,
                           1 + floor(power(bench_u(g.id, 5), 2) * 60)::int AS ci,
                           k.channels[1 + floor(power(bench_u(g.id, 31), 1.5) * 4)::int] AS channel,
                           CASE WHEN bench_u(g.id, 30) < 0.85 THEN 'complete' WHEN bench_u(g.id, 30) < 0.91 THEN 'canceled'
                                WHEN bench_u(g.id, 30) < 0.96 THEN 'returned' ELSE 'pending' END AS status,
                           bench_u(g.id, 6) < 0.02 AS deleted,
                           bench_u(g.id, 12) < 0.7 AS normalized,
                           floor(power(bench_u(g.id, 9), 1.5) * 9)::int AS ni,
                           1 + floor(bench_u(g.id, 7) * 6)::int AS nm,
                           floor(bench_u(g.id, 8) * 12)::int AS ms,
                           floor(power(bench_u(g.id, 32), 2) * 4)::int AS nn) p0
CROSS JOIN LATERAL (SELECT p0.*, k.ccs[p0.ci] AS cc) p
-- items: ni objects, the order's total and discount are their sums
CROSS JOIN LATERAL (
  SELECT coalesce(jsonb_agg(jsonb_build_object('category', CASE WHEN p.normalized THEN a.cat ELSE lower(a.cat) END, 'sku', a.sku,
                                               'kind', CASE WHEN p.normalized THEN a.kind ELSE upper(a.kind) END,
                                               'qty', a.qty, 'price', a.price, 'discount', b.discount) ORDER BY j), '[]'::jsonb) AS items,
         coalesce(sum(a.qty * (a.price - b.discount)), 0) AS total,
         coalesce(sum(a.qty * b.discount), 0) AS discount
  FROM generate_series(0, p.ni - 1) j
  CROSS JOIN LATERAL (SELECT k.cats[1 + floor(power(bench_u(g.id, 200 + j), 2) * 15)::int] AS cat,
                             k.kinds[1 + floor(power(bench_u(g.id, 300 + j), 1.5) * 8)::int] AS kind,
                             'SKU-' || lpad((1 + floor(power(bench_u(g.id, 500 + j), 2) * 20000)::int)::text, 6, '0') AS sku,
                             1 + floor(power(bench_u(g.id, 600 + j), 3) * 5)::int AS qty,
                             (round(4 * (5 + 495 * power(bench_u(g.id, 400 + j), 3))) / 4)::numeric(8,2) AS price) a
  CROSS JOIN LATERAL (SELECT CASE WHEN bench_u(g.id, 700 + j) < 0.3
                                  THEN (round(4 * a.price * (1 + floor(bench_u(g.id, 800 + j) * 3)) / 10) / 4)::numeric(8,2)
                                  ELSE 0::numeric(8,2) END AS discount) b) it
:order_by;

-- metrics: the mapping of the keys of each source (about one in seven has none: the split of the key is the fallback)
INSERT INTO metrics_map
SELECT s, key, initcap(split_part(key, '_', 1)), nullif(initcap(split_part(key, '_', 2)), '')
FROM gen_k, unnest(sources) s, unnest(mkeys) key
WHERE hashtextextended(s || ':' || key, 31) % 7 <> 0;

-- items of an order that is not normalized: lower case category and upper case kind -> canonical; auto, pet and baby have no category of
-- their own, a subscription no kind
INSERT INTO categories_map
SELECT lower(c), CASE WHEN lower(c) IN ('auto','pet','baby') THEN '' WHEN lower(c) = 'jewelry' THEN 'Apparel' ELSE c END
FROM gen_k, unnest(cats) c;

INSERT INTO kinds_map
SELECT upper(x), CASE x WHEN 'preorder' THEN 'standard' WHEN 'subscription' THEN '' ELSE x END
FROM gen_k, unnest(kinds) x;

DROP TABLE gen_k;
DROP FUNCTION bench_u(bigint, int);
