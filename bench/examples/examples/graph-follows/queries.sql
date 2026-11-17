-- query: followers-by-followee-country
-- kind: group_by_dim_attr
-- join: inner
SELECT p.country, count(*) AS edges, count(DISTINCT f.follower) AS followers FROM follows f JOIN person p ON p.id = f.followee GROUP BY p.country;

-- query: cross-country-edges
-- kind: filter_dim_attr
-- join: inner
SELECT a.country AS from_country, b.country AS to_country, count(*) AS edges FROM follows f JOIN person a ON a.id = f.follower JOIN person b ON b.id = f.followee
WHERE a.country <> b.country AND b.tier = 4 GROUP BY a.country, b.country;

-- query: mutual-follow-pairs
-- kind: graph_edges
-- join: inner
SELECT count(*) AS pairs FROM follows a JOIN follows b ON b.follower = a.followee AND b.followee = a.follower AND a.follower < a.followee;

-- query: mutual-pairs-by-country
-- kind: self_join
-- join: inner
SELECT p.country, count(DISTINCT a.id) AS edges FROM follows a JOIN follows b ON b.follower = a.followee AND b.followee = a.follower JOIN person p ON p.id = a.follower GROUP BY p.country;

-- query: people-without-followers
-- kind: anti_join_count
-- join: anti
SELECT p.tier, count(*) AS people FROM person p WHERE NOT EXISTS (SELECT 1 FROM follows f WHERE f.followee = p.id) GROUP BY p.tier;

-- query: top-followed-people
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT p.id, p.country, count(*) AS followers FROM follows f JOIN person p ON p.id = f.followee GROUP BY p.id, p.country ORDER BY followers DESC, p.id LIMIT 10;

-- query: friends-of-friends-of-one
-- kind: graph_edges
-- join: inner
SELECT count(*) AS paths, count(DISTINCT b.followee) AS reached FROM follows a JOIN follows b ON b.follower = a.followee WHERE a.follower = 1;

-- query: tier-mix-of-followees
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT a.tier, count(*) FILTER (WHERE b.tier = 1) AS to_tier1, count(*) FILTER (WHERE b.country = a.country) AS same_country, count(*) AS edges
FROM follows f JOIN person a ON a.id = f.follower JOIN person b ON b.id = f.followee GROUP BY a.tier;
