-- generic array_agg, string_agg, json_agg and jsonb_agg without ORDER BY take the rows in the order of the scan; the outer query sorts what they collect
-- shape: flat
-- expect: agg
-- key: -
SELECT source, n, (SELECT array_agg(v ORDER BY v) FROM unnest(ids) v) AS ids, (SELECT string_agg(v, ',' ORDER BY v) FROM unnest(string_to_array(ccs, ',')) v) AS ccs,
       (SELECT jsonb_agg(v ORDER BY v) FROM jsonb_array_elements(js) v) AS js, (SELECT json_agg(v ORDER BY v) FROM json_array_elements_text(jd) v) AS jd,
       (SELECT array_agg(v ORDER BY v) FROM unnest(tot) v) AS tot
FROM (SELECT source, count(*) AS n, array_agg(tenant_id) AS ids, string_agg(country_code, ',') AS ccs, jsonb_agg(status) AS js, json_agg(placed_at) AS jd,
             array_agg(total) FILTER (WHERE total > 400) AS tot
      FROM orders WHERE tenant_id < 20 GROUP BY source) q
ORDER BY source
