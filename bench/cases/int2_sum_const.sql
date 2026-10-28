-- sum and avg of a smallint column plus, minus or times an integer constant: the sums of the units answer them
-- label: `sum(quantity + 1)`, `avg(quantity - 2)`, `sum(quantity * 3)`
--@ pg
SELECT sum(quantity + 1) AS s1, sum(quantity + 2) AS s2, sum(quantity + 3) AS s3, avg(quantity - 2) AS a1, sum(quantity * 3) AS m3, sum(quantity - 10) AS s4 FROM orders
