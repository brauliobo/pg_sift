-- query: revenue-by-category
-- kind: jsonb_attr
-- join: inner
SELECT c.category, count(*) AS n, sum((e.attrs ->> 'qty')::int * c.price) AS revenue FROM event e JOIN catalog c ON c.sku = (e.attrs ->> 'sku')::int GROUP BY c.category;

-- query: channel-and-brand-filter
-- kind: filter_dim_attr
-- join: inner
SELECT c.brand, count(*) AS n FROM event e JOIN catalog c ON c.sku = (e.attrs ->> 'sku')::int WHERE e.attrs ->> 'channel' = 'ch1' AND c.category = 'w1' GROUP BY c.brand;

-- query: containment-filter-by-category
-- kind: jsonb_attr
-- join: inner
SELECT c.category, count(*) AS n FROM event e JOIN catalog c ON c.sku = (e.attrs ->> 'sku')::int WHERE e.attrs @> '{"channel": "ch2"}' GROUP BY c.category;

-- query: distinct-skus-by-category
-- kind: count_distinct_across_join
-- join: inner
SELECT c.category, count(DISTINCT c.sku) AS skus, sum((e.attrs ->> 'qty')::int) AS qty FROM event e JOIN catalog c ON c.sku = (e.attrs ->> 'sku')::int GROUP BY c.category;

-- query: events-with-unknown-sku
-- kind: anti_join_count
-- join: left
SELECT e.attrs ->> 'channel' AS channel, count(*) AS unknown FROM event e LEFT JOIN catalog c ON c.sku = (e.attrs ->> 'sku')::int WHERE c.sku IS NULL GROUP BY 1;

-- query: top-skus-by-revenue
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT c.sku, c.brand, sum((e.attrs ->> 'qty')::int * c.price) AS revenue FROM event e JOIN catalog c ON c.sku = (e.attrs ->> 'sku')::int GROUP BY c.sku, c.brand ORDER BY revenue DESC, c.sku LIMIT 10;

-- query: skus-never-sold
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS unsold FROM catalog c WHERE NOT EXISTS (SELECT 1 FROM event e WHERE (e.attrs ->> 'sku')::int = c.sku);

-- query: channel-mix-by-brand
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT c.brand, count(*) FILTER (WHERE e.attrs ->> 'channel' = 'ch1') AS ch1, sum((e.attrs ->> 'qty')::int) FILTER (WHERE c.category IN ('w1', 'w2')) AS qty12, count(*) AS n
FROM event e JOIN catalog c ON c.sku = (e.attrs ->> 'sku')::int GROUP BY c.brand;
