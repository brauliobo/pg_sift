-- trap: an aggregate that shows a float as text or in an array keeps the normal plan (the index holds -0 as 0, the sign of a zero would show)
-- shape: trap
-- expect: plain
-- key: -
SELECT grp, (SELECT array_agg(v ORDER BY v::text) FROM unnest(a) v) AS a, (SELECT string_agg(v, ',' ORDER BY v) FROM unnest(string_to_array(s, ',')) v) AS s
FROM (SELECT grp, array_agg(f8) AS a, string_agg(f4::text, ',') AS s FROM t_float GROUP BY grp) q
ORDER BY grp
