-- min and max of values equal in different spellings: the later row of PostgreSQL's own function
-- shape: expr
-- expect: agg
-- key: -
SELECT max(nimg)::text AS mx, min(nimg)::text AS mn, max(iv)::text AS mx_iv, min(iv)::text AS mn_iv FROM t_wide WHERE id BETWEEN 7 AND 10 OR id IN (4, 5)
