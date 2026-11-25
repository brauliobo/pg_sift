-- DISTINCT ON over wide leaves (numeric of 38 digits, interval): their values go through a dictionary
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (grp) grp, id, qty, n38, n19, iv FROM t_wide ORDER BY grp, qty DESC NULLS LAST, id
