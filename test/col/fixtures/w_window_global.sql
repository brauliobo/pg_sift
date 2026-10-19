-- window: over the single row of an aggregate with no GROUP BY, and over an empty input
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum(total) AS revenue, count(*) OVER () AS frames, sum(count(*)) OVER () AS again FROM orders WHERE id < 0 OR store_id < 40
