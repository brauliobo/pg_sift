-- an equality on a text column finds its dictionary code without testing every string
-- label: `WHERE title = '<one title>'`, `count`, `sum`
--@ pg
SELECT count(*), sum(amount) FROM orders WHERE title = 'wireless compact charger'
