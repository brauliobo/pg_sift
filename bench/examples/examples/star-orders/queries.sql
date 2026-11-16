-- query: revenue-by-segment
-- kind: group_by_dim_attr
-- join: inner
SELECT c.segment, count(*) AS n, sum(o.amount) AS amount FROM orders o JOIN customer c ON c.id = o.customer_id GROUP BY c.segment;

-- query: paid-orders-in-country
-- kind: filter_dim_attr
-- join: inner
SELECT count(*) AS n, sum(o.amount) AS amount FROM orders o JOIN customer c ON c.id = o.customer_id WHERE c.country = 'c3' AND o.status = 'paid';

-- query: distinct-customers-by-category
-- kind: count_distinct_across_join
-- join: inner
SELECT i.category, count(DISTINCT o.customer_id) AS customers FROM orders o JOIN item i ON i.id = o.item_id GROUP BY i.category;

-- query: three-dimension-star
-- kind: star
-- join: inner
SELECT c.country, i.category, s.region, count(*) AS n, sum(o.qty) AS qty
FROM orders o JOIN customer c ON c.id = o.customer_id JOIN item i ON i.id = o.item_id JOIN store s ON s.id = o.place_id
WHERE c.tier >= 4 AND s.kind = 'k1' GROUP BY c.country, i.category, s.region;

-- query: status-by-tier-filter
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT o.status, count(*) FILTER (WHERE c.tier = 1) AS tier1, count(*) FILTER (WHERE c.tier >= 4) AS tier4,
       sum(o.amount) FILTER (WHERE c.segment = 'w1') AS seg1_amount
FROM orders o JOIN customer c ON c.id = o.customer_id GROUP BY o.status;

-- query: average-order-by-segment
-- kind: sum_avg
-- join: inner
SELECT c.segment, round(avg(o.amount), 4) AS avg_amount, sum(o.qty) AS qty FROM orders o JOIN customer c ON c.id = o.customer_id GROUP BY c.segment;

-- query: top-customers-in-country
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT c.id, c.segment, sum(o.amount) AS amount FROM orders o JOIN customer c ON c.id = o.customer_id WHERE c.country = 'c1'
GROUP BY c.id, c.segment ORDER BY amount DESC, c.id LIMIT 10;

-- query: orders-by-store-region-left
-- kind: group_by_dim_attr
-- join: left
SELECT s.region, count(*) AS n FROM orders o LEFT JOIN store s ON s.id = o.place_id GROUP BY s.region;

-- query: places-by-kind-right
-- kind: group_by_dim_attr
-- join: right
SELECT s.kind, count(o.id) AS orders, count(*) AS rows FROM orders o RIGHT JOIN store s ON s.id = o.place_id GROUP BY s.kind;

-- query: store-match-counts-full
-- kind: anti_join_count
-- join: full
SELECT count(*) FILTER (WHERE o.id IS NULL) AS stores_only, count(*) FILTER (WHERE s.id IS NULL) AS orders_only, count(*) AS rows FROM orders o FULL JOIN store s ON s.id = o.place_id;
