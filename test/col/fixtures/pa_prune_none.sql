-- a range no partition can hold: pruning leaves none, the query has its one global row
-- shape: part
-- expect: any
-- key: -
SELECT count(*) AS n, sum(qty) AS q FROM t_part WHERE d >= DATE '2024-05-01' AND d < DATE '2024-04-01'
