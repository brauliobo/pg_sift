-- sum and avg of the other types of a lookup join's columns (int2, int4, int8, float4, interval): PostgreSQL's own transition and final functions on the distinct values
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, sum(m.i2) AS s2, avg(m.i2) AS a2, sum(m.i4) AS s4, avg(m.i4) AS a4, sum(m.i8) AS s8, avg(m.i8) AS a8, avg(m.f4) AS af,
       sum(m.iv) AS si, avg(m.iv) AS ai, sum(m.i4 * i.qty) AS sq, avg(m.i8 + i.qty) AS aq, max(m.iv) AS hi, count(m.i4) AS c, count(*) AS n
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_nums m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY i.category ORDER BY i.category NULLS FIRST
