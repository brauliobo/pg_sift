-- a number of 400 digits, one of 300 decimals, and 39 digits: past what a lane pair holds
-- shape: trap
-- expect: agg
-- key: -
SELECT 'gt_400' AS op, count(*) AS n FROM t_wide WHERE nimg > 10::numeric ^ 399 UNION ALL SELECT 'lt_400', count(*) FROM t_wide WHERE nimg < -(10::numeric ^ 399) UNION ALL SELECT 'eq_400', count(*) FROM t_wide WHERE nimg = 10::numeric ^ 400 + 1 UNION ALL SELECT 'tiny_gt', count(*) FROM t_wide WHERE nimg > 1 / 10::numeric ^ 299 UNION ALL SELECT 'tiny_eq', count(*) FROM t_wide WHERE nimg = 1 / 10::numeric ^ 300 UNION ALL SELECT 'eq_39', count(*) FROM t_wide WHERE nimg = 123456789012345678901234567890123456789.123456789 UNION ALL SELECT 'gt_39', count(*) FROM t_wide WHERE nimg > 12345678901234567890.123456789 ORDER BY 1
