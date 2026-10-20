-- bit(8) and varbit of every length (a few out of line): count distinct and a key of the length, which has a hash
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT length(vb) AS len, count(*) AS n, count(DISTINCT vb) AS d, count(DISTINCT b) AS db FROM t_net GROUP BY length(vb) ORDER BY length(vb)
