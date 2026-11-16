-- query: matched-amount-by-class
-- kind: group_by_dim_attr
-- join: inner
SELECT e.class, count(*) AS n, sum(f.amount) AS amount FROM fact f JOIN entity e ON e.id = f.target_id GROUP BY e.class;

-- query: all-facts-by-class-left
-- kind: group_by_dim_attr
-- join: left
SELECT e.class, count(*) AS n, count(e.id) AS matched FROM fact f LEFT JOIN entity e ON e.id = f.target_id GROUP BY e.class;

-- query: orphan-facts
-- kind: anti_join_count
-- join: anti
-- plan: SiftAgg
-- found: yes
SELECT count(*) AS orphans, sum(f.amount) AS amount FROM fact f WHERE NOT EXISTS (SELECT 1 FROM entity e WHERE e.id = f.target_id);

-- query: orphans-below-key-limit
-- kind: anti_join_count
-- join: left
SELECT count(*) AS waiting FROM fact f LEFT JOIN entity e ON e.id = f.target_id WHERE e.id IS NULL AND f.target_id <= (SELECT count(*) FROM entity) + 100;

-- query: targets-without-facts
-- kind: anti_join_count
-- join: anti
SELECT e.region, count(*) AS idle FROM entity e WHERE NOT EXISTS (SELECT 1 FROM fact f WHERE f.target_id = e.id) GROUP BY e.region;

-- query: distinct-targets-by-region
-- kind: count_distinct_across_join
-- join: inner
SELECT e.region, count(DISTINCT f.target_id) AS targets FROM fact f JOIN entity e ON e.id = f.target_id GROUP BY e.region;

-- query: class-mix-filters
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT e.region, sum(f.amount) FILTER (WHERE e.class = 'w1') AS class1, count(*) FILTER (WHERE f.at >= '2024-07-01') AS second_half, count(*) AS n FROM fact f JOIN entity e ON e.id = f.target_id GROUP BY e.region;

-- query: top-targets
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT e.id, e.class, sum(f.amount) AS amount FROM fact f JOIN entity e ON e.id = f.target_id GROUP BY e.id, e.class ORDER BY amount DESC, e.id LIMIT 10;

-- query: matched-facts-semi
-- kind: semi_join_count
-- join: semi
-- plan: SiftAgg
-- found: yes
SELECT count(*) AS matched, sum(f.amount) AS amount FROM fact f WHERE EXISTS (SELECT 1 FROM entity e WHERE e.id = f.target_id);

-- query: matched-facts-join-count
-- kind: semi_join_count
-- join: inner
-- plan: SiftAgg
-- found: yes
SELECT count(*) AS matched FROM fact f JOIN entity e ON e.id = f.target_id;
