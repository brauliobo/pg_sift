-- preagg: a global aggregate with a comparison on the zone
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum(total) AS revenue, min(placed_at) AS first_day FROM orders WHERE id <= 2000
