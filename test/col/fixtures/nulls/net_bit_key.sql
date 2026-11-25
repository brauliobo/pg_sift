-- bit and varbit have no hash operator class: a column key is grouped by the image of the value, the lengths of a varbit told apart
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT b, vb, count(*) AS n FROM t_net GROUP BY b, vb ORDER BY b, vb
