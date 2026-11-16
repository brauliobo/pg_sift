-- query: amount-by-plan
-- kind: group_by_dim_attr
-- join: inner
SELECT a.plan, count(*) AS n, sum(e.amount) AS amount FROM event e JOIN account a ON a.id = e.account_id GROUP BY a.plan;

-- query: events-of-region-plan
-- kind: filter_dim_attr
-- join: inner
SELECT a.region, count(*) AS n FROM event e JOIN account a ON a.id = e.account_id WHERE a.plan = 'w1' AND a.region IN ('r1', 'r2') GROUP BY a.region;

-- query: distinct-accounts-by-region
-- kind: count_distinct_across_join
-- join: inner
SELECT a.region, count(DISTINCT e.account_id) AS accounts FROM event e JOIN account a ON a.id = e.account_id GROUP BY a.region;

-- query: accounts-without-events
-- kind: anti_join_count
-- join: anti
SELECT a.plan, count(*) AS idle FROM account a WHERE NOT EXISTS (SELECT 1 FROM event e WHERE e.account_id = a.id) GROUP BY a.plan;

-- query: top-accounts-by-amount
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT a.n, a.plan, sum(e.amount) AS amount FROM event e JOIN account a ON a.id = e.account_id GROUP BY a.n, a.plan ORDER BY amount DESC, a.n LIMIT 10;

-- query: plan-mix-filters
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT a.region, count(*) FILTER (WHERE a.plan = 'w1') AS plan1, sum(e.amount) FILTER (WHERE a.plan IN ('w2', 'w3')) AS amount23, count(*) AS n FROM event e JOIN account a ON a.id = e.account_id GROUP BY a.region;

-- query: events-by-plan-left
-- kind: group_by_dim_attr
-- join: left
SELECT a.plan, count(*) AS n, count(a.id) AS matched FROM event e LEFT JOIN account a ON a.id = e.account_id GROUP BY a.plan;
