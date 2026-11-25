-- DISTINCT over more than one varying argument, and DISTINCT with ORDER BY: a tuplesort on the DISTINCT keys, equal inputs dropped (NULLs equal)
-- shape: flat
-- expect: agg
-- key: -
SELECT source, string_agg(DISTINCT country_code, lang ORDER BY country_code DESC) AS sep_lang, array_agg(DISTINCT lang ORDER BY lang DESC NULLS FIRST) AS langs,
       regr_count(DISTINCT total::float8, tenant_id::float8) AS pairs, string_agg(DISTINCT status, ',' ORDER BY status) AS sts,
       jsonb_object_agg(DISTINCT lang, country_code) FILTER (WHERE lang IS NOT NULL) AS lc, string_agg(DISTINCT 'x', ',') AS konst
FROM orders WHERE tenant_id < 25 GROUP BY source ORDER BY source
