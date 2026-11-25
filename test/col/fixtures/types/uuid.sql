-- a key, equality and count distinct of uuid with the all-zero and all-one values
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT u, count(*) AS n FROM t_misc GROUP BY u ORDER BY u
