-- numeric filters PostgreSQL evaluates: against float8, IS DISTINCT FROM, a division guarded by an earlier condition (in the WHERE and in a FILTER), a modulo, an IN list of 70 constants
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, count(*) AS n, count(*) FILTER (WHERE n2 > 1.5::float8) AS gt_f, count(*) FILTER (WHERE n2 IS DISTINCT FROM 7) AS dist, count(*) FILTER (WHERE n6 <> 0 AND n2 / n6 > 2) AS ratio,
       count(*) FILTER (WHERE n0 % 2 = 1) AS odd, count(*) FILTER (WHERE n2 IN (-67.07, -64.14, -61.21, -58.28, -55.35, -52.42, -49.49, -46.56, -43.63, -40.70, -37.77, -34.84, -31.91, -28.98, -25.05, -22.12, -19.19, -16.26, -13.33, -10.40, -7.47, -4.54, -1.61, 2.68, 5.75, 8.82, 11.89, 14.96, 17.03, 20.10, 23.17, 26.24, 29.31, 32.38, 35.45, 38.52, 41.59, 44.66, 47.73, 50.80, 53.87, 56.94, 59.01, 62.08, 65.15, 68.22, 71.29, 74.36, 77.43, 80.50, 83.57, 86.64, 89.71, 92.78, 95.85, 98.92, 101.99, 104.06, 107.13, 110.20, 113.27, 116.34, 119.41, 122.48, 125.55, 128.62, 131.69, 134.76, 137.83, 140.90)) AS wide_in
FROM t_num
WHERE n2 <> 0 AND 100 / n2 > 1
GROUP BY grp ORDER BY grp
