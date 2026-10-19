-- the subscript of a jsonb column (x['k'] is x -> 'k'): a typed path as a cast, as a key (#>> '{}' is its text) and as a filter
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT o.attributes['channel'] #>> '{}' AS channel, o.attributes ->> 'channel' AS plain, count(*) AS n, avg((o.metrics['overall'])::float8) AS overall
FROM orders o
WHERE (o.metrics['overall'])::float8 >= 2 AND o.deleted_at IS NULL
GROUP BY 1, 2 ORDER BY 1 NULLS FIRST
