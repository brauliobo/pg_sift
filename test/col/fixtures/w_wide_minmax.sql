-- min and max of money, interval, a lane pair and an image
-- shape: expr
-- expect: agg
-- key: -
SELECT min(m) AS mn_m, max(m) AS mx_m, min(iv) FILTER (WHERE iv < 'infinity') AS mn_iv, max(iv) AS mx_iv, min(n38) AS mn_38, max(n38) AS mx_38, max(nimg) AS mx_img, min(nimg) AS mn_img FROM t_wide
