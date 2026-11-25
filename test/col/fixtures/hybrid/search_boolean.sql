-- AND, OR and NOT of terms with a filter and a group by two columns
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT status, country_code, count(*) AS n, count(DISTINCT tenant_id) AS tenants
FROM orders WHERE memo @@> '(damaged OR broken) AND NOT refund' AND deleted_at IS NULL
GROUP BY status, country_code ORDER BY status, country_code
