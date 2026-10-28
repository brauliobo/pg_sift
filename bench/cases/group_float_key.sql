-- GROUP BY a float8 key whose values have a short mantissa (halves): the key table slot is taken from the top bits of the hash
-- label: `count(*), sum(amount) GROUP BY score` over a `float8` of 1,000 values
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN score float8;
UPDATE orders SET score = (id % 1000) * 0.5;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, score)
--@ pg
SELECT score, count(*) AS n, sum(amount) AS total FROM orders GROUP BY 1 ORDER BY 1
