-- a FULL JOIN of a join of two tables to a VALUES list keyed by a column of the second
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT cc.region, count(o.id) AS n, count(s.id) AS tenants, count(*) AS tuples FROM orders o JOIN tenants s ON s.id = o.tenant_id FULL JOIN (VALUES ('US', 'north'), ('GB', 'west'), ('XX', 'none')) cc(code, region) ON cc.code = s.country_code GROUP BY cc.region ORDER BY cc.region NULLS FIRST
