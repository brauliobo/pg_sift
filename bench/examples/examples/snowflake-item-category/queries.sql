-- query: revenue-by-department
-- kind: chain
-- join: inner
SELECT d.name, count(*) AS n, sum(s.amount) AS amount
FROM sale s JOIN item i ON i.id = s.item_id JOIN category c ON c.id = i.category_id JOIN department d ON d.id = c.department_id GROUP BY d.name;

-- query: revenue-by-division-filtered
-- kind: filter_dim_attr
-- join: inner
SELECT c.kind, sum(s.qty) AS qty FROM sale s JOIN item i ON i.id = s.item_id JOIN category c ON c.id = i.category_id JOIN department d ON d.id = c.department_id
WHERE d.division IN ('w1', 'w2', 'w3') GROUP BY c.kind;

-- query: distinct-items-sold-by-kind
-- kind: count_distinct_across_join
-- join: inner
SELECT c.kind, count(DISTINCT s.item_id) AS items FROM sale s JOIN item i ON i.id = s.item_id JOIN category c ON c.id = i.category_id GROUP BY c.kind;

-- query: brand-by-department-snowflake
-- kind: snowflake
-- join: inner
SELECT d.division, i.brand, count(*) AS n FROM sale s JOIN item i ON i.id = s.item_id JOIN category c ON c.id = i.category_id JOIN department d ON d.id = c.department_id
WHERE c.kind IN ('w1', 'w2', 'w3') GROUP BY d.division, i.brand;

-- query: kind-mix-by-department-filter
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT d.name, sum(s.amount) FILTER (WHERE c.kind = 'w1') AS kind1, sum(s.amount) FILTER (WHERE d.division IN ('w2', 'w3')) AS div2, count(*) AS n
FROM sale s JOIN item i ON i.id = s.item_id JOIN category c ON c.id = i.category_id JOIN department d ON d.id = c.department_id GROUP BY d.name;

-- query: top-categories-by-qty
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT c.id, c.kind, sum(s.qty) AS qty FROM sale s JOIN item i ON i.id = s.item_id JOIN category c ON c.id = i.category_id GROUP BY c.id, c.kind ORDER BY qty DESC, c.id LIMIT 10;

-- query: items-never-sold
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS unsold FROM item i WHERE NOT EXISTS (SELECT 1 FROM sale s WHERE s.item_id = i.id);

-- query: sales-per-average-price
-- kind: sum_avg
-- join: inner
SELECT i.brand, round(avg(i.price), 4) AS avg_price, sum(s.amount) AS amount FROM sale s JOIN item i ON i.id = s.item_id GROUP BY i.brand;
