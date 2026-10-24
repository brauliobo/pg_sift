-- keyset pagination: the page after a (created_at, id) bar, a row comparison with a two-column order
-- label: `WHERE (created_at, id) < (bar) ORDER BY created_at DESC, id DESC LIMIT 50`
-- ordered: yes
--@ pg
SELECT id, amount, created_at FROM orders WHERE (created_at, id) < (TIMESTAMP '2025-06-01 12:00:00', 600000) ORDER BY created_at DESC, id DESC LIMIT 50
