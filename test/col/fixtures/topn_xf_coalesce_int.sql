-- ORDER BY coalesce(int column, constant): the NULLs sort as the constant, in the middle of the values; ties on the constant fall to the next key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, customer_id FROM orders ORDER BY coalesce(customer_id, 500), id LIMIT 12
