-- query: item-revenue-by-category
-- kind: jsonb_attr
-- join: inner
SELECT c.category, count(*) AS items, sum(x.qty * c.price) AS revenue FROM doc d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS x(sku int, qty int) JOIN catalog c ON c.sku = x.sku GROUP BY c.category;

-- query: items-by-channel-and-brand
-- kind: filter_dim_attr
-- join: inner
SELECT d.channel, c.brand, count(*) AS items FROM doc d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS x(sku int, qty int) JOIN catalog c ON c.sku = x.sku WHERE c.category = 'w1' GROUP BY d.channel, c.brand;

-- query: distinct-docs-by-category
-- kind: count_distinct_across_join
-- join: inner
SELECT c.category, count(DISTINCT d.id) AS docs, count(DISTINCT c.sku) AS skus FROM doc d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS x(sku int, qty int) JOIN catalog c ON c.sku = x.sku GROUP BY c.category;

-- query: unknown-sku-items
-- kind: anti_join_count
-- join: left
SELECT d.channel, count(*) AS unknown FROM doc d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS x(sku int, qty int) LEFT JOIN catalog c ON c.sku = x.sku WHERE c.sku IS NULL GROUP BY d.channel;

-- query: skus-never-in-a-doc
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS unsold FROM catalog c WHERE c.sku NOT IN (SELECT x.sku FROM doc d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS x(sku int, qty int));

-- query: docs-with-a-category-item
-- kind: exists_scope
-- join: semi
-- oracle: SELECT d.channel, count(*) AS docs FROM doc d WHERE d.id IN (SELECT d2.id FROM doc d2 CROSS JOIN LATERAL jsonb_to_recordset(d2.items) AS x(sku int, qty int) JOIN catalog c ON c.sku = x.sku WHERE c.category = 'w2' AND x.qty >= 5) GROUP BY d.channel
SELECT d.channel, count(*) AS docs FROM doc d WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS x(sku int, qty int) JOIN catalog c ON c.sku = x.sku WHERE c.category = 'w2' AND x.qty >= 5) GROUP BY d.channel;

-- query: top-skus-by-quantity
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT c.sku, c.brand, sum(x.qty) AS qty FROM doc d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS x(sku int, qty int) JOIN catalog c ON c.sku = x.sku GROUP BY c.sku, c.brand ORDER BY qty DESC, c.sku LIMIT 10;

-- query: quantity-mix-by-brand
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT c.brand, sum(x.qty) FILTER (WHERE d.channel = 'ch1') AS qty_ch1, sum(x.qty) FILTER (WHERE c.category IN ('w1', 'w2')) AS qty12, count(*) AS items
FROM doc d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS x(sku int, qty int) JOIN catalog c ON c.sku = x.sku GROUP BY c.brand;
