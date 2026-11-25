-- bit and varbit conjuncts and expressions: equality is by length and bits, & and bit_count on the leaf
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE b = B'00000001') AS eq, count(*) FILTER (WHERE b < B'00001111') AS lt, count(*) FILTER (WHERE vb = B'0') AS vb0, count(*) FILTER (WHERE vb = B'00') AS vb00, count(*) FILTER (WHERE vb > B'0001') AS gt, count(*) FILTER (WHERE vb IN (B'', B'1')) AS in_, count(DISTINCT b & B'00001111') AS masked, sum(bit_count(b)) AS bits, max(length(vb)) AS maxlen, count(*) FILTER (WHERE vb IS NULL) AS nulls FROM t_net
