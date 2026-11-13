-- query: contacts-by-tier
-- kind: group_by_dim_attr
-- join: inner
SELECT m.tier, count(*) AS contacts FROM contact c JOIN member m ON lower(c.email) = m.email_lc GROUP BY m.tier;

-- query: contacts-in-country-by-channel
-- kind: filter_dim_attr
-- join: inner
SELECT c.channel, count(*) AS contacts FROM contact c JOIN member m ON lower(c.email) = m.email_lc WHERE m.country = 'c3' GROUP BY c.channel;

-- query: unmatched-contacts-left
-- kind: anti_join_count
-- join: left
SELECT count(*) AS unmatched FROM contact c LEFT JOIN member m ON lower(c.email) = m.email_lc WHERE m.id IS NULL;

-- query: distinct-people-by-country
-- kind: count_distinct_across_join
-- join: inner
SELECT m.country, count(DISTINCT m.id) AS people, count(*) AS contacts FROM contact c JOIN member m ON lower(c.email) = m.email_lc GROUP BY m.country;

-- query: people-without-contacts
-- kind: anti_join_count
-- join: anti
SELECT m.tier, count(*) AS silent FROM member m WHERE NOT EXISTS (SELECT 1 FROM contact c WHERE lower(c.email) = m.email_lc) GROUP BY m.tier;

-- query: top-people-by-contacts
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT m.id, m.country, count(*) AS contacts FROM contact c JOIN member m ON lower(c.email) = m.email_lc GROUP BY m.id, m.country ORDER BY contacts DESC, m.id LIMIT 10;

-- query: tier-mix-filters
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT c.channel, count(*) FILTER (WHERE m.tier = 1) AS tier1, count(*) FILTER (WHERE m.country IN ('c1', 'c2')) AS two_countries, count(*) AS n
FROM contact c JOIN member m ON lower(c.email) = m.email_lc GROUP BY c.channel;

-- query: member-email-exact-lookup
-- kind: lookup_exact_key
-- join: inner
SELECT m.id, m.tier, count(*) AS contacts FROM contact c JOIN member m ON lower(c.email) = m.email_lc WHERE lower(c.email) = 'user7@example.com' GROUP BY m.id, m.tier;

-- query: blank-channel-contacts
-- kind: missing_field_count
-- join: inner
SELECT m.country, count(*) FILTER (WHERE c.channel = '') AS blank, count(*) FILTER (WHERE c.channel = 'w1') AS channel1, count(*) AS n FROM contact c JOIN member m ON lower(c.email) = m.email_lc GROUP BY m.country;
