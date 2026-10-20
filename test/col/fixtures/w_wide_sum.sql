-- sum and avg of a numeric of two lanes and of money
-- shape: expr
-- expect: agg
-- key: -
SELECT sum(n38) AS s38, sum(n19) AS s19, sum(m) AS sm, avg(n19) AS a19 FROM t_wide WHERE n38 < 1e20 AND n38 <> 'NaN' AND n19 < 1e15 AND n19 <> 'NaN' AND m BETWEEN '-1000000' AND '1000000'
