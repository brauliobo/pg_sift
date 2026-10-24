-- a lookup of the rows of one customer when the heap is in order of arrival and the index cuts its units by the customer: the units of the customer are read, no others
-- label: `count(*), sum(amount) WHERE customer_id = <one customer>`, `key_cut = 'customer_id'`, no btree on the column
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id = 137
