-- query: usage-by-tier-in-big-tenant
-- kind: group_by_dim_attr
-- join: inner
SELECT a.tier, count(*) AS n, sum(u.units) AS units FROM usage u JOIN account a ON a.tenant_id = u.tenant_id AND a.id = u.account_id WHERE u.tenant_id = 1 GROUP BY a.tier;

-- query: usage-by-tenant-plan
-- kind: chain
-- join: inner
SELECT t.plan, a.status, count(*) AS n, sum(u.units) AS units FROM usage u JOIN account a ON a.tenant_id = u.tenant_id AND a.id = u.account_id JOIN tenant t ON t.id = a.tenant_id GROUP BY t.plan, a.status;

-- query: usage-of-subscribed-accounts
-- kind: exists_scope
-- join: semi
SELECT u.tenant_id, count(*) AS n, count(DISTINCT u.account_id) AS accounts FROM usage u
WHERE u.tenant_id <= 5 AND EXISTS (SELECT 1 FROM grant_row s WHERE s.tenant_id = u.tenant_id AND s.account_id = u.account_id AND s.product = 'p2' AND s.active) GROUP BY u.tenant_id;

-- query: accounts-without-usage
-- kind: anti_join_count
-- join: anti
SELECT a.status, count(*) AS idle FROM account a WHERE NOT EXISTS (SELECT 1 FROM usage u WHERE u.tenant_id = a.tenant_id AND u.account_id = a.id) GROUP BY a.status;

-- query: distinct-accounts-by-region
-- kind: count_distinct_across_join
-- join: inner
SELECT t.region, count(DISTINCT u.account_id) AS accounts, count(DISTINCT u.tenant_id) AS tenants FROM usage u JOIN tenant t ON t.id = u.tenant_id GROUP BY t.region;

-- query: top-accounts-in-tenant
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT u.account_id, a.tier, sum(u.units) AS units FROM usage u JOIN account a ON a.tenant_id = u.tenant_id AND a.id = u.account_id WHERE u.tenant_id = 1 GROUP BY u.account_id, a.tier ORDER BY units DESC, u.account_id LIMIT 10;

-- query: units-by-status-filters
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT t.plan, sum(u.units) FILTER (WHERE a.status = 'active') AS active, sum(u.units) FILTER (WHERE a.tier >= 3) AS high_tier, count(*) AS n
FROM usage u JOIN account a ON a.tenant_id = u.tenant_id AND a.id = u.account_id JOIN tenant t ON t.id = u.tenant_id GROUP BY t.plan;

-- query: grants-by-product-and-plan
-- kind: group_by_dim_attr
-- join: inner
SELECT s.product, t.plan, count(*) AS grants, count(*) FILTER (WHERE s.active) AS active FROM grant_row s JOIN tenant t ON t.id = s.tenant_id GROUP BY s.product, t.plan;

-- query: usage-four-dimension-star
-- kind: star
-- join: inner
SELECT t.region, a.tier, s.product, count(*) AS n, sum(u.units) AS units FROM usage u JOIN account a ON a.tenant_id = u.tenant_id AND a.id = u.account_id
JOIN tenant t ON t.id = u.tenant_id JOIN grant_row s ON s.tenant_id = u.tenant_id AND s.account_id = u.account_id AND s.product = 'p1' WHERE s.active GROUP BY t.region, a.tier, s.product;
