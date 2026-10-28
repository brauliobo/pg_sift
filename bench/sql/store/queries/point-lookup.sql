-- Find one order by its id
-- order: 1
-- key: 1
-- label: `* WHERE id = 42`
--@ pg
SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM orders WHERE id = {point_id}
--@ ch
SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM store.orders WHERE id = {point_id}
--@ es
GET /orders/_doc/{point_id}
rows: _source -> id customer_id country category status amount quantity created_at title
