-- The member lists of the queries: K10, K400 and K4000 are hash buckets of the stores that have orders (K10 is inside K400, which is
-- inside K4000: 4000, 100 and 10 divide each other, so a bigger set repeats the work of the smaller one); sources lists the sources.
\set ON_ERROR_STOP on
INSERT INTO analytics_sets
SELECT s.name, p.store_id::text
FROM (SELECT DISTINCT store_id FROM orders) p
JOIN (VALUES ('K10', 4000), ('K400', 100), ('K4000', 10)) s(name, m) ON hashint8extended(p.store_id, 77) % s.m = 0;
INSERT INTO analytics_sets SELECT DISTINCT 'sources', source FROM orders;
CREATE INDEX analytics_sets_idx ON analytics_sets (set_name);
