-- a grouping by a constant alone over no row has no group; with another key it has none either
-- shape: flat
-- expect: plain
-- key: -
-- scenario: yes
SELECT 'x'::text AS label, count(*) FROM orders WHERE id < 0 GROUP BY 1
