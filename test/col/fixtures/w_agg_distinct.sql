-- count(DISTINCT) of the wide leaves
-- shape: expr
-- expect: agg
-- key: -
SELECT count(DISTINCT n38) AS a, count(DISTINCT n19) AS b, count(DISTINCT nimg) AS c, count(DISTINCT m) AS d, count(DISTINCT iv) AS e, count(DISTINCT nneg) AS f FROM t_wide
