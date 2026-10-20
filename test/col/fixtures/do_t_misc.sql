-- DISTINCT ON over time, enum, uuid and bytea columns
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (grp) grp, id, t, e, u, b FROM t_misc ORDER BY grp, t DESC NULLS LAST, id
