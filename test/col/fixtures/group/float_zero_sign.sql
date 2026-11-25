-- floats shown as text or in arrays keep the sign of a zero: the index holds -0 bit for bit
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, array_agg(f8 ORDER BY id) AS a8, string_agg(f4::text, ',' ORDER BY id) AS s4, jsonb_agg(f8 ORDER BY id) AS j8, array_agg(DISTINCT f8::text) AS d8, min(f8::text) AS lo
FROM t_float GROUP BY grp ORDER BY grp
