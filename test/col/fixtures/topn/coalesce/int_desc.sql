-- ORDER BY coalesce(int column, constant) DESC, and NULLS FIRST on the expression (a coalesce is never NULL: the clause is moot)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, customer_id FROM orders ORDER BY coalesce(customer_id, 500) DESC NULLS FIRST, id DESC LIMIT 12
