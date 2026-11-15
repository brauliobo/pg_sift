-- query: accounts-by-tier-one-to-one
-- kind: group_by_dim_attr
-- join: inner
SELECT p.tier, count(*) AS accounts, sum(p.bio_len) AS bio FROM account a JOIN profile p ON p.account_id = a.id GROUP BY p.tier;

-- query: accounts-without-profile
-- kind: anti_join_count
-- join: left
SELECT a.plan, count(*) AS accounts FROM account a LEFT JOIN profile p ON p.account_id = a.id WHERE p.account_id IS NULL GROUP BY a.plan;

-- query: activity-by-tier-chain
-- kind: chain
-- join: inner
SELECT p.tier, a.plan, count(*) AS n, sum(x.units) AS units FROM activity x JOIN account a ON a.id = x.account_id JOIN profile p ON p.account_id = a.id GROUP BY p.tier, a.plan;

-- query: activity-all-accounts-left
-- kind: group_by_dim_attr
-- join: left
SELECT p.tier, count(*) AS n, count(p.account_id) AS profiled FROM activity x JOIN account a ON a.id = x.account_id LEFT JOIN profile p ON p.account_id = a.id GROUP BY p.tier;

-- query: distinct-accounts-by-locale
-- kind: count_distinct_across_join
-- join: inner
SELECT p.locale, count(DISTINCT x.account_id) AS accounts FROM activity x JOIN profile p ON p.account_id = x.account_id GROUP BY p.locale;

-- query: profile-and-plan-filters
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT a.region, count(*) FILTER (WHERE p.tier = 4) AS tier4, sum(x.units) FILTER (WHERE a.plan = 'w1') AS plan1_units, count(*) AS n
FROM activity x JOIN account a ON a.id = x.account_id JOIN profile p ON p.account_id = a.id GROUP BY a.region;

-- query: top-accounts-with-long-bio
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT a.id, p.tier, sum(x.units) AS units FROM activity x JOIN account a ON a.id = x.account_id JOIN profile p ON p.account_id = a.id WHERE p.bio_len > 1500 GROUP BY a.id, p.tier ORDER BY units DESC, a.id LIMIT 10;

-- query: profiles-without-account-full
-- kind: anti_join_count
-- join: full
SELECT count(*) FILTER (WHERE p.account_id IS NULL) AS no_profile, count(*) FILTER (WHERE a.id IS NULL) AS no_account, count(*) AS rows FROM account a FULL JOIN profile p ON p.account_id = a.id;
