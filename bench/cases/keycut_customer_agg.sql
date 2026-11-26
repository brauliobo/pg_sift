-- a lookup of the rows of one customer when the heap is in order of arrival and the index partitions its parts by the customer: the parts and granules of the customer are read, no others
-- label: `count(*), sum(amount) WHERE customer_id = <one customer>`, `key_order = 'customer_id'`, no btree on the column
-- gap: sift WS-113-7 (S113-17: no key-order part pruning in V6)
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id = 137
