-- bit and varbit have no hash operator class, so PostgreSQL groups them by sorting and the pushdown leaves the plan alone
-- shape: expr
-- expect: plain
-- key: -
SELECT b, vb, count(*) AS n FROM t_net GROUP BY b, vb ORDER BY b, vb
