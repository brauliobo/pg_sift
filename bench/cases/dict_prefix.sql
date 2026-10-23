-- LIKE 'abc%' on a text column finds its dictionary codes from a byte-order permutation of the dictionary
-- label: `WHERE title LIKE 'wireless c%'`, `count`, `sum`
--@ pg
SELECT count(*), sum(amount) FROM orders WHERE title LIKE 'wireless c%'
