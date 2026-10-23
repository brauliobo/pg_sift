-- a first sort key that ties between units is decided by the zone of the next key
-- label: `ORDER BY status, customer_id LIMIT 10` (5 statuses)
-- ordered: yes
--@ pg
SELECT status, customer_id FROM orders ORDER BY status, customer_id LIMIT 10
