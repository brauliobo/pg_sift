-- The orders dataset of the analytics benchmark (bench/steps/store.sh): `orders_src`, one row per order, in the order of id.
--
--   psql -v n=1000000 -f gen.sql
--
-- Every draw is a hash of (order id, draw number): the same n gives the same rows on every run (hashint8extended is what hash
-- partitioning uses). The rows are written to a CSV file once and that file is what every system loads.
-- Shape: 30 countries, 40 categories, 5 statuses, 1 to 5 items per order, prices of $2.99 to $299.99, three years of orders
-- (2023-01-01 to 2025-12-31, more orders towards the end) and a product title of two adjectives and a noun. The nouns come in groups of three
-- per category; the first ones sell the most, an order sits in the category of its noun 60% of the time and in any other category
-- otherwise (gifts, bundles). Customers: n / 5, a few of them order a lot.
\set ON_ERROR_STOP on
SET work_mem = '256MB';
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

DROP TABLE IF EXISTS orders_src;
CREATE UNLOGGED TABLE orders_src AS
WITH v AS (SELECT
  ARRAY['Electronics','Audio','Computers','Phones','Cameras','Gaming','Home','Kitchen','Furniture','Garden',
        'Tools','Lighting','Apparel','Shoes','Bags','Jewelry','Watches','Beauty','Health','Fitness',
        'Sports','Outdoors','Toys','Baby','Pets','Books','Office','Stationery','Auto','Bikes',
        'Grocery','Drinks','Music','Movies','Art','Crafts','Travel','Safety','Cleaning','Gifts'] AS cats,
  ARRAY['US','GB','DE','FR','ES','IT','NL','BR','CA','AU','JP','IN','MX','SE','PL','PT','AT','BE','CH','IE',
        'NO','DK','FI','NZ','AR','CL','CZ','GR','RO','TR'] AS ccs,
  ARRAY['wireless','portable','compact','premium','smart','classic','ultra','mini','heavy','waterproof',
        'foldable','rechargeable','vintage','modern','organic','ergonomic','adjustable','digital','leather','stainless',
        'cotton','wooden','silicone','magnetic','solar','quiet','durable','lightweight','professional','deluxe',
        'black','white','red','blue','green','silver','large','small','folding','travel'] AS adjs,
  ARRAY['charger','speaker','laptop',  'headphones','earbuds','microphone',  'laptop stand','monitor','keyboard',  'phone case','tablet','smartwatch',
        'tripod','camera bag','lens',  'controller','headset','console',  'vacuum','lamp','blender',  'kettle','toaster','cookware',
        'sofa','desk','shelf',  'hose','planter','mower',  'drill','wrench','saw',  'bulb','lantern','strip',
        'jacket','shirt','jeans',  'sneakers','boots','sandals',  'backpack','wallet','suitcase',  'necklace','ring','bracelet',
        'watch','strap','dial',  'lotion','perfume','brush',  'vitamins','monitor cuff','scale',  'dumbbell','yoga mat','bike pump',
        'ball','racket','helmet',  'tent','sleeping bag','compass',  'puzzle','doll','blocks',  'stroller','crib','bottle',
        'leash','pet bed','collar',  'novel','cookbook','atlas',  'chair','organizer','shredder',  'notebook','pen set','stapler',
        'tire','wiper','car cover',  'frame','pedal','lock',  'coffee','tea','snack box',  'water','juice','soda',
        'guitar','drum','piano',  'poster','dvd','projector',  'canvas','easel','paint set',  'yarn','beads','glue',
        'passport holder','adapter','map',  'extinguisher','vest','gloves',  'detergent','mop','sponge',  'gift card','gift basket','candle'] AS nouns)
SELECT g.id,
       1 + floor(c.u * c.u * ncust)::int AS customer_id,
       v.ccs[1 + floor(co.u * co.u * 30)::int] AS country,
       v.cats[CASE WHEN ca.u < 0.6 THEN 1 + (n.i - 1) / 3 ELSE 1 + floor(ca.u2 * 40)::int END] AS category,
       CASE WHEN s.u < 0.05 THEN 'pending' WHEN s.u < 0.15 THEN 'paid' WHEN s.u < 0.70 THEN 'shipped' WHEN s.u < 0.95 THEN 'delivered' ELSE 'cancelled' END AS status,
       (((299 + floor(p.u * p.u * 29700)) * q.q)::numeric / 100)::numeric(10,2) AS amount,
       q.q::smallint AS quantity,
       timestamp '2023-01-01' + floor(t.t * 94608000)::int * interval '1 second' AS created_at,
       v.adjs[a1.i] || ' ' || v.adjs[a2.i] || ' ' || v.nouns[n.i] AS title
FROM generate_series(1, :n) AS g(id)
CROSS JOIN v
CROSS JOIN (SELECT greatest(:n / 5, 1) AS ncust) k
CROSS JOIN LATERAL (SELECT pg_temp.u(g.id, 1) AS u) c
CROSS JOIN LATERAL (SELECT pg_temp.u(g.id, 2) AS u) co
CROSS JOIN LATERAL (SELECT pg_temp.u(g.id, 3) AS u, pg_temp.u(g.id, 4) AS u2) ca
CROSS JOIN LATERAL (SELECT pg_temp.u(g.id, 5) AS u) s
CROSS JOIN LATERAL (SELECT pg_temp.u(g.id, 6) AS u) p
CROSS JOIN LATERAL (SELECT 1 + floor(pg_temp.u(g.id, 7) * pg_temp.u(g.id, 7) * 5)::int AS q) q
CROSS JOIN LATERAL (SELECT 1 - w * (1 + w) / 2 AS t FROM (SELECT 1 - pg_temp.u(g.id, 8) AS w) x) t
CROSS JOIN LATERAL (SELECT 1 + floor(pg_temp.u(g.id, 9) * pg_temp.u(g.id, 9) * 40)::int AS i) a1
CROSS JOIN LATERAL (SELECT 1 + floor(pg_temp.u(g.id, 10) * pg_temp.u(g.id, 10) * 40)::int AS i) a2
CROSS JOIN LATERAL (SELECT 1 + floor(pg_temp.u(g.id, 11) * pg_temp.u(g.id, 11) * 120)::int AS i) n
ORDER BY g.id;
